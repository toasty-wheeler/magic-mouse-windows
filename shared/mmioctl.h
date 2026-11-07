// mmioctl.h - interface between mmfilter.sys and the MagicMouseNative tray app.
//
// The filter exposes a control device, \\.\MagicMouseNative. Include <windows.h> (user mode)
// or <ntddk.h> (kernel) first.
//
//   IOCTL_MM_GET_STATUS    out: MM_STATUS
//   IOCTL_MM_READ_FRAMES   out: MM_FRAME[n]; pends until at least one frame is queued
//   IOCTL_MM_ENABLE_TOUCH  no buffers; resends D7 01 (multi-touch on) to the mouse
//   IOCTL_MM_GET_LOG       out: MM_LOG, the recent HID control channel traffic
//   IOCTL_MM_INJECT_BUTTONS in: MM_INJECT; sends button presses as the mouse's own reports
//   IOCTL_MM_SET_OPTIONS   in: ULONG MM_OPTION_* flags

#pragma once

#define MM_INTERFACE_VERSION  1
#define MM_DEVICE_PATH        L"\\\\.\\MagicMouseNative"

#define IOCTL_MM_GET_STATUS   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MM_READ_FRAMES  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MM_ENABLE_TOUCH CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_MM_GET_LOG      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_MM_INJECT_BUTTONS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_MM_SET_OPTIONS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_WRITE_ACCESS)

// IOCTL_MM_SET_OPTIONS in: ULONG of these flags (kept while the driver is loaded).
#define MM_OPTION_SWAP_BUTTONS  0x01    // left-handed: the mouse's own left / right buttons swap

// Button states for the filter to send as the mouse's own reports (one stock report 0x10
// each, physical buttons still held added in), so they reach every window like a real click,
// elevated ones included. A click is { 2, { MM_BUTTON_LEFT, 0 } }.
#define MM_INJECT_MAX     8
#define MM_BUTTON_LEFT    0x01
#define MM_BUTTON_RIGHT   0x02
typedef struct _MM_INJECT {
    ULONG Count;
    UCHAR Buttons[MM_INJECT_MAX];
} MM_INJECT;

#define MM_REPORT_TOUCH  0x29                          // MM1 multi-touch input report
#define MM_MAX_TOUCHES   16
#define MM_MAX_REPORT    (6 + MM_MAX_TOUCHES * 8)      // ID, 5 header bytes, 8 bytes per touch

// One raw input report exactly as the mouse sent it, stamped with the performance counter
// (same clock as QueryPerformanceCounter in user mode). Report[0] is the report ID: mostly
// 0x29 (touches), plus any report the filter doesn't otherwise handle (e.g. 0x61).
typedef struct _MM_FRAME {
    ULONG    Sequence;
    USHORT   Length;                // valid bytes in Report
    USHORT   Reserved;
    LONGLONG Qpc;
    UCHAR    Report[MM_MAX_REPORT];
} MM_FRAME;

// Internal IOCTLs the filter has seen HidBth send (for working out the Stage 2 descriptor
// path). BRBs are recorded as MM_SEEN_BRB | brb type, everything else by IOCTL code.
#define MM_SEEN_BRB  0xB0000000u
#define MM_SEEN_MAX  16

typedef struct _MM_SEEN {
    ULONG Code;
    ULONG Count;
} MM_SEEN;

// Control channel log (oldest first). Kinds:
enum {
    MM_EV_HIDBTH_SEND = 1,      // HidBth wrote a control request (Data = request)
    MM_EV_HIDBTH_READ,          // HidBth posted a control read (Length = its buffer size)
    MM_EV_REPLY_TO_HIDBTH,      // a control packet went to HidBth
    MM_EV_REPLY_OURS,           // a control packet was the reply to our D7 01: swallowed
    MM_EV_DROPPED,              // a bounced packet didn't fit HidBth's buffer: dropped
    MM_EV_READ_FAILED,          // a control read failed (Status)
    MM_EV_OUR_SEND,             // we sent a request (D7 01, or 43 47 for the battery)
    MM_EV_OUR_SEND_DONE,        // (Status)
};
#define MM_EV_FLAG_BOUNCED   0x01
#define MM_EV_FLAG_CONTROL   0x02

typedef struct _MM_EVENT {
    ULONG  TimeMs;              // interrupt time
    UCHAR  Kind;
    UCHAR  Flags;
    USHORT Length;
    LONG   Status;
    UCHAR  Data[8];
} MM_EVENT;

#define MM_LOG_EVENTS  64
#define MM_LOG_HIDBTH_WAITING   0x01
#define MM_LOG_OUR_REPLY        0x02
#define MM_LOG_SEND_BUSY        0x04

typedef struct _MM_LOG {
    ULONG    Count;             // valid entries in Event
    ULONG    Flags;             // MM_LOG_* now
    LONG     HidbthControlReads;
    MM_EVENT Event[MM_LOG_EVENTS];
} MM_LOG;

typedef struct _MM_STATUS {
    ULONG   Version;                // MM_INTERFACE_VERSION
    ULONG   Attached;               // mice the filter is loaded on
    ULONG   ControlOpen;            // HID control channel (PSM 0x11) known
    ULONG   InterruptOpen;          // HID interrupt channel (PSM 0x13) known
    ULONG   TouchActive;            // report 0x29 seen since the channels opened
    ULONG   EnableAttempts;         // D7 01 sends since the channels opened
    LONG    LastEnableStatus;       // NTSTATUS of the last D7 01 send
    ULONG   FramesTotal;
    ULONG   FramesDropped;          // overwritten before the app read them
    ULONG   SeenCount;
    MM_SEEN Seen[MM_SEEN_MAX];
    ULONG   ChannelOpens;           // L2CAP open / open-response completions seen
    ULONG   ChannelCloses;
    ULONG   D0Exits;                // power transitions (channels survive these)
    ULONG   LastOpenType;           // last open / open-response BRB as completed
    LONG    LastOpenIoStatus;
    LONG    LastOpenBrbStatus;
    ULONG   LastOpenResponse;
    ULONG   LastOpenHadHandle;
    ULONG   ReadErrors;             // interrupt-channel reads that failed
    LONG    LastReadError;
    ULONG   OtherReports;           // input reports with an ID the filter doesn't handle
    ULONG   LastOtherId;
    LONG    Battery;                // %, read by the filter itself (feature report 0x47); -1 unknown
    ULONG   Injected;               // button reports sent for IOCTL_MM_INJECT_BUTTONS
} MM_STATUS;
