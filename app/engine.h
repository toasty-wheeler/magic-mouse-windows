// engine.h - MagicMouseNative gesture engine, shared by the tray app and tools\mmreplay.
//
// Input is one decoded touch frame at a time (report 0x29). The Magic Mouse keeps a touch
// slot in its reports after the finger lifts, so everything follows the touch state:
//   0x20 approaching, 0x30 touch-down, 0x40 dragging, 0x50/0x60/0x70 lifting, 0x00 none.
//
// One finger: a stroke runs from 0x30 through 0x40 and ends at the first other state. It is
//   a scroll (the finger slid across the surface much further than the mouse body moved),
//   a tap (short and still: left click, or right click on the right side), or nothing (the
//   finger rode along while the mouse was moved). Scrolls that end fast keep gliding. A fast
//   horizontal flick is Back (left to right) or Forward (right to left) instead.
// Two fingers: scroll by their midpoint at kTwoFingerBoost x speed (or, with two-finger
//   scrolling off, don't scroll), or, if they move apart / together more than they move
//   along, pinch to zoom; a short still two-finger tap is a middle click. A
//   second finger landing mid-stroke switches the stroke over; lifting one ends it.

#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include "..\shared\mmioctl.h"

struct EngineConfig {
    bool enabled = true;
    bool invertV = false;
    bool invertH = false;
    bool horizontal = true;
    bool inertia = true;
    bool hiRes = true;          // false: whole notches only, for apps that need them
    bool tapToClick = true;
    bool twoFingerTap = true;   // two-finger tap = middle click
    bool twoFingerScroll = true;   // two fingers scroll (faster) instead of nothing
    int  twoTapMs = 300;        // a two-finger tap must be over within this (300..600)
    bool flickInvert = false;   // flick left to right = Forward instead of Back
    bool leftHanded = false;    // the right-click tap zone is on the left
    bool ignoreTouches = false; // e.g. while the mouse is lifted: no gestures at all
    bool flickNav = true;       // horizontal flick = Back / Forward
    bool gameMode = false;      // holding a button, a finger on the other side holds that one too
    bool tapDrag = true;        // tap, then touch and hold: the button stays down (dragging)
    int  speed = 1;             // 0 slow, 1 normal, 2 fast
    int  glide = 1;             // 0 short, 1 normal, 2 long
};

enum class MouseButton { Left, Right, Middle, Back, Forward };
enum class PinchPhase { Begin, Update, End };

class EngineOutput {
public:
    virtual void Wheel(int delta) = 0;
    virtual void HWheel(int delta) = 0;
    virtual void Click(MouseButton button) = 0;
    // A button held down by a gesture (game mode, tap and hold), then released.
    virtual void Button(MouseButton button, bool down) = 0;
    // Pinch: Begin, then Update with the fingers' spread relative to the start (>1 apart,
    // <1 together) and how far their midpoint has moved since (touch units; for panning),
    // then End.
    virtual void Pinch(PinchPhase phase, double scale, double panX, double panY) = 0;
protected:
    ~EngineOutput() = default;
};

struct Touch { int id, x, y, major, minor, state; };

struct Frame {
    LONGLONG qpc;
    int dx, dy, buttons;        // mouse motion and buttons from the report header
    int count;
    Touch touch[MM_MAX_TOUCHES];
};

inline bool IsDown(const Touch& t) { return t.state == 0x30 || t.state == 0x40; }

