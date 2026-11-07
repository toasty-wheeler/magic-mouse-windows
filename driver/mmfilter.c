// mmfilter.c - Stage 1 KMDF lower filter for Apple Magic Mouse 2009 (A1296) on Bluetooth.
//
// Installed below HidBth on the mouse's BTHENUM device, it sees the L2CAP traffic HidBth
// submits as BRBs (IOCTL_INTERNAL_BTH_SUBMIT_BRB):
//   1. Learns which channel is HID control (PSM 0x11) and which is interrupt (PSM 0x13).
//   2. Once both are open, sends SET_REPORT(Feature) D7 01 on the control channel to turn
//      on multi-touch (as Linux hid-magicmouse does) and swallows the mouse's HANDSHAKE
//      reply, so HidBth never sees an answer to a request it didn't make.
//   3. In multi-touch mode the mouse sends buttons, motion and touches in report 0x29,
//      which the stock descriptor doesn't declare. Each 0x29 is queued for the tray app and
//      rewritten in place as stock report 0x10, so mouhid keeps moving the pointer.
//   4. Exposes \\.\MagicMouseNative (see shared\mmioctl.h) for the tray app.
//
// The report descriptor is left alone in Stage 1. The filter records which internal
// IOCTLs HidBth sends so the descriptor path for Stage 2 can be found.

#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include <initguid.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <bthddi.h>
#include "..\shared\mmioctl.h"

#define MM_POOL_TAG          'fMmM'
#define HIDP_HANDSHAKE       0x00    // high nibble of a HIDP transaction header
#define HIDP_SET_FEATURE     0x53    // SET_REPORT | Feature
#define HIDP_DATA_INPUT      0xA1    // DATA | Input
#define MM_REPORT_MOUSE      0x10    // stock report: buttons, X, Y (16-bit each)
#define MM_REPORT_DOUBLE     0xF7    // two touch reports in one packet: F7, len1, r1, r2
#define MM_BOUNCE_SIZE       512
#define MM_ENABLE_DELAY_MS   300     // after the interrupt channel opens
#define MM_ENABLE_RETRY_MS   1000
#define MM_ENABLE_TRIES      4
#define MM_BUSY_RETRY_MS     50      // control channel busy: try again this soon
#define MM_CDO_RETRY_MS      2000    // side channel name still taken: try again
#define MM_BATTERY_FIRST_MS  2000    // first battery read after connecting
#define MM_BATTERY_POLL_MS   60000
#define HIDP_GET_FEATURE     0x43    // GET_REPORT | Feature
#define HIDP_DATA_FEATURE    0xA3    // DATA | Feature
#define MM_REPORT_BATTERY    0x47    // feature: Battery Strength, 0-100 %
#define MM_REQ_ENABLE        1       // our requests on the control channel
#define MM_REQ_BATTERY       2
#define MM_TRACKED_READS     8       // HidBth interrupt reads tracked for injection
#define MM_REPLY_TIMEOUT     (1 * 1000 * 10000LL)   // 1 s in 100 ns units
#define MM_HIDBTH_TIMEOUT    (3 * 1000 * 10000LL)
#define MM_RING_FRAMES       128

typedef struct _BRB_L2CA_ACL_TRANSFER MM_ACL_BRB;

typedef struct _MM_FILTER_CONTEXT {
    WDFDEVICE   Device;
    WDFIOTARGET Target;
    BTH_PROFILE_DRIVER_INTERFACE Bth;
    BOOLEAN     HaveBth;
    WDFTIMER    EnableTimer;
    WDFTIMER    CdoTimer;               // passive level: retries creating the control device

    WDFSPINLOCK Lock;                   // guards everything below
    L2CAP_CHANNEL_HANDLE Control;
    L2CAP_CHANNEL_HANDLE Interrupt;
    L2CAP_CHANNEL_HANDLE Recent[2];     // last two channels opened or used, newest first
    BTH_ADDR    BtAddress;
    ULONG       ChannelOpens;
    ULONG       ChannelCloses;
    ULONG       D0Exits;
    ULONG       LastOpenType;
    NTSTATUS    LastOpenIoStatus;
    NTSTATUS    LastOpenBrbStatus;
    ULONG       LastOpenResponse;
    BOOLEAN     LastOpenHadHandle;
    ULONG       ReadErrors;
    NTSTATUS    LastReadError;
    ULONG       OtherReports;
    ULONG       LastOtherId;
    LONG        HidbthControlReads;     // HidBth reads pending on the control channel
    // HID control transactions are answered in order, one reply each. We only send D7 01
    // while HidBth isn't waiting for a reply, so the next reply after it is ours.
    BOOLEAN     HidbthWaiting;          // HidBth sent a request and hasn't had its reply
    LONGLONG    HidbthWaitingUntil;     // interrupt time
    BOOLEAN     OurReplyPending;
    LONGLONG    OurReplyUntil;
    BOOLEAN     TouchActive;
    BOOLEAN     TouchAcked;             // the mouse answered our D7 01 with HANDSHAKE success
    UCHAR       OurRequest;             // MM_REQ_*: what OurReplyPending is waiting for
    LONG        Battery;                // %, -1 until read
    WDFTIMER    BatteryTimer;
    ULONG       EnableAttempts;
    NTSTATUS    LastEnableStatus;

    // Part of a control packet that didn't fit HidBth's read, for its next control read.
    UCHAR       ControlTail[MM_BOUNCE_SIZE];
    ULONG       ControlTailLen;

    // Our own control-channel request (D7 01 or battery); one in flight.
    BOOLEAN     SendBusy;
    WDFREQUEST  SendRequest;
    WDFMEMORY   SendMemory;
    MM_ACL_BRB  SendBrb;
    UCHAR       SendBuf[3];

    // Injected buttons (IOCTL_MM_INJECT_BUTTONS): HidBth's interrupt reads that are down at
    // BthPort; one is cancelled and completed with the next queued button state.
    WDFREQUEST  Reads[MM_TRACKED_READS];
    UCHAR       InjectQueue[MM_INJECT_MAX];
    ULONG       InjectCount;
    BOOLEAN     InjectBusy;             // a read is being cancelled for an injection
    UCHAR       PhysButtons;            // buttons held on the mouse itself (latest report)
    ULONG       Injected;

    WDFSPINLOCK LogLock;                // control channel log; never held with Lock
    ULONG       LogNext;
    MM_EVENT    Log[MM_LOG_EVENTS];
} MM_FILTER_CONTEXT, *PMM_FILTER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MM_FILTER_CONTEXT, MmGetFilterContext)

typedef struct _MM_REQUEST_CONTEXT {
    PBRB       Brb;
    MM_ACL_BRB Saved;                   // the ACL BRB as HidBth submitted it, for resubmits
    BOOLEAN    ControlRead;
    BOOLEAN    InterruptRead;           // tracked in Reads while down at BthPort
    // Picked for an injection: both the canceller and the completion routine hold a
    // reference, and whichever drops the last one completes the read with Final*.
    BOOLEAN    Inject;
    LONG       InjectRefs;
    NTSTATUS   FinalStatus;
    ULONG_PTR  FinalInfo;
    // HidBth sizes its reads for its own reports (6 bytes on interrupt, 1 for its standing
    // control read), so a 0x29 carrying touches or a reply to our request won't fit: every
    // read goes down with this buffer instead and is copied back.
    UCHAR      Bounce[MM_BOUNCE_SIZE];
} MM_REQUEST_CONTEXT, *PMM_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MM_REQUEST_CONTEXT, MmGetRequestContext)

