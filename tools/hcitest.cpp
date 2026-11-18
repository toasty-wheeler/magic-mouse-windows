// hcitest.cpp - checks app\hciraw.h (Boot Camp mode: touch frames from raw Bluetooth packets)
// with hand-built packets, through to the gesture engine's DecodeFrame.
// Build: cl /nologo /EHsc /W4 /std:c++17 hcitest.cpp   Run: hcitest  (exit code = failures)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <vector>
#include "..\app\engine.h"
#include "..\app\hciraw.h"

static int g_failures;

static void Check(bool ok, const char* what) {
    if (!ok) g_failures++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// An event 402 payload: 3 bytes, type, 32-bit length, HCI packet.
static std::vector<uint8_t> Event402(uint8_t type, const std::vector<uint8_t>& hci) {
    std::vector<uint8_t> e = { 0, 0, 0, type };
    uint32_t n = (uint32_t)hci.size();
    for (int i = 0; i < 4; i++) e.push_back((uint8_t)(n >> (8 * i)));
    e.insert(e.end(), hci.begin(), hci.end());
    return e;
}

// ACL packet on handle: pb 2 = start, 1 = continuation; data = what follows the ACL header.
static std::vector<uint8_t> Acl(uint16_t handle, uint8_t pb, const std::vector<uint8_t>& data, int claimLen = -1) {
    uint16_t hf = (uint16_t)(handle | pb << 12);
    uint16_t len = (uint16_t)(claimLen >= 0 ? claimLen : (int)data.size());
    std::vector<uint8_t> a = { (uint8_t)hf, (uint8_t)(hf >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    a.insert(a.end(), data.begin(), data.end());
    return Event402(kHciAclIn, a);
}

// L2CAP header + payload on the HID interrupt channel's CID.
static std::vector<uint8_t> L2cap(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> l = { (uint8_t)payload.size(), (uint8_t)(payload.size() >> 8), 0x41, 0x00 };
    l.insert(l.end(), payload.begin(), payload.end());
    return l;
}

// A touch record at (x, y) with id and state, laid out as the mouse sends it.
static std::vector<uint8_t> TouchRecord(int x, int y, int id, uint8_t state) {
    int yr = -y;
    return { (uint8_t)x, (uint8_t)(((x >> 8) & 0x0F) | (yr & 0x0F) << 4), (uint8_t)(yr >> 4), 40, 30,
             (uint8_t)(10 | (id & 0x03) << 6), (uint8_t)((id >> 2) & 0x03 | 32 << 2), state };
}

static bool Feed(HciRawDecoder& d, const std::vector<uint8_t>& e, HciRawDecoder::Result want) {
    return d.Feed(e.data(), (uint32_t)e.size()) == want;
}

// Runs a decoded payload through MouseFrames and the engine's DecodeFrame.
static int Frames(HciRawDecoder& d, Frame* f, int max) {
    uint8_t out[2][kMaxFrame];
    uint32_t len[2];
    int n = MouseFrames(d.Data(), d.Length(), out, len), got = 0;
    for (int i = 0; i < n && got < max; i++) {
        MM_FRAME mf{};
        mf.Length = (USHORT)len[i];
        memcpy(mf.Report, out[i], len[i]);
        if (DecodeFrame(mf, &f[got])) got++;
    }
    return got;
}

int main() {
    Frame f[2];
    {
        printf("-- 2009 mouse: report 0x29, one packet\n");
        HciRawDecoder d;
        std::vector<uint8_t> r = { kHidDataInput, kMm1Touch, 5, 0xFD, 0x01, 0, 0 };   // dx 5, dy -3, left button
        auto t = TouchRecord(100, -200, 3, 0x40);
        r.insert(r.end(), t.begin(), t.end());
        Check(Feed(d, Acl(0x0B, 2, L2cap(r)), HciRawDecoder::Payload), "payload decoded");
        int n = Frames(d, f, 2);
        Check(n == 1 && f[0].count == 1 && f[0].touch[0].x == 100 && f[0].touch[0].y == -200 && f[0].touch[0].id == 3 &&
              f[0].touch[0].state == 0x40 && f[0].buttons == 1, "touch (100, -200), id 3, down, left button");
    }
    {
        printf("-- Magic Mouse 2: report 0x12, two touches split over two ACL packets\n");
        HciRawDecoder d;
        std::vector<uint8_t> r = { kHidDataInput, kMm2Report, 0x02, 0x0C, 0x00, 0xF6, 0xFF };   // right, dx 12, dy -10
        r.resize(1 + 14, 0);
        auto a = TouchRecord(-300, 500, 1, 0x40), b = TouchRecord(250, -100, 2, 0x30);
        r.insert(r.end(), a.begin(), a.end());
        r.insert(r.end(), b.begin(), b.end());
        auto l = L2cap(r);                                       // 35 bytes: more than a 27-byte packet
        std::vector<uint8_t> first(l.begin(), l.begin() + 27), rest(l.begin() + 27, l.end());
        Check(Feed(d, Acl(0x0C, 2, first), HciRawDecoder::Ignored), "first part held");
        std::vector<uint8_t> kb = { kHidDataInput, 0x01, 0, 0, 4, 0, 0, 0, 0, 0 };
        Check(Feed(d, Acl(0x0D, 2, L2cap(kb)), HciRawDecoder::Payload) && Frames(d, f, 2) == 0,
              "another device's report in between: decoded, no frame");
        Check(Feed(d, Acl(0x0C, 1, rest), HciRawDecoder::Payload) && d.Handle() == 0x0C, "second part completes it");
        int n = Frames(d, f, 2);
        Check(n == 1 && f[0].count == 2 && f[0].dx == 12 && f[0].dy == -10 && f[0].buttons == 2, "header: dx 12, dy -10, right button");
        Check(n == 1 && f[0].touch[0].x == -300 && f[0].touch[0].y == 500 && f[0].touch[1].x == 250 &&
              f[0].touch[1].y == -100 && f[0].touch[1].id == 2, "both touches intact");
    }
    {
        printf("-- MM2 motion beyond 10 bits is clamped\n");
        HciRawDecoder d;
        std::vector<uint8_t> r = { kHidDataInput, kMm2Report, 0, 0xE8, 0x03, 0x18, 0xFC };   // dx 1000, dy -1000
        r.resize(1 + 14, 0);
        auto a = TouchRecord(0, 0, 0, 0x40);
        r.insert(r.end(), a.begin(), a.end());
        Feed(d, Acl(0x0C, 2, L2cap(r)), HciRawDecoder::Payload);
        Check(Frames(d, f, 2) == 1 && f[0].dx == 511 && f[0].dy == -511, "dx 511, dy -511");
        std::vector<uint8_t> stock = { kHidDataInput, kMm2Report, 1, 5, 0, 5, 0, 0, 0 };
        Feed(d, Acl(0x0C, 2, L2cap(stock)), HciRawDecoder::Payload);
        Check(Frames(d, f, 2) == 0, "stock 8-byte 0x12 (no touches) is no frame");
    }
    {
        printf("-- two reports packed in F7, lift report\n");
        HciRawDecoder d;
        std::vector<uint8_t> one = { kMm1Touch, 1, 0, 0, 0, 0 }, two = { kMm1Touch, 2, 0, 0, 0, 0 };
        auto t = TouchRecord(10, 10, 1, 0x40);
        one.insert(one.end(), t.begin(), t.end());
        std::vector<uint8_t> r = { kHidDataInput, kMmDouble, (uint8_t)one.size() };
        r.insert(r.end(), one.begin(), one.end());
        r.insert(r.end(), two.begin(), two.end());
        Feed(d, Acl(0x0B, 2, L2cap(r)), HciRawDecoder::Payload);
        int n = Frames(d, f, 2);
        Check(n == 2 && f[0].dx == 1 && f[0].count == 1 && f[1].dx == 2 && f[1].count == 0, "both frames, in order");
        Feed(d, Acl(0x0B, 2, L2cap({ kHidDataInput, kMmLift, 1 })), HciRawDecoder::Payload);
        uint8_t out[2][kMaxFrame];
        uint32_t len[2];
        Check(MouseFrames(d.Data(), d.Length(), out, len) == 1 && len[0] == 2 && out[0][0] == kMmLift && out[0][1] == 1,
              "lift report passed on as 61 01");
    }
    {
        printf("-- what isn't a frame\n");
        HciRawDecoder d;
        std::vector<uint8_t> r = { kHidDataInput, kMm1Touch, 0, 0, 0, 0, 0 };
        auto l = L2cap(r);
        l.resize(4);                                            // the log kept only the header
        Check(Feed(d, Acl(0x0B, 2, l, 4 + 7), HciRawDecoder::Scrubbed), "blanked payload: Scrubbed");
        Check(Feed(d, Event402(kHciAclOut, { 0x0B, 0x20, 4, 0, 0, 0, 0x40, 0 }), HciRawDecoder::Ignored), "outgoing ACL ignored");
        Check(Feed(d, Event402(kHciEvent, { 0x05, 4, 0x00, 0x0B, 0x00, 0x13 }), HciRawDecoder::Disconnected) &&
              d.Handle() == 0x0B, "disconnection of handle 0x0B");
        Check(Feed(d, Event402(kHciEvent, { 0x05, 4, 0x08, 0x0B, 0x00, 0x13 }), HciRawDecoder::Ignored), "failed disconnection ignored");
        std::vector<uint8_t> tiny = { 0, 0, 0, kHciAclIn, 50, 0, 0, 0, 1, 2 };
        Check(d.Feed(tiny.data(), (uint32_t)tiny.size()) == HciRawDecoder::Ignored, "length past the end ignored");
        std::vector<uint8_t> ctl = { kHidDataFeature, 0x47, 80 };
        Check(Feed(d, Acl(0x0B, 2, L2cap(ctl)), HciRawDecoder::Payload) && Frames(d, f, 2) == 0, "feature reply: no frame");
        Check(Feed(d, Acl(0x0B, 1, { 1, 2, 3 }), HciRawDecoder::Ignored), "stray continuation ignored");
    }
    {
        printf("-- a disconnect drops a half-built packet\n");
        HciRawDecoder d;
        std::vector<uint8_t> r(1 + 14 + 16, 0);
        r[0] = kHidDataInput;
        r[1] = kMm2Report;
        auto l = L2cap(r);
        std::vector<uint8_t> first(l.begin(), l.begin() + 27), rest(l.begin() + 27, l.end());
        Feed(d, Acl(0x0C, 2, first), HciRawDecoder::Ignored);
        Feed(d, Event402(kHciEvent, { 0x05, 4, 0x00, 0x0C, 0x00, 0x13 }), HciRawDecoder::Disconnected);
        Check(Feed(d, Acl(0x0C, 1, rest), HciRawDecoder::Ignored), "the rest after reconnecting is dropped");
    }
    printf("\n%d failure(s)\n", g_failures);
    return g_failures;
}