// Same bit layout as Linux hid-magicmouse for the MM1.
inline bool DecodeFrame(const MM_FRAME& f, Frame* out) {
    const UCHAR* r = f.Report;
    if (r[0] != MM_REPORT_TOUCH || f.Length < 6 || (f.Length - 6) % 8 != 0) return false;
    out->qpc = f.Qpc;
    out->dx = (int32_t)(((uint32_t)(r[3] & 0x0C) << 28) | ((uint32_t)r[1] << 22)) >> 22;
    out->dy = (int32_t)(((uint32_t)(r[3] & 0x30) << 26) | ((uint32_t)r[2] << 22)) >> 22;
    out->buttons = r[3] & 0x03;
    out->count = min((f.Length - 6) / 8, MM_MAX_TOUCHES);
    for (int i = 0; i < out->count; i++) {
        const UCHAR* t = r + 6 + i * 8;
        out->touch[i].id    = ((t[6] << 2) | (t[5] >> 6)) & 0xF;
        out->touch[i].x     = (int32_t)((uint32_t)t[1] << 28 | (uint32_t)t[0] << 20) >> 20;
        out->touch[i].y     = -((int32_t)((uint32_t)t[2] << 24 | (uint32_t)t[1] << 16) >> 20);
        out->touch[i].major = t[3];
        out->touch[i].minor = t[4];
        out->touch[i].state = t[7] & 0xF0;
    }
    return true;
}

class Engine {
public:
    // Touch coordinates span roughly x -1100..1258 (left to right), y -1589..2047; a slow
    // swipe is ~2,500 units/s, a flick ~10,000.
    static constexpr int    kStateStart       = 0x30;
    static constexpr int    kStateDrag        = 0x40;
    static constexpr double kGain[3]          = { 0.35, 0.55, 0.85 };  // wheel units per touch unit
    static constexpr double kAccelMax         = 1.0;     // up to (1 + this) x gain on fast strokes
    static constexpr double kAccelFromSpeed   = 1500.0;  // touch units/s
    static constexpr double kAccelFullSpeed   = 8000.0;
    static constexpr int    kDeadZone         = 20;      // touch units before a stroke scrolls
    static constexpr int    kSlowDeadZone     = 15;      // ... once it's held too long to be a tap
    static constexpr double kAxisLockRatio    = 1.5;
    static constexpr double kFingerVsMouse    = 3.0;     // finger travel needed per mouse count
    static constexpr double kLandingSeconds   = 0.03;    // a landing finger nudges the mouse: not counted
    static constexpr double kVelocityWindow   = 0.06;    // seconds of drag used for lift speed
    static constexpr double kGlideMinSpeed    = 2500.0;  // touch units/s at lift to glide
    static constexpr double kGlideMaxWheel    = 9000.0;  // wheel units/s
    static constexpr double kGlideStop        = 120.0;   // wheel units/s (1 notch/s)
    static constexpr double kGlideTau[3]      = { 0.35, 0.7, 1.1 };    // s for speed to fall to 1/e
    static constexpr int    kGlideCancelMove  = 3;       // mouse counts per frame that stop a glide
    static constexpr double kTapMaxSeconds    = 0.2;
    static constexpr int    kTapMaxTravel     = 40;
    // Two-finger taps are judged loosely: contacts shift as fingers press and roll off, two
    // fingers nudge the mouse, and both can read as lifted for a moment mid-tap.
    static constexpr int    kTwoTapMaxTravel  = 180;     // per finger, touch units
    static constexpr int    kTwoTapMaxMove    = 12;      // mouse counts during a two-finger tap
    static constexpr double kSessionGrace     = 0.05;    // s with no finger down before a touch ends
    static constexpr int    kTwoFingerDeadZone = 120;    // two fingers scroll only after this travel
    static constexpr int    kTapMaxMove       = 4;       // mouse counts during a tap
    static constexpr int    kRightZoneX       = 300;     // taps right of this are right-clicks
    static constexpr int    kSurfaceMinX      = -1100, kSurfaceMaxX = 1258;
    static constexpr int    kPinchDeadZone    = 150;     // change in finger spread before zooming
    static constexpr double kFlickMaxSeconds  = 0.3;     // horizontal scroll held back this long
    static constexpr int    kFlickMinTravel   = 400;
    static constexpr double kFlickMinSpeed    = 2500.0;  // average touch units/s over the stroke
    static constexpr double kFlickAxisRatio   = 2.0;
    static constexpr double kTwoFingerBoost   = 2.0;     // two-finger scrolling speed multiplier
    static constexpr double kDragTapGap       = 0.3;     // s from a tap to the touch that holds
    static constexpr double kHoldSeconds      = 0.2;     // ... held still this long presses (or move the mouse)