// Driver-wide state: the control device and the frame ring it serves.
static struct {
    WDFSPINLOCK Lock;                   // guards everything below except Cdo/ReadQueue
    WDFWAITLOCK CdoLock;
    WDFDEVICE   Cdo;
    WDFQUEUE    ReadQueue;
    PMM_FILTER_CONTEXT Filter;          // most recently attached mouse
    ULONG       Attached;
    ULONG       Options;                // MM_OPTION_* from the app
    ULONG       Head, Count, Sequence;
    ULONG       FramesTotal, FramesDropped;
    ULONG       SeenCount;
    MM_SEEN     Seen[MM_SEEN_MAX];
    MM_FRAME    Ring[MM_RING_FRAMES];
} g_Mm;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MmEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP MmEvtFilterCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE MmEvtPrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY MmEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT MmEvtD0Exit;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL MmEvtIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL MmEvtCdoDeviceControl;
EVT_WDF_REQUEST_COMPLETION_ROUTINE MmAclReadComplete;
EVT_WDF_REQUEST_COMPLETION_ROUTINE MmOpenComplete;
EVT_WDF_REQUEST_COMPLETION_ROUTINE MmSendComplete;
EVT_WDF_TIMER MmEnableTimer;
EVT_WDF_TIMER MmBatteryTimer;
EVT_WDF_TIMER MmCdoRetryTimer;
static VOID MmFillLog(PMM_FILTER_CONTEXT c, MM_LOG* log);
static NTSTATUS MmQueueInject(PMM_FILTER_CONTEXT c, const MM_INJECT* In);
static VOID MmKickInject(PMM_FILTER_CONTEXT c);

// ---------------------------------------------------------------------------------------
// Side channel: frame ring, seen-IOCTL table, control device

static VOID MmRecordSeen(ULONG Code)
{
    ULONG i;
    BOOLEAN isNew = FALSE;

    WdfSpinLockAcquire(g_Mm.Lock);
    for (i = 0; i < g_Mm.SeenCount && g_Mm.Seen[i].Code != Code; i++) {}
    if (i < g_Mm.SeenCount) {
        g_Mm.Seen[i].Count++;
    } else if (g_Mm.SeenCount < MM_SEEN_MAX) {
        g_Mm.Seen[g_Mm.SeenCount].Code = Code;
        g_Mm.Seen[g_Mm.SeenCount].Count = 1;
        g_Mm.SeenCount++;
        isNew = TRUE;
    }
    WdfSpinLockRelease(g_Mm.Lock);

    if (isNew) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "mmfilter: first internal request 0x%08X\n", Code);
    }
}

// Completes a READ_FRAMES request with as many queued frames as fit (possibly none).
static VOID MmCompleteRead(WDFREQUEST Request)
{
    MM_FRAME* out;
    size_t outLen;
    ULONG n = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request, sizeof(MM_FRAME), (PVOID*)&out, &outLen);

    if (NT_SUCCESS(status)) {
        WdfSpinLockAcquire(g_Mm.Lock);
        while (g_Mm.Count > 0 && (n + 1) * sizeof(MM_FRAME) <= outLen) {
            out[n++] = g_Mm.Ring[g_Mm.Head];
            g_Mm.Head = (g_Mm.Head + 1) % MM_RING_FRAMES;
            g_Mm.Count--;
        }
        WdfSpinLockRelease(g_Mm.Lock);
    }
    WdfRequestCompleteWithInformation(Request, status, n * sizeof(MM_FRAME));
}

// Hands queued frames to pending READ_FRAMES requests.
static VOID MmDrainReads(VOID)
{
    WDFREQUEST request;
    BOOLEAN have;

    if (!g_Mm.ReadQueue) return;
    for (;;) {
        WdfSpinLockAcquire(g_Mm.Lock);
        have = g_Mm.Count > 0;
        WdfSpinLockRelease(g_Mm.Lock);
        if (!have || !NT_SUCCESS(WdfIoQueueRetrieveNextRequest(g_Mm.ReadQueue, &request))) return;
        MmCompleteRead(request);
    }
}

static VOID MmPushFrame(const UCHAR* Report, ULONG Length)
{
    ULONG slot;
    LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);

    WdfSpinLockAcquire(g_Mm.Lock);
    if (g_Mm.Count == MM_RING_FRAMES) {         // full: overwrite the oldest
        slot = g_Mm.Head;
        g_Mm.Head = (g_Mm.Head + 1) % MM_RING_FRAMES;
        g_Mm.FramesDropped++;
    } else {
        slot = (g_Mm.Head + g_Mm.Count) % MM_RING_FRAMES;
        g_Mm.Count++;
    }
    g_Mm.Ring[slot].Sequence = ++g_Mm.Sequence;
    g_Mm.Ring[slot].Length = (USHORT)Length;
    g_Mm.Ring[slot].Reserved = 0;
    g_Mm.Ring[slot].Qpc = qpc.QuadPart;
    RtlCopyMemory(g_Mm.Ring[slot].Report, Report, Length);
    RtlZeroMemory(g_Mm.Ring[slot].Report + Length, MM_MAX_REPORT - Length);
    g_Mm.FramesTotal++;
    WdfSpinLockRelease(g_Mm.Lock);

    MmDrainReads();
}

static VOID MmFillStatus(MM_STATUS* s)
{
    PMM_FILTER_CONTEXT c;

    RtlZeroMemory(s, sizeof(*s));
    WdfSpinLockAcquire(g_Mm.Lock);
    s->Version = MM_INTERFACE_VERSION;
    s->Attached = g_Mm.Attached;
    s->FramesTotal = g_Mm.FramesTotal;
    s->FramesDropped = g_Mm.FramesDropped;
    s->SeenCount = g_Mm.SeenCount;
    RtlCopyMemory(s->Seen, g_Mm.Seen, sizeof(s->Seen));
    s->Battery = -1;
    c = g_Mm.Filter;
    if (c) {
        WdfSpinLockAcquire(c->Lock);
        s->ControlOpen = c->Control != NULL;
        s->InterruptOpen = c->Interrupt != NULL;
        s->TouchActive = c->TouchActive;
        s->EnableAttempts = c->EnableAttempts;
        s->LastEnableStatus = c->LastEnableStatus;
        s->ChannelOpens = c->ChannelOpens;
        s->ChannelCloses = c->ChannelCloses;
        s->D0Exits = c->D0Exits;
        s->LastOpenType = c->LastOpenType;
        s->LastOpenIoStatus = c->LastOpenIoStatus;
        s->LastOpenBrbStatus = c->LastOpenBrbStatus;
        s->LastOpenResponse = c->LastOpenResponse;
        s->LastOpenHadHandle = c->LastOpenHadHandle;
        s->ReadErrors = c->ReadErrors;
        s->LastReadError = c->LastReadError;
        s->OtherReports = c->OtherReports;
        s->LastOtherId = c->LastOtherId;
        s->Battery = c->Battery;
        s->Injected = c->Injected;
        WdfSpinLockRelease(c->Lock);
    }
    WdfSpinLockRelease(g_Mm.Lock);
}

