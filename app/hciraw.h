// hciraw.h - touch frames from raw Bluetooth traffic, for Boot Camp mode (after
// OpenMagicMouse, github.com/haikieudng-cyber/OpenMagicMouse).
//
// With Apple's signed Boot Camp driver (applewirelessmouse.sys) on the mouse instead of
// mmfilter, the mouse is in multi-touch mode but its touch reports never reach user mode.
// Windows' Bluetooth stack logs every packet it moves as ETW event 402 ("HCIRAW") of
// Microsoft-Windows-BTH-BTHPORT, so the app reads them there. Event 402's data:
//   3 bytes, packet type (1 command, 2 event, 3 ACL from the device, 4 ACL to it),
//   32-bit length, the HCI packet.
// An ACL packet carries L2CAP: "handle len | l2cap-len cid | payload", split across several
// packets when the payload is longer than the link's packets (two or more fingers). HID input
// reports are payloads "A1 id ...". Windows blanks ACL payloads in the log unless
// BTHPORT\Parameters\EtwLogSensitiveData is 1.
//
// Pure logic: no Windows headers needed.

#pragma once
#include <cstdint>
#include <cstring>

static const uint8_t kHciCommand = 1, kHciEvent = 2, kHciAclIn = 3, kHciAclOut = 4;
static const uint8_t kHidDataInput = 0xA1, kHidDataFeature = 0xA3;

// Reports the mouse sends, as the app's frames use them (report ID first). 0x29 is the
// frame format the gesture engine reads; Magic Mouse 2 reports are converted to it.
static const uint8_t kMm1Touch = 0x29, kMm2Report = 0x12, kMmDouble = 0xF7, kMmLift = 0x61;
static const uint32_t kMaxTouches = 16, kMaxFrame = 6 + kMaxTouches * 8;

class HciRawDecoder {
public:
    enum Result { Ignored, Payload, Disconnected, Scrubbed };

    // Feeds one event 402 payload. On Payload, Handle() / Data() / Length() are a complete
    // L2CAP payload received from a device (valid until the next Feed); on Disconnected,
    // Handle() is the link that closed; Scrubbed: Windows blanked the payload.
    Result Feed(const uint8_t* p, uint32_t n) {
        if (n < 8) return Ignored;
        uint8_t type = p[3];
        uint32_t len = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
        if (len == 0 || len > n - 8) return Ignored;
        const uint8_t* h = p + 8;

        if (type == kHciEvent) {
            if (len >= 6 && h[0] == 0x05 && h[2] == 0x00) {        // Disconnection Complete, success
                handle_ = (uint16_t)((h[3] | h[4] << 8) & 0x0FFF);
                for (Pending& s : slots_) if (s.handle == handle_) s.expected = 0;
                return Disconnected;
            }
            return Ignored;
        }
        if (type != kHciAclIn || len < 4) return Ignored;

        uint16_t hf = (uint16_t)(h[0] | h[1] << 8);
        uint16_t handle = hf & 0x0FFF;
        uint8_t pb = (hf >> 12) & 0x03;
        uint32_t acl = h[2] | h[3] << 8;
        if (4 + acl > len) return acl > 4 ? Scrubbed : Ignored;   // the log kept only the header
        const uint8_t* a = h + 4;

        if (pb == 0x02 || pb == 0x00) {                            // start of an L2CAP packet
            Pending& s = Slot(handle, true);
            s.expected = 0;
            if (acl < 4) return Ignored;
            uint32_t total = a[0] | a[1] << 8;
            if (total == 0 || total > sizeof(s.buf)) return Ignored;
            if (acl - 4 >= total) return Complete(handle, a + 4, total);
            s.handle = handle;
            s.expected = total;
            s.have = acl - 4;
            memcpy(s.buf, a + 4, s.have);
            return Ignored;
        }
        if (pb == 0x01) {                                           // continuation
            Pending& s = Slot(handle, false);
            if (!s.expected || s.handle != handle) return Ignored;
            uint32_t take = acl < s.expected - s.have ? acl : s.expected - s.have;
            memcpy(s.buf + s.have, a, take);
            s.have += take;
            if (s.have < s.expected) return Ignored;
            s.expected = 0;
            return Complete(handle, s.buf, s.have);
        }
        return Ignored;
    }