    Engine(double qpcFrequency, EngineOutput& out) : freq_(qpcFrequency), out_(out) {}

    // Where the right-click tap zone starts: right of this x, or left of it when left-handed.
    static int ZoneX(bool leftHanded) { return leftHanded ? kSurfaceMinX + kSurfaceMaxX - kRightZoneX : kRightZoneX; }

    // Input stopped (the mouse went away): end whatever was in progress.
    void Cancel() {
        StopGlide();
        AbortStroke();
        ReleaseGame();
    }

    bool Gliding() const { return gliding_; }
    bool Pinching() const { return stroke_ && pinch_; }
    // Tick() has work to do: a glide, or a touch waiting out kSessionGrace to end.
    bool NeedsTick() const { return gliding_ || (session_ && sessGapStart_ != 0); }
    bool Scrolling() const { return stroke_ && committed_ && !multi_ && !pinch_; }
    int  Fingers() const { return fingers_; }
    // Vertical glide speed in wheel units/s, signed by the finger's direction (positive:
    // the finger moved towards the back of the mouse), whatever the scroll inversion.
    double GlideFingerSpeed() const { return gliding_ ? velV_ * glideSignV_ : 0.0; }

    void OnFrame(const Frame& f, const EngineConfig& cfg) {
        double dt = lastQpc_ ? (f.qpc - lastQpc_) / freq_ : 0.0;
        lastQpc_ = f.qpc;
        if (dt < 0 || dt > 0.1) dt = 0;
        int move = std::abs(f.dx) + std::abs(f.dy);
        fingers_ = 0;
        for (int i = 0; i < f.count; i++) fingers_ += IsDown(f.touch[i]);

        Session(f, move, cfg);
        Game(f, cfg);

        if (holding_) {
            // Tap and hold: the button stays down until the finger lifts or the mouse is
            // clicked (lifting the mouse to reposition it doesn't let go).
            const Touch* t = Find(f, trackId_);
            if (f.buttons || !cfg.enabled || !t || !IsDown(*t)) AbortStroke();
            else return;
        }
        if (!cfg.enabled || f.buttons || cfg.ignoreTouches) {
            // Clicking (or paused, or lifted): no scrolling, no tap, nothing to glide.
            StopGlide();
            AbortStroke();
            return;
        }
        if (gliding_ && move >= kGlideCancelMove) StopGlide();

        bool pair = fingers_ == 2;
        if (stroke_) {
            const Touch* t = Find(f, trackId_);
            strokeMove_ += move;
            if ((f.qpc - startQpc_) / freq_ > kLandingSeconds) rideMove_ += move;
            if (fingers_ >= 3) {
                multi_ = true;
                EndPinch();                     // a third finger: lift the pinch's touches now
            }
            if (twoFinger_ ? fingers_ < 2 : (!t || !IsDown(*t))) {
                EndStroke(cfg);
            } else if (pair && !twoFinger_) {
                twoFinger_ = true;              // second finger landed: follow the midpoint
                holdArmed_ = false;
                if (navPending_) ReleasePending();     // no longer a one-finger flick
                int x, y;
                Midpoint(f, &x, &y);
                Rebase(x, y, f.qpc);
                startSpread_ = lastSpread_ = Spread(f);
            } else if (twoFinger_) {
                int x, y;
                Midpoint(f, &x, &y);
                Drag(x, y, f.qpc, dt, cfg, Spread(f));
            } else if (holdArmed_ && Hold(*t, f.qpc)) {
                // deciding whether this touch holds the button: no scrolling meanwhile
            } else if (t->state == kStateDrag) {
                Drag(t->x, t->y, f.qpc, dt, cfg);
            }
        }
        if (!stroke_ && (fingers_ == 1 || pair)) {
            for (int i = 0; i < f.count; i++) {
                if (f.touch[i].state != kStateStart) continue;
                Begin(f.touch[i], f.qpc);
                if (pair) {
                    twoFinger_ = true;
                    int x, y;
                    Midpoint(f, &x, &y);
                    Rebase(x, y, f.qpc);
                    startX_ = x;
                    startY_ = y;
                    startSpread_ = lastSpread_ = Spread(f);
                } else if (cfg.tapDrag && tapQpc_ && (f.qpc - tapQpc_) / freq_ <= kDragTapGap &&
                           Side(f.touch[i].x, cfg) == tapButton_) {
                    holdArmed_ = true;          // a touch right after a tap, on the same side
                }
                break;
            }
        }
    }