VOID MmEvtCdoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength,
                           size_t InputBufferLength, ULONG IoControlCode)
{
    NTSTATUS status;
    MM_STATUS* s;
    PMM_FILTER_CONTEXT c;
    BOOLEAN have;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_MM_GET_STATUS:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(MM_STATUS), (PVOID*)&s, NULL);
        if (!NT_SUCCESS(status)) break;
        MmFillStatus(s);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(MM_STATUS));
        return;

    case IOCTL_MM_READ_FRAMES:
        if (OutputBufferLength < sizeof(MM_FRAME)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        WdfSpinLockAcquire(g_Mm.Lock);
        have = g_Mm.Count > 0;
        WdfSpinLockRelease(g_Mm.Lock);
        if (have) { MmCompleteRead(Request); return; }
        status = WdfRequestForwardToIoQueue(Request, g_Mm.ReadQueue);
        if (!NT_SUCCESS(status)) break;
        MmDrainReads();     // a frame may have arrived before the request was queued
        return;

    case IOCTL_MM_GET_LOG: {
        MM_LOG* log;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(MM_LOG), (PVOID*)&log, NULL);
        if (!NT_SUCCESS(status)) break;
        status = STATUS_DEVICE_NOT_CONNECTED;
        WdfSpinLockAcquire(g_Mm.Lock);
        c = g_Mm.Filter;
        if (c) {
            MmFillLog(c, log);
            status = STATUS_SUCCESS;
        }
        WdfSpinLockRelease(g_Mm.Lock);
        if (!NT_SUCCESS(status)) break;
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(MM_LOG));
        return;
    }

    case IOCTL_MM_SET_OPTIONS: {
        ULONG* options;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID*)&options, NULL);
        if (NT_SUCCESS(status)) g_Mm.Options = *options;
        break;
    }

    case IOCTL_MM_INJECT_BUTTONS: {
        MM_INJECT* in;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(MM_INJECT), (PVOID*)&in, NULL);
        if (!NT_SUCCESS(status)) break;
        if (in->Count == 0 || in->Count > MM_INJECT_MAX) { status = STATUS_INVALID_PARAMETER; break; }
        // Keep the mouse's device (and its context) alive without holding g_Mm.Lock, which
        // the injection's completion may need.
        WdfSpinLockAcquire(g_Mm.Lock);
        c = g_Mm.Filter;
        if (c) WdfObjectReference(c->Device);
        WdfSpinLockRelease(g_Mm.Lock);
        if (!c) { status = STATUS_DEVICE_NOT_CONNECTED; break; }
        status = MmQueueInject(c, in);
        WdfObjectDereference(c->Device);
        break;
    }

    case IOCTL_MM_ENABLE_TOUCH:
        status = STATUS_DEVICE_NOT_CONNECTED;
        WdfSpinLockAcquire(g_Mm.Lock);
        c = g_Mm.Filter;
        if (c) {
            WdfSpinLockAcquire(c->Lock);
            c->TouchActive = FALSE;
            c->TouchAcked = FALSE;
            c->EnableAttempts = 0;
            WdfSpinLockRelease(c->Lock);
            WdfTimerStart(c->EnableTimer, WDF_REL_TIMEOUT_IN_MS(1));
            status = STATUS_SUCCESS;
        }
        WdfSpinLockRelease(g_Mm.Lock);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    WdfRequestComplete(Request, status);
}