    uint16_t Handle() const { return handle_; }
    const uint8_t* Data() const { return data_; }
    uint32_t Length() const { return length_; }

private:
    struct Pending {
        uint16_t handle = 0xFFFF;
        uint32_t expected = 0, have = 0;
        uint8_t buf[512];
    };

    Result Complete(uint16_t handle, const uint8_t* d, uint32_t n) {
        handle_ = handle;
        data_ = d;
        length_ = n;
        return Payload;
    }

    // The slot reassembling this link's packet; a free (or the first) one when starting.
    Pending& Slot(uint16_t handle, bool start) {
        for (Pending& s : slots_) if (s.handle == handle) return s;
        if (start) for (Pending& s : slots_) if (!s.expected) return s;
        return slots_[0];
    }

    Pending slots_[4];
    uint16_t handle_ = 0;
    const uint8_t* data_ = nullptr;
    uint32_t length_ = 0;
};

// One Magic Mouse 2 / USB-C report 0x12 with touches (ID, buttons, X, Y 16-bit each, header to
// 14 bytes, 8 bytes per touch) as a 2009-mouse 0x29 frame: motion clamped to its 10 bits, the
// touch records unchanged. Returns the frame's length, 0 if it isn't one.
inline uint32_t Mm2ToTouchFrame(const uint8_t* r, uint32_t n, uint8_t* out) {
    if (n < 14 || r[0] != kMm2Report || (n - 14) % 8) return 0;
    uint32_t touches = (n - 14) / 8;
    if (touches > kMaxTouches) touches = kMaxTouches;
    int x = (int16_t)(r[2] | r[3] << 8), y = (int16_t)(r[4] | r[5] << 8);
    x = x < -511 ? -511 : x > 511 ? 511 : x;
    y = y < -511 ? -511 : y > 511 ? 511 : y;
    out[0] = kMm1Touch;
    out[1] = (uint8_t)x;
    out[2] = (uint8_t)y;
    out[3] = (uint8_t)((r[1] & 0x03) | ((x >> 8) & 0x03) << 2 | ((y >> 8) & 0x03) << 4);
    out[4] = out[5] = 0;
    memcpy(out + 6, r + 14, touches * 8);
    return 6 + touches * 8;
}

inline bool IsMm1TouchFrame(const uint8_t* r, uint32_t n) {
    return n >= 6 && n <= kMaxFrame && (n - 6) % 8 == 0 && r[0] == kMm1Touch;
}

// The app's frames in an L2CAP payload from the mouse ("A1 id ..."): touch frames (0x29, a
// converted 0x12, or two of either packed in F7) and the lift report (0x61). Writes up to two
// frames; returns how many.
inline int MouseFrames(const uint8_t* p, uint32_t n, uint8_t out[2][kMaxFrame], uint32_t outLen[2]) {
    if (n < 2 || p[0] != kHidDataInput) return 0;
    const uint8_t* part[2];
    uint32_t partLen[2], parts = 1;
    if (p[1] == kMmDouble && n >= 3 && p[2] <= n - 3) {
        part[0] = p + 3;
        partLen[0] = p[2];
        part[1] = p + 3 + p[2];
        partLen[1] = n - 3 - p[2];
        parts = 2;
    } else {
        part[0] = p + 1;
        partLen[0] = n - 1;
    }
    int count = 0;
    for (uint32_t i = 0; i < parts; i++) {
        const uint8_t* r = part[i];
        uint32_t len = partLen[i];
        if (IsMm1TouchFrame(r, len)) {
            memcpy(out[count], r, len);
            outLen[count++] = len;
        } else if (uint32_t m = Mm2ToTouchFrame(r, len, out[count])) {
            outLen[count++] = m;
        } else if (len >= 2 && r[0] == kMmLift && parts == 1) {
            out[count][0] = r[0];
            out[count][1] = r[1];
            outLen[count++] = 2;
        }
    }
    return count;
}