    void Tick(LONGLONG nowQpc, const EngineConfig& cfg) {
        if (session_ && sessGapStart_ && (nowQpc - sessGapStart_) / freq_ >= kSessionGrace) EndSession(cfg);
        if (!gliding_) return;
        double dt = (nowQpc - glideQpc_) / freq_;
        if (dt <= 0) return;
        glideQpc_ = nowQpc;
        double decay = std::exp(-dt / kGlideTau[cfg.glide]);
        // Integrate the decaying speed exactly over dt, so tick timing doesn't matter.
        double travel = kGlideTau[cfg.glide] * (1.0 - decay);
        accV_ += velV_ * travel;
        accH_ += velH_ * travel;
        velV_ *= decay;
        velH_ *= decay;
        Emit(cfg);
        if (std::abs(velV_) < kGlideStop && std::abs(velH_) < kGlideStop) StopGlide();
    }

private:
    enum class Axis { Vertical, Horizontal, Both };
    struct Sample { LONGLONG qpc; int x, y; };
    struct Start { int id, x, y; };

    static const Touch* Find(const Frame& f, int id) {
        for (int i = 0; i < f.count; i++) if (f.touch[i].id == id) return &f.touch[i];
        return nullptr;
    }

    // A session runs from the first finger down until no finger has been down for
    // kSessionGrace. Touching stops a glide (and such a touch is never a tap); a short,
    // still two-finger session that didn't scroll is a middle click.
    void Session(const Frame& f, int move, const EngineConfig& cfg) {
        if (fingers_ > 0) {
            if (!session_) {
                session_ = true;
                caughtGlide_ = gliding_;
                StopGlide();
                sessStart_ = f.qpc;
                sessFingers_ = 0;
                sessMove_ = 0;
                sessSpoiled_ = false;
                sessScrolled_ = false;
                starts_ = 0;
            }
            sessGapStart_ = 0;
            sessEnd_ = f.qpc;
            sessFingers_ = max(sessFingers_, fingers_);
            sessMove_ += move;
            if (f.buttons) sessSpoiled_ = true;
            for (int i = 0; i < f.count; i++) {
                const Touch& t = f.touch[i];
                if (!IsDown(t)) continue;
                int s = 0;
                while (s < starts_ && start_[s].id != t.id) s++;
                if (s == starts_) {
                    if (starts_ == kMaxStarts) continue;
                    start_[starts_++] = { t.id, t.x, t.y };
                }
                int tx = t.x - start_[s].x, ty = t.y - start_[s].y;
                if (tx * tx + ty * ty > kTwoTapMaxTravel * kTwoTapMaxTravel) sessSpoiled_ = true;
            }
        } else if (session_) {
            sessMove_ += move;
            if (!sessGapStart_) sessGapStart_ = f.qpc;
            if ((f.qpc - sessGapStart_) / freq_ >= kSessionGrace) EndSession(cfg);
        }
    }