static NTSTATUS MmCreateControlDevice(WDFDRIVER Driver)
{
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\MagicMouseNative");
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\MagicMouseNative");
    PWDFDEVICE_INIT init;
    WDFDEVICE cdo = NULL;
    WDF_IO_QUEUE_CONFIG qc;
    NTSTATUS status;

    // Any user may read frames and status; the tray app runs unelevated.
    init = WdfControlDeviceInitAllocate(Driver, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    WdfDeviceInitSetExclusive(init, FALSE);
    status = WdfDeviceInitAssignName(init, &name);
    if (NT_SUCCESS(status)) status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &cdo);
    if (!NT_SUCCESS(status)) {
        if (init) WdfDeviceInitFree(init);
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(cdo, &link);
    if (NT_SUCCESS(status)) {
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
        qc.EvtIoDeviceControl = MmEvtCdoDeviceControl;
        qc.PowerManaged = WdfFalse;
        status = WdfIoQueueCreate(cdo, &qc, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    }
    if (NT_SUCCESS(status)) {
        WDF_IO_QUEUE_CONFIG_INIT(&qc, WdfIoQueueDispatchManual);
        qc.PowerManaged = WdfFalse;
        status = WdfIoQueueCreate(cdo, &qc, WDF_NO_OBJECT_ATTRIBUTES, &g_Mm.ReadQueue);
    }
    if (!NT_SUCCESS(status)) {
        g_Mm.ReadQueue = NULL;
        WdfObjectDelete(cdo);
        return status;
    }

    WdfControlFinishInitializing(cdo);
    g_Mm.Cdo = cdo;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Channel tracking and multi-touch enable

// Records one control channel event (see MM_EV_* in mmioctl.h). Call without c->Lock held.
static VOID MmLog(PMM_FILTER_CONTEXT c, UCHAR Kind, UCHAR Flags, ULONG Length, NTSTATUS Status, const UCHAR* Data)
{
    MM_EVENT* e;

    if (!c->LogLock) return;
    WdfSpinLockAcquire(c->LogLock);
    e = &c->Log[c->LogNext++ % MM_LOG_EVENTS];
    RtlZeroMemory(e, sizeof(*e));
    e->TimeMs = (ULONG)(KeQueryInterruptTime() / 10000);
    e->Kind = Kind;
    e->Flags = Flags;
    e->Length = (USHORT)min(Length, 0xFFFF);
    e->Status = Status;
    if (Data) RtlCopyMemory(e->Data, Data, min(Length, sizeof(e->Data)));
    WdfSpinLockRelease(c->LogLock);
}

static VOID MmFillLog(PMM_FILTER_CONTEXT c, MM_LOG* log)
{
    ULONG i, first;

    RtlZeroMemory(log, sizeof(*log));
    WdfSpinLockAcquire(c->Lock);
    log->Flags = (c->HidbthWaiting ? MM_LOG_HIDBTH_WAITING : 0) | (c->OurReplyPending ? MM_LOG_OUR_REPLY : 0) |
                 (c->SendBusy ? MM_LOG_SEND_BUSY : 0);
    log->HidbthControlReads = c->HidbthControlReads;
    WdfSpinLockRelease(c->Lock);

    WdfSpinLockAcquire(c->LogLock);
    log->Count = min(c->LogNext, MM_LOG_EVENTS);
    first = c->LogNext - log->Count;
    for (i = 0; i < log->Count; i++) log->Event[i] = c->Log[(first + i) % MM_LOG_EVENTS];
    WdfSpinLockRelease(c->LogLock);
}

static PUCHAR MmAclBuffer(MM_ACL_BRB* Acl)
{
    if (Acl->Buffer) return (PUCHAR)Acl->Buffer;
    if (Acl->BufferMDL) return (PUCHAR)MmGetSystemAddressForMdlSafe(Acl->BufferMDL, NormalPagePriority | MdlMappingNoExecute);
    return NULL;
}

// Submits one of our own ACL BRBs on the control channel.
static BOOLEAN MmSubmitAcl(PMM_FILTER_CONTEXT c, WDFREQUEST Request, WDFMEMORY Memory, MM_ACL_BRB* Brb,
                           L2CAP_CHANNEL_HANDLE Channel, BTH_ADDR Address, ULONG Flags, PVOID Buffer,
                           ULONG Length, PFN_WDF_REQUEST_COMPLETION_ROUTINE Done)
{
    WDF_REQUEST_REUSE_PARAMS reuse;

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    if (!NT_SUCCESS(WdfRequestReuse(Request, &reuse))) return FALSE;

    c->Bth.BthReuseBrb((PBRB)Brb, BRB_L2CA_ACL_TRANSFER);
    Brb->BtAddress = Address;
    Brb->ChannelHandle = Channel;
    Brb->TransferFlags = Flags;
    Brb->BufferSize = Length;
    Brb->Buffer = Buffer;
    Brb->BufferMDL = NULL;
    Brb->Timeout = (Flags & ACL_TRANSFER_TIMEOUT) ? 1000 : 0;

    if (!NT_SUCCESS(WdfIoTargetFormatRequestForInternalIoctlOthers(c->Target, Request, IOCTL_INTERNAL_BTH_SUBMIT_BRB,
                                                                  Memory, NULL, NULL, NULL, NULL, NULL))) {
        return FALSE;
    }
    WdfRequestSetCompletionRoutine(Request, Done, c);
    return WdfRequestSend(Request, c->Target, WDF_NO_SEND_OPTIONS);
}

// Called with c->Lock held. Expires waits whose reply never came.
static BOOLEAN MmControlBusy(PMM_FILTER_CONTEXT c)
{
    LONGLONG now = (LONGLONG)KeQueryInterruptTime();

    if (c->HidbthWaiting && now > c->HidbthWaitingUntil) c->HidbthWaiting = FALSE;
    if (c->OurReplyPending && now > c->OurReplyUntil) c->OurReplyPending = FALSE;
    return c->SendBusy || c->HidbthWaiting || c->OurReplyPending;
}

// Called with c->Lock held for each packet read from the control channel. Returns TRUE if
// it is the reply to our own request (to be swallowed, after taking what we asked for);
// any other reply is HidBth's.
static BOOLEAN MmIsOurReply(PMM_FILTER_CONTEXT c, const UCHAR* Buf, ULONG Len)
{
    UCHAR type = Buf[0] >> 4;

    if (type != (HIDP_HANDSHAKE >> 4) && type != (HIDP_DATA_INPUT >> 4)) return FALSE;  // not a reply
    MmControlBusy(c);
    if (!c->OurReplyPending) {
        c->HidbthWaiting = FALSE;
        return FALSE;
    }
    c->OurReplyPending = FALSE;
    if (c->OurRequest == MM_REQ_ENABLE && Buf[0] == HIDP_HANDSHAKE) {
        c->TouchAcked = TRUE;                   // HANDSHAKE SUCCESSFUL
    } else if (c->OurRequest == MM_REQ_BATTERY && Len >= 3 && Buf[0] == HIDP_DATA_FEATURE &&
               Buf[1] == MM_REPORT_BATTERY) {
        c->Battery = min(Buf[2], 100);
    }
    c->OurRequest = 0;
    return TRUE;
}

// HidBth is sending on Acl: if it's a control request that expects a reply (GET/SET_REPORT,
// GET/SET_PROTOCOL, GET/SET_IDLE), note that HidBth is waiting for it.
static VOID MmNoteHidbthRequest(PMM_FILTER_CONTEXT c, MM_ACL_BRB* Acl)
{
    PUCHAR buf = Acl->BufferSize ? MmAclBuffer(Acl) : NULL;
    UCHAR type = buf ? buf[0] >> 4 : 0;
    BOOLEAN control, interrupt;

    WdfSpinLockAcquire(c->Lock);
    control = c->Control && Acl->ChannelHandle == c->Control;
    interrupt = c->Interrupt && Acl->ChannelHandle == c->Interrupt;
    if (control && type >= 0x4 && type <= 0x9) {
        c->HidbthWaiting = TRUE;
        c->HidbthWaitingUntil = (LONGLONG)KeQueryInterruptTime() + MM_HIDBTH_TIMEOUT;
    }
    WdfSpinLockRelease(c->Lock);
    if (!interrupt) MmLog(c, MM_EV_HIDBTH_SEND, control ? MM_EV_FLAG_CONTROL : 0, Acl->BufferSize, 0, buf);
}

// Sends one of our requests on the control channel: SET_REPORT(Feature) D7 01 (multi-touch
// on) or GET_REPORT(Feature) 0x47 (battery). The reply lands in HidBth's standing control
// read, where MmAclReadComplete takes and swallows it. Returns FALSE if the channel is busy
// (try again soon).
static BOOLEAN MmSendRequest(PMM_FILTER_CONTEXT c, UCHAR Request)
{
    L2CAP_CHANNEL_HANDLE control;
    BTH_ADDR address;
    ULONG len;

    WdfSpinLockAcquire(c->Lock);
    if (!c->HaveBth || !c->Control) {
        WdfSpinLockRelease(c->Lock);
        return TRUE;
    }
    if (MmControlBusy(c)) {
        WdfSpinLockRelease(c->Lock);
        return FALSE;
    }
    c->SendBusy = TRUE;
    if (Request == MM_REQ_ENABLE) c->EnableAttempts++;
    c->OurRequest = Request;
    c->OurReplyPending = TRUE;
    c->OurReplyUntil = (LONGLONG)KeQueryInterruptTime() + MM_REPLY_TIMEOUT;
    control = c->Control;
    address = c->BtAddress;
    WdfSpinLockRelease(c->Lock);

    if (Request == MM_REQ_ENABLE) {
        c->SendBuf[0] = HIDP_SET_FEATURE;
        c->SendBuf[1] = 0xD7;
        c->SendBuf[2] = 0x01;
        len = 3;
    } else {
        c->SendBuf[0] = HIDP_GET_FEATURE;
        c->SendBuf[1] = MM_REPORT_BATTERY;
        len = 2;
    }
    if (!MmSubmitAcl(c, c->SendRequest, c->SendMemory, &c->SendBrb, control, address,
                     ACL_TRANSFER_DIRECTION_OUT, c->SendBuf, len, MmSendComplete)) {
        WdfSpinLockAcquire(c->Lock);
        c->SendBusy = FALSE;
        c->OurReplyPending = FALSE;
        c->OurRequest = 0;
        if (Request == MM_REQ_ENABLE) c->LastEnableStatus = STATUS_UNSUCCESSFUL;
        WdfSpinLockRelease(c->Lock);
        return TRUE;
    }
    MmLog(c, MM_EV_OUR_SEND, 0, len, 0, c->SendBuf);
    return TRUE;
}

VOID MmSendComplete(WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    PMM_FILTER_CONTEXT c = (PMM_FILTER_CONTEXT)Context;
    NTSTATUS status = Params->IoStatus.Status;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    WdfSpinLockAcquire(c->Lock);
    c->SendBusy = FALSE;
    if (c->OurRequest == MM_REQ_ENABLE) c->LastEnableStatus = status;
    if (!NT_SUCCESS(status)) {              // never reached the mouse
        c->OurReplyPending = FALSE;
        c->OurRequest = 0;
    }
    WdfSpinLockRelease(c->Lock);
    MmLog(c, MM_EV_OUR_SEND_DONE, 0, 0, status, NULL);
}

VOID MmEnableTimer(WDFTIMER Timer)
{
    PMM_FILTER_CONTEXT c = MmGetFilterContext((WDFDEVICE)WdfTimerGetParentObject(Timer));
    BOOLEAN again;

    WdfSpinLockAcquire(c->Lock);
    again = c->Control && c->Interrupt && !c->TouchActive && !c->TouchAcked && c->EnableAttempts < MM_ENABLE_TRIES;
    WdfSpinLockRelease(c->Lock);
    if (!again) return;

    WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(MmSendRequest(c, MM_REQ_ENABLE) ? MM_ENABLE_RETRY_MS : MM_BUSY_RETRY_MS));
}

// Reads the battery once a minute while connected; stops when the channels close (it is
// restarted with the enable timer on the next connection).
VOID MmBatteryTimer(WDFTIMER Timer)
{
    PMM_FILTER_CONTEXT c = MmGetFilterContext((WDFDEVICE)WdfTimerGetParentObject(Timer));
    BOOLEAN connected;

    WdfSpinLockAcquire(c->Lock);
    connected = c->Control && c->Interrupt;
    WdfSpinLockRelease(c->Lock);
    if (!connected) return;

    WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(MmSendRequest(c, MM_REQ_BATTERY) ? MM_BATTERY_POLL_MS : MM_BUSY_RETRY_MS));
}

// Starts enabling multi-touch and reading the battery on a (new) connection.
static VOID MmStartTimers(PMM_FILTER_CONTEXT c)
{
    WdfTimerStart(c->EnableTimer, WDF_REL_TIMEOUT_IN_MS(MM_ENABLE_DELAY_MS));
    WdfTimerStart(c->BatteryTimer, WDF_REL_TIMEOUT_IN_MS(MM_BATTERY_FIRST_MS));
}

// Called with c->Lock held after Control or Interrupt changed. Returns TRUE if both are now
// known and touch still needs enabling (the caller starts the timer after unlocking).
static BOOLEAN MmChannelsChanged(PMM_FILTER_CONTEXT c)
{
    if (!c->Control || !c->Interrupt || c->Control == c->Interrupt || c->TouchActive) return FALSE;
    c->EnableAttempts = 0;
    c->TouchAcked = FALSE;
    return TRUE;
}

// Called with c->Lock held: moves Channel to the front of Recent.
static VOID MmRemember(PMM_FILTER_CONTEXT c, L2CAP_CHANNEL_HANDLE Channel)
{
    if (c->Recent[0] == Channel) return;
    c->Recent[1] = c->Recent[0];
    c->Recent[0] = Channel;
}

static VOID MmChannelOpened(PMM_FILTER_CONTEXT c, L2CAP_CHANNEL_HANDLE Channel, USHORT Psm, BTH_ADDR Address)
{
    BOOLEAN both;

    WdfSpinLockAcquire(c->Lock);
    c->BtAddress = Address;
    c->ChannelOpens++;
    MmRemember(c, Channel);
    if (Psm == PSM_HID_CONTROL) {
        c->Control = Channel;
    } else if (Psm == PSM_HID_INTERRUPT) {
        c->Interrupt = Channel;
    } else if (!c->Control || c->Interrupt) {
        // Incoming connection, PSM unknown: HID opens control first, then interrupt.
        // MmNoteInterrupt corrects this if input reports show up on the other channel.
        c->Control = Channel;
        c->Interrupt = NULL;
    } else {
        c->Interrupt = Channel;
    }
    if (c->Control && c->Interrupt) c->TouchActive = FALSE;    // new connection
    both = MmChannelsChanged(c);
    WdfSpinLockRelease(c->Lock);

    if (both) MmStartTimers(c);
}

static VOID MmChannelClosed(PMM_FILTER_CONTEXT c, L2CAP_CHANNEL_HANDLE Channel)
{
    WdfSpinLockAcquire(c->Lock);
    c->ChannelCloses++;
    if (Channel == c->Recent[0]) c->Recent[0] = NULL;
    if (Channel == c->Recent[1]) c->Recent[1] = NULL;
    if (Channel == c->Control) c->Control = NULL;
    if (Channel == c->Interrupt) {
        c->Interrupt = NULL;
        c->TouchActive = FALSE;
        c->InjectCount = 0;
    }
    WdfSpinLockRelease(c->Lock);
}

// Input reports arrived on Channel, so it is the interrupt channel, and the other recently
// used channel is control.
static VOID MmNoteInterrupt(PMM_FILTER_CONTEXT c, L2CAP_CHANNEL_HANDLE Channel, BOOLEAN Touch)
{
    BOOLEAN both = FALSE;

    WdfSpinLockAcquire(c->Lock);
    if (Touch) c->TouchActive = TRUE;
    if (Channel != c->Interrupt || !c->Control) {
        c->Interrupt = Channel;
        if (c->Control == Channel) c->Control = NULL;
        if (!c->Control) {
            c->Control = c->Recent[0] != Channel ? c->Recent[0] : c->Recent[1];
        }
        both = MmChannelsChanged(c);
    }
    WdfSpinLockRelease(c->Lock);

    if (both) MmStartTimers(c);
}

// HidBth submitted a transfer on Channel. Remembered even before the interrupt channel is
// known: HidBth posts one long-lived control read right after connecting. Once interrupt
// is known, any other channel used is control.
static VOID MmNoteTraffic(PMM_FILTER_CONTEXT c, L2CAP_CHANNEL_HANDLE Channel, BTH_ADDR Address)
{
    BOOLEAN both = FALSE;

    WdfSpinLockAcquire(c->Lock);
    if (Address) c->BtAddress = Address;
    MmRemember(c, Channel);
    if (c->Interrupt && Channel != c->Interrupt && Channel != c->Control) {
        c->Control = Channel;
        both = MmChannelsChanged(c);
    }
    WdfSpinLockRelease(c->Lock);

    if (both) MmStartTimers(c);
}

static BOOLEAN MmIsTouchReport(const UCHAR* Report, ULONG Length)
{
    return Length >= 6 && Length <= MM_MAX_REPORT && (Length - 6) % 8 == 0 && Report[0] == MM_REPORT_TOUCH;
}

// Motion and buttons from the header of a 0x29 report. Motion is 10-bit signed, split
// across the header like Linux hid-magicmouse decodes it.
static VOID MmDecodeMotion(const UCHAR* r, LONG* X, LONG* Y, UCHAR* Buttons)
{
    *X = (LONG)(((ULONG)(r[3] & 0x0C) << 28) | ((ULONG)r[1] << 22)) >> 22;
    *Y = (LONG)(((ULONG)(r[3] & 0x30) << 26) | ((ULONG)r[2] << 22)) >> 22;
    *Buttons = r[3] & 0x03;
}

// Handles one interrupt-channel packet ("A1 id ..."). Touch reports (0x29, or two packed in
// 0xF7) are queued for the app and replaced in place by stock report "A1 10 buttons X X Y Y".
// Returns the length to hand to HidBth, or 0 to drop the packet.
// Left-handed mode swaps the mouse's own buttons (injected ones are already logical).
static UCHAR MmSwapButtons(UCHAR b)
{
    if (!(g_Mm.Options & MM_OPTION_SWAP_BUTTONS)) return b;
    return (UCHAR)(((b & 1) << 1) | ((b & 2) >> 1));
}

static ULONG MmTranslateInput(PMM_FILTER_CONTEXT c, L2CAP_CHANNEL_HANDLE Channel, PUCHAR Buf, ULONG Len)
{
    const UCHAR* part[2];
    ULONG partLen[2];
    ULONG parts = 0, i;
    LONG x = 0, y = 0, dx, dy;
    UCHAR buttons = 0;
    BOOLEAN any = FALSE;

    if (Len < 2 || Buf[0] != HIDP_DATA_INPUT) return Len;
    if (Buf[1] == MM_REPORT_MOUSE) {
        MmNoteInterrupt(c, Channel, FALSE);
        if (Len >= 3) {
            Buf[2] = (Buf[2] & ~0x03) | MmSwapButtons(Buf[2] & 0x03);
            c->PhysButtons = Buf[2] & 0x03;
        }
        return Len;
    }
    if (Buf[1] == MM_REPORT_TOUCH) {
        part[0] = Buf + 1;
        partLen[0] = Len - 1;
        parts = 1;
    } else if (Buf[1] == MM_REPORT_DOUBLE && Len >= 3 && Buf[2] <= Len - 3) {
        part[0] = Buf + 3;
        partLen[0] = Buf[2];
        part[1] = Buf + 3 + Buf[2];
        partLen[1] = Len - 3 - Buf[2];
        parts = 2;
    } else {
        WdfSpinLockAcquire(c->Lock);
        c->OtherReports++;
        c->LastOtherId = Buf[1];
        WdfSpinLockRelease(c->Lock);
        MmPushFrame(Buf + 1, min(Len - 1, MM_MAX_REPORT));
        return Len;
    }

    MmNoteInterrupt(c, Channel, TRUE);
    for (i = 0; i < parts; i++) {
        if (!MmIsTouchReport(part[i], partLen[i])) continue;
        MmPushFrame(part[i], partLen[i]);
        MmDecodeMotion(part[i], &dx, &dy, &buttons);
        x += dx;
        y += dy;
        any = TRUE;
    }
    if (!any) return 0;
    buttons = MmSwapButtons(buttons);
    c->PhysButtons = buttons;

    x = max(-511, min(511, x));     // stock descriptor's logical range
    y = max(-511, min(511, y));
    Buf[1] = MM_REPORT_MOUSE;
    Buf[2] = buttons;
    Buf[3] = (UCHAR)x;
    Buf[4] = (UCHAR)(x >> 8);
    Buf[5] = (UCHAR)y;
    Buf[6] = (UCHAR)(y >> 8);
    return 7;
}

static VOID MmBounce(PMM_REQUEST_CONTEXT rc, MM_ACL_BRB* Acl)
{
    Acl->Buffer = rc->Bounce;
    Acl->BufferMDL = NULL;
    Acl->BufferSize = sizeof(rc->Bounce);
    Acl->TransferFlags |= ACL_SHORT_TRANSFER_OK;
}

// ---- injected buttons
//
// To press a button as if on the mouse itself, one of HidBth's interrupt reads that is down
// at BthPort is cancelled and completed with a stock report 0x10 carrying the button state.
// The canceller (MmKickInject) and the completion routine each hold a reference on the
// picked read; whichever releases last completes it, so it is never completed twice or
// touched after HidBth has it back.

static VOID MmTrackRead(PMM_FILTER_CONTEXT c, WDFREQUEST Request)
{
    ULONG i;

    WdfSpinLockAcquire(c->Lock);
    for (i = 0; i < MM_TRACKED_READS && c->Reads[i]; i++) {}
    if (i < MM_TRACKED_READS) c->Reads[i] = Request;
    WdfSpinLockRelease(c->Lock);
}

// Returns TRUE if the read was picked for an injection.
static BOOLEAN MmUntrackRead(PMM_FILTER_CONTEXT c, WDFREQUEST Request)
{
    ULONG i;
    BOOLEAN inject;

    WdfSpinLockAcquire(c->Lock);
    for (i = 0; i < MM_TRACKED_READS; i++) if (c->Reads[i] == Request) c->Reads[i] = NULL;
    inject = MmGetRequestContext(Request)->Inject;
    WdfSpinLockRelease(c->Lock);
    return inject;
}

// Drops one reference on a read picked for an injection; the last one completes it.
static VOID MmInjectRelease(PMM_FILTER_CONTEXT c, WDFREQUEST Request)
{
    PMM_REQUEST_CONTEXT rc = MmGetRequestContext(Request);

    if (InterlockedDecrement(&rc->InjectRefs) != 0) return;
    rc->Inject = FALSE;
    WdfSpinLockAcquire(c->Lock);
    c->InjectBusy = FALSE;
    WdfSpinLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(Request, rc->FinalStatus, rc->FinalInfo);
    MmKickInject(c);        // the next state (e.g. the release) may have another read ready
}

// If button states are queued, picks a pending interrupt read and cancels it; its
// completion fills in the next state. Called when states are queued and whenever HidBth
// posts another interrupt read.
static VOID MmKickInject(PMM_FILTER_CONTEXT c)
{
    WDFREQUEST victim = NULL;
    ULONG i;

    WdfSpinLockAcquire(c->Lock);
    if (c->InjectCount && !c->InjectBusy) {
        for (i = 0; i < MM_TRACKED_READS && !victim; i++) victim = c->Reads[i];
        if (victim) {
            PMM_REQUEST_CONTEXT rc = MmGetRequestContext(victim);
            rc->Inject = TRUE;
            rc->InjectRefs = 2;
            c->InjectBusy = TRUE;
        }
    }
    WdfSpinLockRelease(c->Lock);
    if (!victim) return;

    WdfRequestCancelSentRequest(victim);
    MmInjectRelease(c, victim);
}

// Queues button states from the app (IOCTL_MM_INJECT_BUTTONS).
static NTSTATUS MmQueueInject(PMM_FILTER_CONTEXT c, const MM_INJECT* In)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;

    WdfSpinLockAcquire(c->Lock);
    if (!c->Control || !c->Interrupt) {
        status = STATUS_DEVICE_NOT_CONNECTED;
    } else if (c->InjectCount + In->Count > MM_INJECT_MAX) {
        status = STATUS_DEVICE_BUSY;
    } else {
        for (i = 0; i < In->Count; i++) c->InjectQueue[c->InjectCount++] = In->Buttons[i] & 0x03;
    }
    WdfSpinLockRelease(c->Lock);
    if (NT_SUCCESS(status)) MmKickInject(c);
    return status;
}

