// mmreplay.cpp - runs an mmcapture_log.txt through the tray app's engine (app\engine.h)
// and prints what it would have sent: scroll per stroke, glides and clicks.
//
// Usage: mmreplay [log] [invert 0/1] [speed 0-2] [glide 0-2]
// Build: cl /nologo /EHsc /O2 /W4 /std:c++17 mmreplay.cpp

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "..\app\engine.h"

static const double kFreq = 1e7;    // replay clock: 100 ns ticks

struct Recorder : EngineOutput {
    double now = 0;                 // ms
    bool inGlide = false;
    long v = 0, h = 0, gv = 0, gh = 0;
    int events = 0;
    double glideStart = -1, glideEnd = 0;
    double strokeStart = -1, firstOut = -1;     // when the stroke began / first scrolled
    void Wheel(int d) override { (inGlide ? gv : v) += d; events++; Glide(); First(); }
    void HWheel(int d) override { (inGlide ? gh : h) += d; events++; Glide(); First(); }
    void First() { if (!inGlide && firstOut < 0) firstOut = now; }
    void Pinch(PinchPhase phase, double scale, double, double) override {
        if (phase == PinchPhase::Update) return;
        Flush();
        printf("%9.1f  PINCH %s (x%.2f)\n", now, phase == PinchPhase::Begin ? "begin" : "end", scale);
    }
    void Click(MouseButton b) override {
        static const char* const names[] = { "left", "right", "middle", "BACK", "FORWARD" };
        Flush();
        printf("%9.1f  CLICK %s\n", now, names[(int)b]);
    }
    void Button(MouseButton b, bool down) override {
        static const char* const names[] = { "left", "right", "middle", "back", "forward" };
        Flush();
        printf("%9.1f  BUTTON %s %s\n", now, names[(int)b], down ? "down" : "up");
    }
    void Glide() {
        if (!inGlide) return;
        if (glideStart < 0) glideStart = now;
        glideEnd = now;
    }
    void Flush() {
        if (v || h) printf("%9.1f  scroll  v %+6ld  h %+6ld  (%.1f notches, first after %.0f ms)\n", now, v, h,
                           (abs(v) + abs(h)) / 120.0, firstOut - strokeStart);
        if (gv || gh) printf("%9.1f  glide   v %+6ld  h %+6ld  (%.1f notches over %.0f ms)\n", glideStart, gv, gh,
                             (abs(gv) + abs(gh)) / 120.0, glideEnd - glideStart);
        v = h = gv = gh = 0;
        glideStart = -1;
        firstOut = -1;
    }
};

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "mmcapture_log.txt";
    EngineConfig cfg;
    if (argc > 2) cfg.invertV = cfg.invertH = atoi(argv[2]) != 0;
    if (argc > 3) cfg.speed = atoi(argv[3]);
    if (argc > 4) cfg.glide = atoi(argv[4]);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) { printf("can't open %s\n", path); return 1; }

    Recorder rec;
    Engine engine(kFreq, rec);
    char line[2048];
    double lastMs = 0;
    int lastState = -1;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        Frame fr{};
        double ms;
        int n = 0;
        if (sscanf_s(line, "%lf mv(%d,%d) b%d |%n", &ms, &fr.dx, &fr.dy, &fr.buttons, &n) < 4) continue;
        const char* p = line + n;
        while ((p = strchr(p, '[')) != nullptr && fr.count < MM_MAX_TOUCHES) {
            Touch& t = fr.touch[fr.count];
            if (sscanf_s(p, "[%d %x %d %d %d %d]", &t.id, &t.state, &t.x, &t.y, &t.major, &t.minor) == 6) fr.count++;
            p++;
        }

        // Ticks every 8 ms between frames while the engine needs them, as the app does.
        rec.inGlide = true;
        for (double t = lastMs + 8; engine.NeedsTick() && t < ms; t += 8) {
            rec.now = t;
            engine.Tick((LONGLONG)(t * kFreq / 1000), cfg);
        }
        rec.inGlide = false;
        lastMs = ms;

        // A stroke boundary: print what the previous stroke produced.
        int state = fr.count ? fr.touch[0].state : -1;
        if (state == Engine::kStateStart && lastState != Engine::kStateStart) { rec.Flush(); rec.strokeStart = ms; }
        lastState = state;

        rec.now = ms;
        fr.qpc = (LONGLONG)(ms * kFreq / 1000);
        engine.OnFrame(fr, cfg);
    }
    rec.inGlide = true;
    for (double t = lastMs + 8; engine.NeedsTick(); t += 8) {
        rec.now = t;
        engine.Tick((LONGLONG)(t * kFreq / 1000), cfg);
    }
    rec.Flush();
    fclose(f);
    return 0;
}