    void EndSession(const EngineConfig& cfg) {
        session_ = false;
        sessGapStart_ = 0;
        if (cfg.enabled && cfg.twoFingerTap && sessFingers_ == 2 && !sessSpoiled_ && !sessScrolled_ && !caughtGlide_ &&
            (sessEnd_ - sessStart_) / freq_ <= cfg.twoTapMs / 1000.0 && sessMove_ <= kTwoTapMaxMove) {
            out_.Click(MouseButton::Middle);
        }
    }

    double Gain(double fingerSpeed, const EngineConfig& cfg) const {
        double t = (fingerSpeed - kAccelFromSpeed) / (kAccelFullSpeed - kAccelFromSpeed);
        return kGain[cfg.speed] * (1.0 + kAccelMax * max(0.0, min(1.0, t))) * (twoFinger_ ? kTwoFingerBoost : 1.0);
    }

    // Midpoint of the fingers on the surface.
    static void Midpoint(const Frame& f, int* x, int* y) {
        int n = 0, sx = 0, sy = 0;
        for (int i = 0; i < f.count; i++) {
            if (!IsDown(f.touch[i])) continue;
            sx += f.touch[i].x;
            sy += f.touch[i].y;
            n++;
        }
        *x = n ? sx / n : 0;
        *y = n ? sy / n : 0;
    }

    // Distance between the two fingers on the surface (0 unless exactly two).
    static double Spread(const Frame& f) {
        const Touch* a = nullptr;
        for (int i = 0; i < f.count; i++) {
            if (!IsDown(f.touch[i])) continue;
            if (!a) { a = &f.touch[i]; continue; }
            double dx = f.touch[i].x - a->x, dy = f.touch[i].y - a->y;
            return std::sqrt(dx * dx + dy * dy);
        }
        return 0;
    }

    // The followed point jumped (a finger joined): continue from here without scrolling the jump.
    void Rebase(int x, int y, LONGLONG qpc) {
        lastX_ = x;
        lastY_ = y;
        if (!committed_) {
            startX_ = x;
            startY_ = y;
        }
        samples_ = 0;
        Record(qpc, x, y);
    }

    void Begin(const Touch& t, LONGLONG qpc) {
        stroke_ = true;
        committed_ = false;
        multi_ = false;
        twoFinger_ = false;
        pinch_ = false;
        pinchEnded_ = false;
        navPending_ = false;
        holdArmed_ = false;
        trackId_ = t.id;
        startX_ = lastX_ = t.x;
        startY_ = lastY_ = t.y;
        startQpc_ = endQpc_ = qpc;
        strokeMove_ = rideMove_ = 0;
        maxTravel_ = 0;
        samples_ = 0;
        accV_ = accH_ = pendingH_ = 0;
        Record(qpc, t.x, t.y);
    }

    void Record(LONGLONG qpc, int x, int y) {
        history_[samples_ % kHistory] = { qpc, x, y };
        samples_++;
    }