// Writes the next queued button state into Buf as "A1 10 buttons 0 0 0 0".
static ULONG MmFillInjected(PMM_FILTER_CONTEXT c, PUCHAR Buf)
{
    UCHAR buttons;

    WdfSpinLockAcquire(c->Lock);
    buttons = c->PhysButtons;
    if (c->InjectCount) {
        buttons |= c->InjectQueue[0];
        c->InjectCount--;
        RtlMoveMemory(c->InjectQueue, c->InjectQueue + 1, c->InjectCount);
        c->Injected++;
    }
    WdfSpinLockRelease(c->Lock);
    Buf[0] = HIDP_DATA_INPUT;
    Buf[1] = MM_REPORT_MOUSE;
    Buf[2] = buttons;
    Buf[3] = Buf[4] = Buf[5] = Buf[6] = 0;
    return 7;
}

// ---- HidBth's reads

// Sends HidBth's read down again as HidBth submitted it, bounced.
static BOOLEAN MmResubmit(PMM_FILTER_CONTEXT c, WDFREQUEST Request)
{
    PMM_REQUEST_CONTEXT rc = MmGetRequestContext(Request);
    MM_ACL_BRB* acl = &rc->Brb->BrbL2caAclTransfer;

    *acl = rc->Saved;
    MmBounce(rc, acl);
    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, MmAclReadComplete, c);
    if (rc->InterruptRead) MmTrackRead(c, Request);
    if (WdfRequestSend(Request, c->Target, WDF_NO_SEND_OPTIONS)) return TRUE;
    if (rc->InterruptRead) MmUntrackRead(c, Request);
    *acl = rc->Saved;
    return FALSE;
}