    void Drag(int x, int y, LONGLONG qpc, double dt, const EngineConfig& cfg, double spread = 0) {
        int ddx = x - lastX_, ddy = y - lastY_;
        lastX_ = x;
        lastY_ = y;
        endQpc_ = qpc;
        Record(qpc, x, y);
        int tx = x - startX_, ty = y - startY_;
        double travel = std::sqrt((double)tx * tx + (double)ty * ty);
        maxTravel_ = max(maxTravel_, travel);
        if (multi_) return;

        if (pinch_) {
            if (pinchEnded_) return;
            if (spread > 1) lastSpread_ = spread;
            out_.Pinch(PinchPhase::Update, lastSpread_ / startSpread_, x - pinchX_, y - pinchY_);
            return;
        }
        if (twoFinger_ && !committed_ && spread > 1) {
            // Fingers moving apart / together more than along: a pinch, not a scroll. The
            // dead-zone change counts, so the zoom starts where the fingers already are.
            double change = std::fabs(spread - startSpread_);
            if (change >= kPinchDeadZone && change > travel) {
                committed_ = pinch_ = true;
                sessScrolled_ = true;           // not a tap either
                lastSpread_ = spread;
                pinchX_ = x;
                pinchY_ = y;
                out_.Pinch(PinchPhase::Begin, 1.0, 0, 0);
                out_.Pinch(PinchPhase::Update, lastSpread_ / startSpread_, 0, 0);
                return;
            }
        }

        if (twoFinger_ && !cfg.twoFingerScroll) return;     // two fingers only pinch

        double speed = dt > 0 ? std::sqrt((double)ddx * ddx + (double)ddy * ddy) / dt : 0.0;
        if (!committed_) {
            // Scroll only if the finger slid across the surface rather than riding along
            // while the mouse was moved. A finger down too long to be a tap needs less travel.
            int deadZone = twoFinger_ ? kTwoFingerDeadZone
                         : (qpc - startQpc_) / freq_ > kTapMaxSeconds ? kSlowDeadZone : kDeadZone;
            if (travel < deadZone || travel < kFingerVsMouse * rideMove_) return;
            committed_ = true;
            sessScrolled_ = true;
            if (std::abs(ty) > kAxisLockRatio * std::abs(tx)) axis_ = Axis::Vertical;
            else if (std::abs(tx) > kAxisLockRatio * std::abs(ty)) axis_ = Axis::Horizontal;
            else axis_ = Axis::Both;
            navPending_ = cfg.flickNav && axis_ == Axis::Horizontal && !twoFinger_;
            if (!twoFinger_) {
                // One finger scrolls its dead-zone travel too, so the page moves from the
                // first frame with the finger, not only after it.
                ddx = tx;
                ddy = ty;
            }
        }

        double gain = Gain(speed, cfg);
        if (axis_ != Axis::Horizontal) accV_ += ddy * gain * SignV(cfg);
        double h = axis_ != Axis::Vertical && cfg.horizontal ? ddx * gain * SignH(cfg) : 0.0;
        if (navPending_) {
            // Hold horizontal scrolling back until the stroke is too long to be a flick.
            pendingH_ += h;
            if ((qpc - startQpc_) / freq_ > kFlickMaxSeconds) ReleasePending();
        } else {
            accH_ += h;
        }
        Emit(cfg);
    }

    void ReleasePending() {
        navPending_ = false;
        accH_ += pendingH_;
        pendingH_ = 0;
    }

    // Ends a pinch in progress once (it stays a pinch, so the stroke doesn't turn into a scroll).
    void EndPinch() {
        if (pinch_ && !pinchEnded_) out_.Pinch(PinchPhase::End, lastSpread_ / startSpread_, lastX_ - pinchX_, lastY_ - pinchY_);
        pinchEnded_ = true;
    }

    // A stroke cut short (button pressed, gestures off): a pinch in progress must still end.
    void AbortStroke() {
        if (stroke_) EndPinch();
        stroke_ = false;
        holdArmed_ = false;
        ReleaseHold();
    }

    // After a tap, a touch on the same side that stays put (or moves the mouse along with it)
    // holds the tap's button down until it lifts. Returns false once it's sliding (a scroll).
    bool Hold(const Touch& t, LONGLONG qpc) {
        int tx = t.x - startX_, ty = t.y - startY_;
        if (tx * tx + ty * ty > kTapMaxTravel * kTapMaxTravel) {
            holdArmed_ = false;
            return false;
        }
        if ((qpc - startQpc_) / freq_ >= kHoldSeconds || rideMove_ > kTapMaxMove) {
            holdArmed_ = false;
            holding_ = true;
            tapQpc_ = 0;
            out_.Button(tapButton_, true);
        }
        return true;
    }

    void ReleaseHold() {
        if (holding_) out_.Button(tapButton_, false);
        holding_ = false;
    }

    // Game mode: while a mouse button is held down, a finger landing on the other button's
    // side holds that button too (aim and shoot), until that finger lifts.
    void Game(const Frame& f, const EngineConfig& cfg) {
        unsigned down = 0;
        for (int i = 0; i < f.count; i++) if (IsDown(f.touch[i])) down |= 1u << f.touch[i].id;
        unsigned landed = down & ~prevDown_;
        prevDown_ = down;
        if (gameHeld_) {
            if (!(down & (1u << gameTouch_)) || !cfg.enabled) ReleaseGame();
            return;
        }
        if (!cfg.gameMode || !cfg.enabled || cfg.ignoreTouches || !f.buttons) return;
        for (int i = 0; i < f.count; i++) {
            const Touch& t = f.touch[i];
            if (!(landed & (1u << t.id))) continue;
            bool rightSide = t.x > kRightZoneX;      // the mouse's own sides (its buttons aren't swapped here)
            if (f.buttons & (rightSide ? MM_BUTTON_RIGHT : MM_BUTTON_LEFT)) continue;    // the held one
            gameHeld_ = true;
            gameTouch_ = t.id;
            gameButton_ = rightSide != cfg.leftHanded ? MouseButton::Right : MouseButton::Left;
            out_.Button(gameButton_, true);
            return;
        }
    }

    void ReleaseGame() {
        if (gameHeld_) out_.Button(gameButton_, false);
        gameHeld_ = false;
    }

    void EndStroke(const EngineConfig& cfg) {
        if (holding_) {
            AbortStroke();                      // the hold ends with the touch; it isn't a tap
            return;
        }
        stroke_ = false;
        EndPinch();
        if (multi_ || pinch_) return;
        if (committed_ && !twoFinger_ && IsTap()) {
            // Scrolling starts sooner than a tap can be ruled out: a short, small stroke
            // still clicks (after scrolling a fraction of a notch).
            Tap(cfg);
            return;
        }
        if (committed_) {
            if (navPending_) {
                int dx = lastX_ - startX_, dy = lastY_ - startY_;
                double seconds = (endQpc_ - startQpc_) / freq_;
                if (std::abs(dx) >= kFlickMinTravel && std::abs(dx) >= kFlickAxisRatio * std::abs(dy) &&
                    seconds > 0 && std::abs(dx) / seconds >= kFlickMinSpeed) {
                    navPending_ = false;
                    pendingH_ = 0;
                    out_.Click((dx > 0) != cfg.flickInvert ? MouseButton::Back : MouseButton::Forward);
                    return;
                }
                ReleasePending();
                Emit(cfg);
            }
            if (cfg.inertia) StartGlide(cfg);
            return;
        }
        if (twoFinger_) return;                 // a two-finger touch is the session's to judge
        Tap(cfg);
    }

    bool IsTap() const {
        return (endQpc_ - startQpc_) / freq_ <= kTapMaxSeconds && maxTravel_ <= kTapMaxTravel && strokeMove_ <= kTapMaxMove;
    }

    void Tap(const EngineConfig& cfg) {
        if (!cfg.tapToClick || caughtGlide_ || !IsTap()) return;
        tapButton_ = Side(startX_, cfg);
        tapQpc_ = endQpc_;
        out_.Click(tapButton_);
    }

    // The button a tap at x clicks.
    static MouseButton Side(int x, const EngineConfig& cfg) {
        bool secondary = cfg.leftHanded ? x < ZoneX(true) : x > ZoneX(false);
        return secondary ? MouseButton::Right : MouseButton::Left;
    }

    // Finger velocity over the last kVelocityWindow of the stroke, in touch units/s.
    bool LiftVelocity(double* vx, double* vy) const {
        if (samples_ < 2) return false;
        const Sample& last = history_[(samples_ - 1) % kHistory];
        const Sample* first = &last;
        for (int i = 2; i <= min(samples_, kHistory); i++) {
            const Sample& s = history_[(samples_ - i) % kHistory];
            if ((last.qpc - s.qpc) / freq_ > kVelocityWindow) break;
            first = &s;
        }
        double dt = (last.qpc - first->qpc) / freq_;
        if (dt <= 0) return false;
        *vx = (last.x - first->x) / dt;
        *vy = (last.y - first->y) / dt;
        return true;
    }