// Completes one of HidBth's reads, keeping the control-read count right; a read picked for
// an injection completes once both its references are gone.
static VOID MmCompleteHidbthRead(PMM_FILTER_CONTEXT c, WDFREQUEST Request, NTSTATUS Status, ULONG_PTR Information)
{
    PMM_REQUEST_CONTEXT rc = MmGetRequestContext(Request);

    if (rc->Inject) {
        rc->FinalStatus = Status;
        rc->FinalInfo = Information;
        MmInjectRelease(c, Request);
        return;
    }
    if (rc->ControlRead) {
        WdfSpinLockAcquire(c->Lock);
        c->HidbthControlReads--;
        WdfSpinLockRelease(c->Lock);
    }
    WdfRequestCompleteWithInformation(Request, Status, Information);
}

// Reads HidBth has no use for (our own replies, dropped packets) go straight back down.
static VOID MmReadAgain(PMM_FILTER_CONTEXT c, WDFREQUEST Request)
{
    if (!MmResubmit(c, Request)) MmCompleteHidbthRead(c, Request, WdfRequestGetStatus(Request), 0);
}

// Every HidBth read is bounced (MmBounce), so whole packets arrive here in rc->Bounce:
//   - interrupt: touch reports are queued for the app and rewritten as report 0x10; a read
//     picked for an injection carries the injected button state instead (unless real input
//     arrived first, which goes through and leaves the injection queued);
//   - control: the reply to our own request is taken and swallowed; anything else goes to
//     HidBth, and what doesn't fit its buffer is kept for its next control read, as BthPort
//     would have split it.
VOID MmAclReadComplete(WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    PMM_FILTER_CONTEXT c = (PMM_FILTER_CONTEXT)Context;
    PMM_REQUEST_CONTEXT rc = MmGetRequestContext(Request);
    MM_ACL_BRB* acl = &rc->Brb->BrbL2caAclTransfer;
    NTSTATUS status = Params->IoStatus.Status;
    ULONG len = NT_SUCCESS(status) ? acl->BufferSize : 0;
    ULONG room = rc->Saved.BufferSize;
    PUCHAR buf;
    BOOLEAN isControl, swallow, inject = FALSE;
    UCHAR flags;

    UNREFERENCED_PARAMETER(Target);

    acl->Buffer = rc->Saved.Buffer;
    acl->BufferMDL = rc->Saved.BufferMDL;
    acl->TransferFlags = rc->Saved.TransferFlags;
    if (rc->InterruptRead) inject = MmUntrackRead(c, Request);

    // A read posted before the channels were identified may turn out to be on control.
    WdfSpinLockAcquire(c->Lock);
    isControl = rc->ControlRead || (c->Control && rc->Saved.ChannelHandle == c->Control);
    WdfSpinLockRelease(c->Lock);
    flags = MM_EV_FLAG_BOUNCED | (isControl ? MM_EV_FLAG_CONTROL : 0);

    if (!NT_SUCCESS(status)) {
        if (inject && status == STATUS_CANCELLED) goto deliverInjected;
        WdfSpinLockAcquire(c->Lock);
        c->ReadErrors++;
        c->LastReadError = status;
        if (inject) c->InjectCount = 0;     // the channel is going away: don't press later
        WdfSpinLockRelease(c->Lock);
        if (isControl) MmLog(c, MM_EV_READ_FAILED, flags, room, status, NULL);
        MmCompleteHidbthRead(c, Request, status, Params->IoStatus.Information);
        return;
    }

    if (isControl) {
        WdfSpinLockAcquire(c->Lock);
        swallow = len && MmIsOurReply(c, rc->Bounce, len);
        WdfSpinLockRelease(c->Lock);
        MmLog(c, swallow ? MM_EV_REPLY_OURS : MM_EV_REPLY_TO_HIDBTH, flags, len, status, rc->Bounce);
        if (swallow) {
            MmReadAgain(c, Request);
            return;
        }
        if (len > room) {
            WdfSpinLockAcquire(c->Lock);
            c->ControlTailLen = len - room;
            RtlCopyMemory(c->ControlTail, rc->Bounce + room, c->ControlTailLen);
            WdfSpinLockRelease(c->Lock);
            len = room;
        }
    } else {
        ULONG raw = len;
        len = MmTranslateInput(c, rc->Saved.ChannelHandle, rc->Bounce, len);
        if (len == 0 || len > room) {
            if (inject) goto deliverInjected;
            MmLog(c, MM_EV_DROPPED, flags, raw, (NTSTATUS)room, rc->Bounce);
            MmReadAgain(c, Request);
            return;
        }
    }

    buf = MmAclBuffer(acl);
    if (!buf) {
        if (inject) {
            MmCompleteHidbthRead(c, Request, STATUS_INSUFFICIENT_RESOURCES, 0);
            return;
        }
        MmReadAgain(c, Request);
        return;
    }
    RtlCopyMemory(buf, rc->Bounce, len);
    acl->BufferSize = len;
    acl->RemainingBufferSize = room - len;
    MmCompleteHidbthRead(c, Request, status, Params->IoStatus.Information);
    return;

deliverInjected:
    buf = MmAclBuffer(acl);
    if (!buf || room < 7) {
        MmCompleteHidbthRead(c, Request, STATUS_CANCELLED, 0);
        return;
    }
    len = MmFillInjected(c, buf);
    acl->BufferSize = len;
    acl->RemainingBufferSize = room - len;
    MmCompleteHidbthRead(c, Request, STATUS_SUCCESS, 0);
}

// A control read arriving while part of an earlier control packet is still held back gets
// that part straight away. Returns TRUE if it completed the read.
static BOOLEAN MmServeControlTail(PMM_FILTER_CONTEXT c, WDFREQUEST Request, MM_ACL_BRB* Acl)
{
    PUCHAR buf = MmAclBuffer(Acl);
    ULONG n = 0;

    if (!buf) return FALSE;
    WdfSpinLockAcquire(c->Lock);
    if (c->ControlTailLen) {
        n = min(c->ControlTailLen, Acl->BufferSize);
        RtlCopyMemory(buf, c->ControlTail, n);
        c->ControlTailLen -= n;
        RtlMoveMemory(c->ControlTail, c->ControlTail + n, c->ControlTailLen);
    }
    WdfSpinLockRelease(c->Lock);
    if (!n) return FALSE;

    MmLog(c, MM_EV_REPLY_TO_HIDBTH, MM_EV_FLAG_CONTROL, n, STATUS_SUCCESS, buf);
    Acl->RemainingBufferSize = Acl->BufferSize - n;
    Acl->BufferSize = n;
    WdfRequestComplete(Request, STATUS_SUCCESS);
    return TRUE;
}

VOID MmOpenComplete(WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    PMM_FILTER_CONTEXT c = (PMM_FILTER_CONTEXT)Context;
    PBRB brb = MmGetRequestContext(Request)->Brb;
    struct _BRB_L2CA_OPEN_CHANNEL* open = &brb->BrbL2caOpenChannel;

    UNREFERENCED_PARAMETER(Target);

    WdfSpinLockAcquire(c->Lock);
    c->LastOpenType = brb->BrbHeader.Type;
    c->LastOpenIoStatus = Params->IoStatus.Status;
    c->LastOpenBrbStatus = brb->BrbHeader.Status;
    c->LastOpenResponse = open->Response;
    c->LastOpenHadHandle = open->ChannelHandle != NULL;
    WdfSpinLockRelease(c->Lock);

    if (NT_SUCCESS(Params->IoStatus.Status) && open->ChannelHandle) {
        if (brb->BrbHeader.Type == BRB_L2CA_OPEN_CHANNEL) {
            MmChannelOpened(c, open->ChannelHandle, open->Psm, open->BtAddress);
        } else if (open->Response == PSM_HID_CONTROL || open->Response == PSM_HID_INTERRUPT) {
            // Seen on Windows 10: a completed response carries the PSM in the Response/Psm union.
            MmChannelOpened(c, open->ChannelHandle, open->Response, open->BtAddress);
        } else if (open->Response == CONNECT_RSP_RESULT_SUCCESS) {
            MmChannelOpened(c, open->ChannelHandle, 0, open->BtAddress);
        }
    }
    WdfRequestCompleteWithInformation(Request, Params->IoStatus.Status, Params->IoStatus.Information);
}

// ---------------------------------------------------------------------------------------
// Filter device

static VOID MmForward(PMM_FILTER_CONTEXT c, WDFREQUEST Request, PFN_WDF_REQUEST_COMPLETION_ROUTINE Done)
{
    WDF_REQUEST_SEND_OPTIONS options;
    BOOLEAN sent;

    if (Done) {
        WdfRequestFormatRequestUsingCurrentType(Request);
        WdfRequestSetCompletionRoutine(Request, Done, c);
        sent = WdfRequestSend(Request, c->Target, WDF_NO_SEND_OPTIONS);
    } else {
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
        sent = WdfRequestSend(Request, c->Target, &options);
    }
    if (!sent) WdfRequestComplete(Request, WdfRequestGetStatus(Request));
}

VOID MmEvtIoInternalDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength,
                                  size_t InputBufferLength, ULONG IoControlCode)
{
    PMM_FILTER_CONTEXT c = MmGetFilterContext(WdfIoQueueGetDevice(Queue));
    PMM_REQUEST_CONTEXT rc = MmGetRequestContext(Request);
    PBRB brb;
    BOOLEAN isControl, logRead;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != IOCTL_INTERNAL_BTH_SUBMIT_BRB) {
        MmRecordSeen(IoControlCode);
        MmForward(c, Request, NULL);
        return;
    }

    brb = (PBRB)IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request))->Parameters.Others.Argument1;
    if (!brb) {
        MmForward(c, Request, NULL);
        return;
    }
    MmRecordSeen(MM_SEEN_BRB | brb->BrbHeader.Type);
    rc->Brb = brb;
    rc->ControlRead = FALSE;

    switch (brb->BrbHeader.Type) {
    case BRB_L2CA_ACL_TRANSFER:
        MmNoteTraffic(c, brb->BrbL2caAclTransfer.ChannelHandle, brb->BrbL2caAclTransfer.BtAddress);
        if (!(brb->BrbL2caAclTransfer.TransferFlags & ACL_TRANSFER_DIRECTION_IN)) {
            MmNoteHidbthRequest(c, &brb->BrbL2caAclTransfer);
            break;
        }
        rc->Saved = brb->BrbL2caAclTransfer;
        rc->Inject = FALSE;
        WdfSpinLockAcquire(c->Lock);
        isControl = c->Control && brb->BrbL2caAclTransfer.ChannelHandle == c->Control;
        logRead = isControl || !c->Interrupt || brb->BrbL2caAclTransfer.ChannelHandle != c->Interrupt;
        rc->InterruptRead = !logRead;
        WdfSpinLockRelease(c->Lock);
        if (logRead) {
            MmLog(c, MM_EV_HIDBTH_READ, isControl ? MM_EV_FLAG_CONTROL : 0, rc->Saved.BufferSize, 0, NULL);
        }
        if (isControl) {
            if (MmServeControlTail(c, Request, &brb->BrbL2caAclTransfer)) return;
            rc->ControlRead = TRUE;
            WdfSpinLockAcquire(c->Lock);
            c->HidbthControlReads++;
            WdfSpinLockRelease(c->Lock);
        }
        MmBounce(rc, &brb->BrbL2caAclTransfer);
        if (!rc->InterruptRead) {
            MmForward(c, Request, MmAclReadComplete);
            return;
        }
        // Interrupt reads are tracked while down, for injected buttons.
        WdfRequestFormatRequestUsingCurrentType(Request);
        WdfRequestSetCompletionRoutine(Request, MmAclReadComplete, c);
        MmTrackRead(c, Request);
        if (!WdfRequestSend(Request, c->Target, WDF_NO_SEND_OPTIONS)) {
            MmUntrackRead(c, Request);
            WdfRequestComplete(Request, WdfRequestGetStatus(Request));
            return;
        }
        MmKickInject(c);
        return;

    case BRB_L2CA_OPEN_CHANNEL:
    case BRB_L2CA_OPEN_CHANNEL_RESPONSE:
        MmForward(c, Request, MmOpenComplete);
        return;

    case BRB_L2CA_CLOSE_CHANNEL:
        MmChannelClosed(c, brb->BrbL2caCloseChannel.ChannelHandle);
        break;
    }
    MmForward(c, Request, NULL);
}