    void StartGlide(const EngineConfig& cfg) {
        double vx, vy;
        if (!LiftVelocity(&vx, &vy)) return;
        double speed = std::sqrt(vx * vx + vy * vy);
        if (speed < kGlideMinSpeed) return;
        double gain = Gain(speed, cfg);
        velV_ = axis_ != Axis::Horizontal ? vy * gain * SignV(cfg) : 0.0;
        velH_ = axis_ != Axis::Vertical && cfg.horizontal ? vx * gain * SignH(cfg) : 0.0;
        velV_ = max(-kGlideMaxWheel, min(kGlideMaxWheel, velV_));
        velH_ = max(-kGlideMaxWheel, min(kGlideMaxWheel, velH_));
        glideSignV_ = SignV(cfg);
        glideQpc_ = endQpc_;
        gliding_ = velV_ != 0 || velH_ != 0;
    }

    void StopGlide() {
        gliding_ = false;
        velV_ = velH_ = 0;
    }

    static double SignV(const EngineConfig& cfg) { return cfg.invertV ? -1.0 : 1.0; }
    static double SignH(const EngineConfig& cfg) { return cfg.invertH ? -1.0 : 1.0; }

    // Sends the whole wheel units accumulated so far (whole notches in classic mode).
    void Emit(const EngineConfig& cfg) {
        int step = cfg.hiRes ? 1 : WHEEL_DELTA;
        int v = (int)(accV_ / step) * step;
        int h = (int)(accH_ / step) * step;
        if (v) { accV_ -= v; out_.Wheel(v); }
        if (h) { accH_ -= h; out_.HWheel(h); }
    }

    static constexpr int kHistory = 16;
    static constexpr int kMaxStarts = 5;

    double freq_;
    EngineOutput& out_;
    LONGLONG lastQpc_ = 0, startQpc_ = 0, endQpc_ = 0, glideQpc_ = 0;
    bool stroke_ = false, committed_ = false, gliding_ = false, multi_ = false, navPending_ = false;
    bool twoFinger_ = false;                // the stroke follows two fingers' midpoint
    bool pinch_ = false;                    // ... and is a pinch zoom
    bool pinchEnded_ = false;               // the pinch's End was sent
    int pinchX_ = 0, pinchY_ = 0;           // the fingers' midpoint when the pinch began
    double startSpread_ = 0, lastSpread_ = 0;
    double glideSignV_ = 1.0;
    int trackId_ = -1, startX_ = 0, startY_ = 0, lastX_ = 0, lastY_ = 0;
    int strokeMove_ = 0, samples_ = 0, fingers_ = 0;
    int rideMove_ = 0;                      // mouse motion after the finger's landing bump
    LONGLONG tapQpc_ = 0;                   // when the last tap clicked (0: none to hold after)
    MouseButton tapButton_ = MouseButton::Left;
    bool holdArmed_ = false;                // this touch came right after a tap: it may hold
    bool holding_ = false;                  // ... and is holding tapButton_ down
    unsigned prevDown_ = 0;                 // touch ids down in the previous frame
    bool gameHeld_ = false;                 // game mode is holding gameButton_ for touch gameTouch_
    int gameTouch_ = 0;
    MouseButton gameButton_ = MouseButton::Left;
    double maxTravel_ = 0;
    Axis axis_ = Axis::Vertical;
    Sample history_[kHistory] = {};
    double accV_ = 0, accH_ = 0, pendingH_ = 0, velV_ = 0, velH_ = 0;

    bool session_ = false, sessSpoiled_ = false, sessScrolled_ = false, caughtGlide_ = false;
    LONGLONG sessStart_ = 0, sessEnd_ = 0, sessGapStart_ = 0;
    int sessFingers_ = 0, sessMove_ = 0, starts_ = 0;
    Start start_[kMaxStarts] = {};
};