NTSTATUS MmEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Resources, WDFCMRESLIST ResourcesTranslated)
{
    PMM_FILTER_CONTEXT c = MmGetFilterContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Resources);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (c->HaveBth) return STATUS_SUCCESS;
    status = WdfFdoQueryForInterface(Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE, (PINTERFACE)&c->Bth,
                                     sizeof(c->Bth), BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
    if (!NT_SUCCESS(status)) {
        // Without it we can't send D7 01; the mouse still works as a plain mouse.
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "mmfilter: profile interface QI failed 0x%08X\n", status);
        return STATUS_SUCCESS;
    }
    c->Bth.BthInitializeBrb((PBRB)&c->SendBrb, BRB_L2CA_ACL_TRANSFER);
    c->HaveBth = TRUE;
    return STATUS_SUCCESS;
}

// L2CAP channels belong to BthPort and survive power transitions of this stack, so channel
// state is kept; only the timers are paused.
NTSTATUS MmEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PMM_FILTER_CONTEXT c = MmGetFilterContext(Device);
    BOOLEAN connected;

    UNREFERENCED_PARAMETER(PreviousState);

    WdfSpinLockAcquire(c->Lock);
    connected = c->Control && c->Interrupt;
    WdfSpinLockRelease(c->Lock);
    if (connected) MmStartTimers(c);    // each timer checks whether it still has work
    return STATUS_SUCCESS;
}

NTSTATUS MmEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PMM_FILTER_CONTEXT c = MmGetFilterContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WdfSpinLockAcquire(c->Lock);
    c->D0Exits++;
    WdfSpinLockRelease(c->Lock);
    WdfTimerStop(c->EnableTimer, TRUE);
    WdfTimerStop(c->BatteryTimer, TRUE);
    return STATUS_SUCCESS;
}

// Creates the side channel if no mouse has yet. The side channel is optional: if it can't
// be created the mouse still works. Creation fails while the previous control device still
// exists, e.g. when the tray app held it open across a driver restart; retry until the
// name is free, as long as a mouse is attached (or the retry would keep the driver loaded).
static VOID MmEnsureControlDevice(WDFDRIVER Driver, PMM_FILTER_CONTEXT c)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG attached;

    WdfWaitLockAcquire(g_Mm.CdoLock, NULL);
    WdfSpinLockAcquire(g_Mm.Lock);
    attached = g_Mm.Attached;
    WdfSpinLockRelease(g_Mm.Lock);
    if (attached && !g_Mm.Cdo) status = MmCreateControlDevice(Driver);
    WdfWaitLockRelease(g_Mm.CdoLock);

    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "mmfilter: control device failed 0x%08X, retrying\n", status);
        if (c->CdoTimer) WdfTimerStart(c->CdoTimer, WDF_REL_TIMEOUT_IN_MS(MM_CDO_RETRY_MS));
    }
}

VOID MmCdoRetryTimer(WDFTIMER Timer)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    MmEnsureControlDevice(WdfDeviceGetDriver(device), MmGetFilterContext(device));
}

VOID MmEvtFilterCleanup(WDFOBJECT Object)
{
    PMM_FILTER_CONTEXT c = MmGetFilterContext((WDFDEVICE)Object);
    ULONG attached;

    WdfSpinLockAcquire(g_Mm.Lock);
    if (g_Mm.Filter == c) g_Mm.Filter = NULL;
    attached = --g_Mm.Attached;
    WdfSpinLockRelease(g_Mm.Lock);

    if (attached == 0) {
        WdfWaitLockAcquire(g_Mm.CdoLock, NULL);
        if (g_Mm.Cdo) {
            // Fail the app's pending reads first, so it closes its handle: a control device
            // that's still open lingers (and keeps the driver loaded) after being deleted.
            WDFQUEUE readQueue = g_Mm.ReadQueue;
            g_Mm.ReadQueue = NULL;
            if (readQueue) WdfIoQueuePurgeSynchronously(readQueue);
            WdfObjectDelete(g_Mm.Cdo);
            g_Mm.Cdo = NULL;
        }
        WdfWaitLockRelease(g_Mm.CdoLock);
    }

    if (c->HaveBth) c->Bth.Interface.InterfaceDereference(c->Bth.Interface.Context);
}

NTSTATUS MmEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_IO_QUEUE_CONFIG qc;
    WDF_TIMER_CONFIG tc;
    WDFDEVICE device;
    PMM_FILTER_CONTEXT c;
    NTSTATUS status;

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = MmEvtPrepareHardware;
    pnp.EvtDeviceD0Entry = MmEvtD0Entry;
    pnp.EvtDeviceD0Exit = MmEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, MM_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &attrs);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, MM_FILTER_CONTEXT);
    attrs.EvtCleanupCallback = MmEvtFilterCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) return status;

    // From here on cleanup runs on failure too, so count this device first.
    WdfSpinLockAcquire(g_Mm.Lock);
    g_Mm.Attached++;
    WdfSpinLockRelease(g_Mm.Lock);

    c = MmGetFilterContext(device);
    c->Device = device;
    c->Target = WdfDeviceGetIoTarget(device);
    c->Battery = -1;

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    status = WdfSpinLockCreate(&attrs, &c->Lock);
    if (NT_SUCCESS(status)) status = WdfSpinLockCreate(&attrs, &c->LogLock);
    if (!NT_SUCCESS(status)) return status;

    WDF_TIMER_CONFIG_INIT(&tc, MmEnableTimer);
    tc.AutomaticSerialization = FALSE;
    status = WdfTimerCreate(&tc, &attrs, &c->EnableTimer);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG_INIT(&tc, MmBatteryTimer);
    tc.AutomaticSerialization = FALSE;
    status = WdfTimerCreate(&tc, &attrs, &c->BatteryTimer);
    if (!NT_SUCCESS(status)) return status;

    status = WdfRequestCreate(&attrs, c->Target, &c->SendRequest);
    if (NT_SUCCESS(status)) status = WdfMemoryCreatePreallocated(&attrs, &c->SendBrb, sizeof(c->SendBrb), &c->SendMemory);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
    qc.EvtIoInternalDeviceControl = MmEvtIoInternalDeviceControl;
    qc.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &qc, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) return status;

    WDF_TIMER_CONFIG_INIT(&tc, MmCdoRetryTimer);
    tc.AutomaticSerialization = FALSE;
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    if (!NT_SUCCESS(WdfTimerCreate(&tc, &attrs, &c->CdoTimer))) c->CdoTimer = NULL;     // retry is optional

    WdfSpinLockAcquire(g_Mm.Lock);
    g_Mm.Filter = c;
    WdfSpinLockRelease(g_Mm.Lock);

    MmEnsureControlDevice(Driver, c);
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, MmEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) return status;

    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &g_Mm.Lock);
    if (NT_SUCCESS(status)) status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &g_Mm.CdoLock);
    return status;
}
