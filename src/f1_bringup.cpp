// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// f1_bringup.cpp - F1/F2 milestone: a real NVMe over Fabrics admin-queue
// exchange between our own initiator and our own target, over NetworkDirect.
//
// WHAT THIS PROVES
//   Property Set (CC.EN)  -> target enables its controller
//   Property Get (CSTS)   -> target reports RDY
//   Connect (admin QP)    -> controller id assigned, host/subsystem NQNs logged
//   Identify Controller   -> 4096 bytes transferred by the TARGET, into the
//                            initiator's memory, via RDMA Write using the rkey
//                            that arrived in the command's SGL
//
// That last step is the point of the whole exercise: it is the NVMe-oF data
// path - a capsule carrying an SGL, and the peer moving the payload with a
// one-sided operation against an rkey the host published.  Everything in this
// project that follows (I/O queues, Read/Write commands, the real target) is the
// same shape with different opcodes.
//
// Everything here follows constraints established by measurement in
// stag_smoketest.cpp - see DESIGN.md section 8.  The ones that actually bite:
//   * NdStartup() first, or NdOpenAdapter fails ND_DEVICE_NOT_READY
//   * outboundReadLimit must be non-zero or this side cannot issue RDMA Read
//   * GetPrivateData only works between GetConnectionRequest and Accept
//     (target) / Connect and CompleteConnect (initiator)
//   * every SGE buffer must lie inside the region named by its token, or the
//     provider returns a raw 0xC000003E that appears in no header
//   * a two-sided Send needs its Receive posted first, or the send times out and
//     the *peer* is the side that reports the failure
//   * ND_TIMEOUT/ND_PENDING are success-severity, so SUCCEEDED() cannot be used
//     to decide whether a wait succeeded
//
// USAGE
//   f1_bringup.exe -target    192.168.100.2 54340
//   f1_bringup.exe -initiator 192.168.100.2 54340 192.168.100.3
// Start the target first.  Exit code = number of failures.

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <initguid.h>
#include <ndspi.h>
#include <ndsupport.h>
#include <nddef.h>
#include <ndstatus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "nvmeof_wire.h"

#pragma comment(lib, "Ws2_32.lib")

static int g_failures = 0;

static bool ndOk(HRESULT hr) {
    return SUCCEEDED(hr) && hr != ND_TIMEOUT && hr != ND_PENDING;
}
static void Report(const char* what, int ok, const char* detail) {
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           (detail && *detail) ? " - " : "", (detail && *detail) ? detail : "");
    if (!ok) g_failures++;
}
static const char* ndStr(HRESULT hr, char* buf, size_t n) {
    sprintf_s(buf, n, "0x%08X%s", (unsigned)hr,
              hr == ND_TIMEOUT      ? " (ND_TIMEOUT)" :
              hr == ND_PENDING      ? " (ND_PENDING)" :
              hr == ND_CANCELED     ? " (ND_CANCELED)" :
              hr == ND_IO_TIMEOUT   ? " (ND_IO_TIMEOUT)" :
              hr == ND_ACCESS_VIOLATION ? " (ND_ACCESS_VIOLATION)" : "");
    return buf;
}

// ===========================================================================
//  Queue pair + memory: the transport under test
// ===========================================================================
static const ULONG  kCqDepth = 128;
static const ULONG  kQpDepth = 128;
static const ULONG  kSqSize = 32;         // entries in the queue this side uses
static const ULONG  kReadLimit = 16;      // MUST be non-zero to issue Reads
static const DWORD  kWaitMs = 5000;
static const SIZE_T kRegionSize = 2u * 1024 * 1024;

// Layout inside the single registered region.  Everything an SGE can name lives
// here; that is not stylistic, it is the provider's requirement.
static const SIZE_T kCapsuleOff   = 0;                 // 64 B outbound capsule
static const SIZE_T kResponseOff  = 256;               // 16 B inbound response
static const SIZE_T kConnectOff   = 512;               // 1024 B Connect data
static const SIZE_T kIdentifyOff  = 4096;              // 4096 B Identify payload
static const SIZE_T kScratchOff   = 8192;              // scratch for bookkeeping
// Outbound completions must live in the region too.  A `uint8_t cqe[16]` on the
// stack with the MR's token is rejected with ND_ACCESS_VIOLATION on the SEND -
// which is the third time this exact mistake has appeared in this project, in
// three different disguises.
static const SIZE_T kCqeOutOff    = 16384;

#define CTX_CAPSULE ((VOID*)(uintptr_t)0xC1)
#define CTX_RESP    ((VOID*)(uintptr_t)0xC2)
#define CTX_MISC    ((VOID*)(uintptr_t)0xC3)

struct Transport {
    IND2Adapter*         adapter = nullptr;
    IND2CompletionQueue* cq = nullptr;
    IND2Connector*       conn = nullptr;
    IND2QueuePair*       qp = nullptr;
    IND2MemoryRegion*    mr = nullptr;
    IND2Listener*        listener = nullptr;
    HANDLE               ovFile = INVALID_HANDLE_VALUE;
    OVERLAPPED           ov = {};
    uint8_t*             reg = nullptr;      // the registered region
    uint32_t             rkey = 0;           // remote token for the whole region

    HRESULT waitOverlapped(IND2Overlapped* obj, DWORD ms) {
        ULONGLONG t0 = GetTickCount64();
        for (;;) {
            HRESULT hr = obj->GetOverlappedResult(&ov, FALSE);
            if (hr != ND_PENDING) return hr;
            if (GetTickCount64() - t0 >= ms) {
                obj->CancelOverlappedRequests();
                obj->GetOverlappedResult(&ov, FALSE);
                return ND_TIMEOUT;
            }
            SwitchToThread();
        }
    }
    // Reap one completion, optionally requiring a specific context.  A wildcard
    // reap is deliberately available but every caller that uses it is a place a
    // stray CQE can be swallowed - used only where that is understood.
    HRESULT reap(VOID* expect, ND2_RESULT* out, DWORD ms) {
        ULONGLONG t0 = GetTickCount64();
        for (;;) {
            ND2_RESULT r = {};
            if (cq->GetResults(&r, 1) == 1) {
                if (expect && r.RequestContext != expect) {
                    printf("    (ignoring completion ctx=%p type=%d st=0x%08X)\n",
                           r.RequestContext, (int)r.RequestType, (unsigned)r.Status);
                    continue;
                }
                if (out) *out = r;
                return r.Status;
            }
            if (GetTickCount64() - t0 >= ms) return ND_TIMEOUT;
            SwitchToThread();
        }
    }

    bool open(const sockaddr_in& local, bool asListener) {
        HRESULT hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&local,
                                   sizeof(local), (VOID**)&adapter);
        if (FAILED(hr)) { printf("NdOpenAdapter 0x%08X\n", (unsigned)hr); return false; }
        if (FAILED(hr = adapter->CreateOverlappedFile(&ovFile))) return false;
        ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!ov.hEvent) return false;
        if (FAILED(hr = adapter->CreateCompletionQueue(IID_IND2CompletionQueue, ovFile,
                                                       kCqDepth, 0, 0, (VOID**)&cq))) return false;
        if (FAILED(hr = adapter->CreateConnector(IID_IND2Connector, ovFile, (VOID**)&conn))) return false;
        if (FAILED(hr = adapter->CreateQueuePair(IID_IND2QueuePair, cq, cq, nullptr,
                                                 kQpDepth, kQpDepth, 1, 1, 0, (VOID**)&qp))) return false;
        if (FAILED(hr = adapter->CreateMemoryRegion(IID_IND2MemoryRegion, ovFile, (VOID**)&mr))) return false;
        if (asListener && FAILED(hr = adapter->CreateListener(IID_IND2Listener, ovFile, (VOID**)&listener)))
            return false;

        reg = (uint8_t*)VirtualAlloc(nullptr, kRegionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!reg) return false;
        memset(reg, 0, kRegionSize);
        hr = mr->Register(reg, kRegionSize,
                          ND_MR_FLAG_ALLOW_LOCAL_WRITE |
                          ND_MR_FLAG_ALLOW_REMOTE_READ |
                          ND_MR_FLAG_ALLOW_REMOTE_WRITE |
                          ND_MR_FLAG_RDMA_READ_SINK, &ov);
        if (hr == ND_PENDING) hr = waitOverlapped(mr, kWaitMs);
        if (!ndOk(hr)) { printf("Register 0x%08X\n", (unsigned)hr); return false; }
        rkey = mr->GetRemoteToken();
        return true;
    }

    // Validate the peer's RDMA-CM private data and Accept, the way a real fabrics
    // target does (nvmet rejects a Connect that carries none, and one whose
    // hsqsize + 1 exceeds the admin queue depth).  This file's Transport type is
    // older than nvmeof::Queue, so the check is spelled out here rather than
    // shared - but it is the same check, with the same status codes.
    int acceptChecked(ULONG rqDepth, uint16_t* claimedQid, uint16_t* rejStatus) {
        uint8_t buf[512];
        memset(buf, 0, sizeof(buf));
        ULONG len = sizeof(buf);
        HRESULT h = conn->GetPrivateData(buf, &len);
        nvmeof_rdma_request_pd req;
        memcpy(&req, buf, sizeof(req));
        uint16_t peerQid = 0;
        uint16_t sts = FAILED(h) ? (uint16_t)NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH
                                 : nvmeof_rdma_check_req(&req, len, &peerQid);
        if (claimedQid) *claimedQid = peerQid;
        printf("  [conn] private data: hr=0x%08X len=%u recfmt=%u qid=%u hrqsize=%u hsqsize=%u\n",
               (unsigned)h, len, req.recfmt, req.qid, req.hrqsize, req.hsqsize);
        if (FAILED(h) || sts != 0) {
            nvmeof_rdma_reject_pd rej;
            rej.recfmt = 0;
            rej.sts = sts ? sts : (uint16_t)NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH;
            printf("  [conn] rejecting with nvme_rdma status %u\n", rej.sts);
            conn->Reject(&rej, sizeof(rej));
            if (rejStatus) *rejStatus = rej.sts;
            return 1;
        }
        nvmeof_rdma_accept_pd acc;
        acc.recfmt = 0;
        acc.crqsize = (uint16_t)rqDepth;
        h = conn->Accept(qp, kReadLimit, kReadLimit, &acc, sizeof(acc), &ov);
        if (h == ND_PENDING) h = waitOverlapped(conn, kWaitMs);
        if (!ndOk(h)) return -1;
        printf("  [conn] accepted; advertised crqsize=%u\n", acc.crqsize);
        return 0;
    }

    void close() {
        if (conn) { conn->Disconnect(&ov); waitOverlapped(conn, 1500); }
        if (mr) { mr->Deregister(&ov); waitOverlapped(mr, 1500); }
        if (listener) listener->Release();
        if (qp) qp->Release();
        if (cq) cq->Release();
        if (conn) conn->Release();
        if (ovFile != INVALID_HANDLE_VALUE) CloseHandle(ovFile);
        if (adapter) adapter->Release();
        if (ov.hEvent) CloseHandle(ov.hEvent);
        if (reg) VirtualFree(reg, 0, MEM_RELEASE);
        *this = Transport{};
    }

    // ---- two-sided ----
    bool postReceive(void* buf, ULONG len, VOID* ctx) {
        ND2_SGE s = {};
        s.Buffer = buf; s.BufferLength = len; s.MemoryRegionToken = mr->GetLocalToken();
        return ndOk(qp->Receive(ctx, &s, 1));
    }
    bool sendBuf(const void* buf, ULONG len, VOID* ctx) {
        ND2_SGE s = {};
        s.Buffer = (void*)buf; s.BufferLength = len; s.MemoryRegionToken = mr->GetLocalToken();
        return ndOk(qp->Send(ctx, &s, 1, 0));
    }
    // ---- one-sided ----
    bool readInto(void* local, ULONG len, uint64_t remoteAddr, uint32_t remoteKey, VOID* ctx) {
        ND2_SGE s = {};
        s.Buffer = local; s.BufferLength = len; s.MemoryRegionToken = mr->GetLocalToken();
        return ndOk(qp->Read(ctx, &s, 1, remoteAddr, remoteKey, 0));
    }
    bool writeFrom(const void* local, ULONG len, uint64_t remoteAddr, uint32_t remoteKey, VOID* ctx) {
        ND2_SGE s = {};
        s.Buffer = (void*)local; s.BufferLength = len; s.MemoryRegionToken = mr->GetLocalToken();
        return ndOk(qp->Write(ctx, &s, 1, remoteAddr, remoteKey, 0));
    }
};

// ===========================================================================
//  Controller state (shared by both roles; the target is the one that owns it)
// ===========================================================================
struct Controller {
    bool     enabled = false;
    uint16_t cntlid = 0;
    uint32_t kato = 0;
    char     hostnqn[NVMEOF_NQN_FIELD_LEN] = {};
    char     subsysnqn[NVMEOF_NQN_FIELD_LEN] = {};
    uint8_t  hostid[16] = {};
    uint32_t identifyDataRequests = 0;

    static const char* kNqn() { return "nqn.2024-01.local.rdma:windows-nd"; }
};

// Fill an Identify Controller payload with plausible, *self-describing* values.
// The initiator checks these came back unchanged, so a truncated or
// partially-transferred payload cannot pass as success.
static const char kSerial[] = "NDVMEOF0000000000001";
static const char kModel[]  = "NetworkDirect NVMe-oF prototype          ";
static const char kFwRev[]  = "0.1.0   ";

static void buildIdentifyController(uint8_t* id, const Controller& c) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_VID, 0x15B3);      // Mellanox, as a joke
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_SSVID, 0x103C);    // HP
    memcpy(id + NVMEOF_ID_CTRL_OFF_SN, kSerial, sizeof(kSerial) - 1);
    memcpy(id + NVMEOF_ID_CTRL_OFF_MN, kModel, sizeof(kModel) - 1);
    memcpy(id + NVMEOF_ID_CTRL_OFF_FR, kFwRev, sizeof(kFwRev) - 1);
    id[NVMEOF_ID_CTRL_OFF_MDTS] = 0;                       // no transfer limit
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_CNTLID, c.cntlid);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_VER, 0x00010400);  // 1.4
    id[NVMEOF_ID_CTRL_OFF_CNTRLTYPE] = NVMEOF_CTRLTYPE_IO;
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_OACS, 0);
    id[NVMEOF_ID_CTRL_OFF_SQES] = 6;                       // 2^6 = 64
    id[NVMEOF_ID_CTRL_OFF_CQES] = 4;                       // 2^4 = 16
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_NN, 1);            // one namespace
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_ONCS, NVMEOF_CTRL_ONCS_WRITE_ZEROES);
    id[NVMEOF_ID_CTRL_OFF_VWC] = 1;
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_AWUN, 0);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_AWUPF, 0);
    // sgls: we do support keyed data-block SGLs, and that is what the initiator
    // must check before it ever sends one.
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_SGLS,
                NVMEOF_CTRL_SGLS_ADVERTISED);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SUBNQN, c.subsysnqn, NVMEOF_NQN_FIELD_LEN);
    // In-capsule capacities.  ioccsz is "the largest command capsule this
    // controller accepts", in 16-byte units, and this target accepts exactly one
    // 64-byte SQE and no in-capsule data - so it is 4, which is also what Linux's
    // nvmet reports when the port has no inline data configured.  Advertising 8
    // would say "send me 128-byte capsules", which is a promise the receive
    // buffer (64 bytes) does not keep.  icdoff is the offset at which in-capsule
    // data would begin; with no in-capsule support nvmet leaves it 0, and so do
    // we.  (This file previously claimed 8 and 2, both of which were invented.)
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ, 4);        // 4 * 16 = 64 bytes
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IORCSZ, 1);        // 1 * 16 = 16 bytes
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_ICDOFF, 0);
    id[NVMEOF_ID_CTRL_OFF_MSDBD] = 1;                      // one data block descriptor
}

// ===========================================================================
//  Admin queue: capsule in, completion out
// ===========================================================================
struct Completion {
    HRESULT  status;
    uint16_t cid;
    uint64_t result;
};

// Both roles need this: send a capsule and collect the matching completion.
// Matching is by command id, NOT by arrival order - the peer is free to complete
// out of order and a queue that assumes otherwise is a queue that will silently
// mis-attribute one command's result to another.
static bool adminCommand(Transport& t, const uint8_t* capsule64, Completion* out,
                         DWORD timeoutMs) {
    memcpy(t.reg + kCapsuleOff, capsule64, 64);
    memcpy(t.reg + kResponseOff, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                                 "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 16);  // poison
    if (!t.postReceive(t.reg + kResponseOff, 16, CTX_RESP)) {
        printf("    postReceive for the response failed\n");
        return false;
    }
    if (!t.sendBuf(t.reg + kCapsuleOff, 64, CTX_CAPSULE)) {
        printf("    send of the command capsule failed\n");
        return false;
    }
    ND2_RESULT sres = {};
    HRESULT st = t.reap(CTX_CAPSULE, &sres, kWaitMs);
    if (!ndOk(st)) { char b[64]; printf("    capsule send completion %s\n", ndStr(st, b, sizeof(b))); return false; }

    ND2_RESULT rres = {};
    st = t.reap(CTX_RESP, &rres, timeoutMs);
    if (!ndOk(st)) { char b[64]; printf("    response completion %s\n", ndStr(st, b, sizeof(b))); return false; }
    if (rres.BytesTransferred != 16) {
        printf("    response length %u (expected 16)\n", rres.BytesTransferred);
        return false;
    }
    const uint8_t* cqe = t.reg + kResponseOff;
    out->status = (HRESULT)0;
    out->cid = nvmeof_rd16(cqe + 12);
    out->result = nvmeof_rd64(cqe + 0);
    // The phase bit is part of the wire format; a completion whose phase does
    // not match what we expect is a completion from a previous lap.
    if ((nvmeof_rd16(cqe + 14) & NVMEOF_STATUS_P_MASK) == 0) {
        printf("    response phase bit clear - stale completion?\n");
    }
    return true;
}

static uint16_t cqeStatus(const Transport& t) {
    return nvmeof_rd16(t.reg + kResponseOff + 14);
}

// Build a 16-byte completion into buf.
static void buildCqe(uint8_t* buf, uint16_t cid, uint16_t sqHead, uint16_t sqId,
                     uint64_t result, uint8_t sct, uint8_t sc) {
    memset(buf, 0, 16);
    nvmeof_wr64(buf + 0, result);
    nvmeof_wr16(buf + 8, sqHead);
    nvmeof_wr16(buf + 10, sqId);
    nvmeof_wr16(buf + 12, cid);
    nvmeof_wr16(buf + 14, (uint16_t)(NVMEOF_STATUS_MAKE(sct, sc) | NVMEOF_STATUS_P_MASK));
}

// ===========================================================================
//  TARGET
// ===========================================================================
static int runTarget(const char* ip, uint16_t port) {
    printf("=== F1 TARGET on %s:%u\n", ip, port);

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, ip, &local.sin_addr);

    Transport t;
    if (!t.open(local, true)) return 1;
    Controller ctrl;
    strcpy_s(ctrl.subsysnqn, Controller::kNqn());

    HRESULT hr = t.listener->Bind((const sockaddr*)&local, sizeof(local));
    if (hr == ND_PENDING) hr = t.waitOverlapped(t.listener, kWaitMs);
    if (!ndOk(hr)) { printf("listener Bind failed\n"); t.close(); return 1; }
    t.listener->Listen(1);
    hr = t.listener->GetConnectionRequest(t.conn, &t.ov);
    if (hr == ND_PENDING) hr = t.waitOverlapped(t.listener, 20000);
    if (!ndOk(hr)) { printf("GetConnectionRequest failed\n"); t.close(); return 1; }

    // Arm the Receive BEFORE Accept, not after it.
    //
    // Accept is what releases the host's CompleteConnect, so a Receive posted
    // afterwards is racing a peer that is already entitled to send.  Losing that
    // race is not a retryable hiccup: the host's Send never completes at all -
    // no RNR retry, no error completion, just a 5 s timeout - and the initiator
    // reports status 0xFFFF out of a response buffer that was never written.
    // This test used to post the Receive inside the loop below and failed
    // intermittently for exactly that reason.
    if (!t.postReceive(t.reg + kCapsuleOff, 64, CTX_CAPSULE)) {
        printf("  initial Receive could not be posted\n");
        t.close();
        return 1;
    }
    // Validate the peer's private data and Accept, the way a real target does.
    // A real fabrics target refuses a Connect that carries none, so this one does
    // too: if the initiator above ever stops sending it, this is where it shows.
    {
        uint16_t claimedQid = 0, rejStatus = 0;
        int verdict = t.acceptChecked((uint16_t)kSqSize, &claimedQid, &rejStatus);
        if (verdict != 0) {
            printf("  connection not accepted (verdict=%d, nvme_rdma status=%u)\n",
                   verdict, rejStatus);
            t.close();
            return 1;
        }
    }
    printf("  connected; our rkey=0x%08X\n", t.rkey);

    // Serve the admin queue until the initiator disconnects.
    uint8_t idbuf[NVMEOF_IDENTIFY_SIZE];
    int served = 0;
    bool armed = true;      // the Receive above is already outstanding
    for (;;) {
        ND2_RESULT cr = {};
        hr = t.reap(CTX_CAPSULE, &cr, 20000);
        if (!ndOk(hr)) {
            char b[64];
            printf("  admin queue idle/closed (%s) after %d commands\n", ndStr(hr, b, sizeof(b)), served);
            break;
        }
        armed = false;
        if (cr.BytesTransferred != 64) {
            printf("  capsule length %u, expected 64 - dropping\n", cr.BytesTransferred);
            if (!t.postReceive(t.reg + kCapsuleOff, 64, CTX_CAPSULE)) break;
            continue;
        }
        // Re-arm BEFORE answering, not after.
        //
        // The peer is entitled to send its next command the instant it sees this
        // response, and if no Receive is posted by then the command is not
        // retried - it simply never completes, and the initiator reports
        // status 0xFFFF out of a response buffer that was never written.  That is
        // the same failure as arming after Accept (DESIGN 8.10(4)), one step
        // later in the sequence: there, the first command was lost; here, the
        // second.  It showed up as Property Set succeeding and Property Get
        // failing, which reads like a protocol problem and is an ordering one.
        //
        // The capsule is copied out first, because re-posting into the buffer it
        // lives in makes that memory available to the next capsule while the
        // handler is still reading it.
        uint8_t capCopy[64];
        memcpy(capCopy, t.reg + kCapsuleOff, 64);
        if (!t.postReceive(t.reg + kCapsuleOff, 64, CTX_CAPSULE)) break;
        armed = true;
        const uint8_t* cap = capCopy;
        uint8_t opcode = cap[0];
        uint16_t cid = nvmeof_rd16(cap + 2);
        uint8_t  fctype = cap[4];
        served++;

        uint8_t* cqe = t.reg + kCqeOutOff;      // inside the MR
        uint8_t sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
        uint64_t result = 0;

        // Two disjoint dispatch spaces share this queue.  A fabrics command is
        // opcode 0x7f and is discriminated by fctype; everything else is an
        // ordinary admin opcode.  Conflating them is how Identify ends up
        // rejected as an unknown fctype.
        if (opcode == NVMEOF_OPC_FABRICS) {
            if (fctype == NVMEOF_FCTYPE_PROPERTY_SET) {
                uint32_t off = nvmeof_rd32(cap + 44);
                uint64_t val = nvmeof_rd64(cap + 48);
                printf("  cmd %d: Property Set off=0x%X val=0x%llX\n", served, off,
                       (unsigned long long)val);
                if (off == NVMEOF_PROP_CC) {
                    ctrl.enabled = (val & 1) != 0;
                } else if (off == NVMEOF_PROP_CSTS) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD;  // read-only
                }
            } else if (fctype == NVMEOF_FCTYPE_PROPERTY_GET) {
                uint32_t off = nvmeof_rd32(cap + 44);
                printf("  cmd %d: Property Get off=0x%X\n", served, off);
                if (off == NVMEOF_PROP_CSTS) {
                    result = ctrl.enabled ? 1u : 0u;      // RDY
                } else if (off == NVMEOF_PROP_CC) {
                    result = ctrl.enabled ? 1u : 0u;
                } else {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD;
                }
            } else if (fctype == NVMEOF_FCTYPE_CONNECT) {
                uint16_t qid = nvmeof_rd16(cap + 42);
                uint16_t sqsize = nvmeof_rd16(cap + 44);
                printf("  cmd %d: Connect qid=%u sqsize=%u kato=%u\n", served, qid, sqsize,
                       nvmeof_rd32(cap + 48));
                uint8_t  type = nvmeof_sgl_type_of(cap[24 + 15]);
                uint8_t  sub  = nvmeof_sgl_subtype_of(cap[24 + 15]);
                uint64_t addr = nvmeof_rd64(cap + 24);
                uint32_t len  = nvmeof_rd24(cap + 24 + 8);
                uint32_t key  = nvmeof_rd32(cap + 24 + 11);
                printf("        SGL type=%u subtype=%u addr=0x%llX len=%u rkey=0x%08X\n",
                       type, sub, (unsigned long long)addr, len, key);
                if (type != NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK || sub != NVMEOF_SGL_SUBTYPE_ADDRESS) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_SGL_INVALID_TYPE;
                } else if (len < NVMEOF_CONNECT_DATA_SIZE) {
                    sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_FORMAT;
                } else if (!t.readInto(t.reg + kConnectOff, NVMEOF_CONNECT_DATA_SIZE,
                                       addr, key, CTX_MISC)) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                } else {
                    ND2_RESULT xr = {};
                    HRESULT xst = t.reap(CTX_MISC, &xr, kWaitMs);
                    if (!ndOk(xst)) {
                        char b[64];
                        printf("        Connect data Read %s\n", ndStr(xst, b, sizeof(b)));
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                    } else {
                        const uint8_t* cd = t.reg + kConnectOff;
                        uint16_t want = nvmeof_rd16(cd + 16);
                        memcpy(ctrl.hostid, cd + 0, 16);
                        memcpy(ctrl.subsysnqn, cd + 256, NVMEOF_NQN_FIELD_LEN);
                        memcpy(ctrl.hostnqn, cd + 512, NVMEOF_NQN_FIELD_LEN);
                        ctrl.kato = nvmeof_rd32(cap + 48);
                        printf("        hostnqn='%s'\n", ctrl.hostnqn);
                        if (strncmp(ctrl.subsysnqn, Controller::kNqn(), NVMEOF_NQN_FIELD_LEN) != 0) {
                            printf("        subsysnqn mismatch: '%s'\n", ctrl.subsysnqn);
                            sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                        } else if (qid != NVMEOF_QID_ADMIN) {
                            printf("        qid %u is not the admin queue\n", qid);
                            sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                        } else {
                            ctrl.cntlid = (want == NVMEOF_CNTLID_DYNAMIC) ? 0x0001 : want;
                            result = ctrl.cntlid;
                            printf("        cntlid=%u assigned\n", ctrl.cntlid);
                        }
                    }
                }
            } else {
                printf("  cmd %d: unhandled fctype 0x%02X\n", served, fctype);
                sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE;
            }
        } else if (opcode == NVMEOF_OPC_IDENTIFY) {
            uint8_t cns = cap[40];          // cdw10 byte 0
            uint32_t nsid = nvmeof_rd32(cap + 4);
            printf("  cmd %d: Identify cns=%u nsid=%u\n", served, cns, nsid);
            if (cns != NVMEOF_ID_CNS_CTRL) {
                sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD;
            } else {
                uint8_t  type = nvmeof_sgl_type_of(cap[24 + 15]);
                uint64_t addr = nvmeof_rd64(cap + 24);
                uint32_t len  = nvmeof_rd24(cap + 24 + 8);
                uint32_t key  = nvmeof_rd32(cap + 24 + 11);
                printf("        SGL type=%u addr=0x%llX len=%u rkey=0x%08X\n",
                       type, (unsigned long long)addr, len, key);
                if (type != NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_SGL_INVALID_TYPE;
                } else if (len < NVMEOF_IDENTIFY_SIZE) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                } else {
                    buildIdentifyController(idbuf, ctrl);
                    memcpy(t.reg + kScratchOff, idbuf, NVMEOF_IDENTIFY_SIZE);
                    // The TARGET moves the payload, into the host's memory, using
                    // the rkey the host published in the command's SGL.  This is
                    // the NVMe-oF data path in one call.
                    if (!t.writeFrom(t.reg + kScratchOff, NVMEOF_IDENTIFY_SIZE,
                                     addr, key, CTX_MISC)) {
                        printf("        RDMA Write of the Identify payload failed\n");
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                    } else {
                        ND2_RESULT wr = {};
                        HRESULT wst = t.reap(CTX_MISC, &wr, kWaitMs);
                        if (!ndOk(wst)) {
                            char b[64];
                            printf("        Identify payload Write %s\n", ndStr(wst, b, sizeof(b)));
                            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                        } else {
                            ctrl.identifyDataRequests++;
                            printf("        wrote %u bytes of Identify data to the host\n",
                                   wr.BytesTransferred);
                        }
                    }
                }
            }
        } else {
            printf("  cmd %d: unknown admin opcode 0x%02X\n", served, opcode);
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE;
        }

        buildCqe(cqe, cid, (uint16_t)served, 0, result, sct, sc);

        // The response goes back as a two-sided Send, into the receive the
        // initiator must already have posted.
        if (!t.sendBuf(cqe, 16, CTX_RESP)) {
            printf("  failed to send the completion; stopping\n");
            break;
        }
        ND2_RESULT sr = {};
        hr = t.reap(CTX_RESP, &sr, kWaitMs);
        if (!ndOk(hr)) { char b[64]; printf("  completion send %s\n", ndStr(hr, b, sizeof(b))); break; }
    }

    printf("  target served %d admin commands\n", served);
    t.close();
    printf("\ntarget failures: %d\n", g_failures);
    return g_failures;
}

// ===========================================================================
//  INITIATOR
// ===========================================================================
static int runInitiator(const char* serverIp, uint16_t port, const char* localIp) {
    printf("=== F1 INITIATOR %s -> %s:%u\n", localIp, serverIp, port);

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, localIp, &local.sin_addr);
    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    InetPtonA(AF_INET, serverIp, &remote.sin_addr);

    Transport t;
    if (!t.open(local, false)) return 1;
    printf("  local rkey=0x%08X, region at %p\n", t.rkey, (void*)t.reg);

    HRESULT hr = t.conn->Bind((const sockaddr*)&local, sizeof(local));
    if (hr == ND_PENDING) hr = t.waitOverlapped(t.conn, kWaitMs);
    Report("connector Bind", ndOk(hr), nullptr);

    // outboundReadLimit is not optional: with 0 this side cannot issue an RDMA
    // Read at all, and the Connect data transfer below would fail with a raw
    // status code that appears in no header.  See DESIGN.md 8.3.
    // Every Connect carries RDMA-CM private data, filled the way the Linux host
    // fills it.  This suite used to send none, which a real fabrics target
    // refuses outright (nvmet: private_data_len == 0 -> NVME_RDMA_CM_INVALID_LEN).
    nvmeof_rdma_request_pd pd;
    nvmeof_rdma_fill_req(&pd, 0, (uint16_t)kSqSize, 0);
    hr = t.conn->Connect(t.qp, (const sockaddr*)&remote, sizeof(remote),
                         kReadLimit, kReadLimit, &pd, sizeof(pd), &t.ov);
    if (hr == ND_PENDING) hr = t.waitOverlapped(t.conn, 20000);
    {
        char b[64];
        Report("Connect (inbound=outbound=16)", ndOk(hr), ndStr(hr, b, sizeof(b)));
    }
    if (!ndOk(hr)) { t.close(); return g_failures; }

    hr = t.conn->CompleteConnect(&t.ov);
    if (hr == ND_PENDING) hr = t.waitOverlapped(t.conn, kWaitMs);
    Report("CompleteConnect", ndOk(hr), nullptr);
    if (!ndOk(hr)) { t.close(); return g_failures; }

    // ---- 1. Property Set: enable the controller (CC.EN = 1)
    printf("\n-- controller bring-up\n");
    {
        uint8_t cap[64];
        memset(cap, 0, sizeof(cap));
        nvmeof_fabrics_property_set* p = (nvmeof_fabrics_property_set*)cap;
        p->opcode = NVMEOF_OPC_FABRICS;
        p->command_id = 1;
        p->fctype = NVMEOF_FCTYPE_PROPERTY_SET;
        p->offset = NVMEOF_PROP_CC;
        p->value = 1;                       // EN
        Completion c = {};
        bool sent = adminCommand(t, cap, &c, kWaitMs);
        char d[128];
        sprintf_s(d, "cid=%u status=0x%04X", c.cid, cqeStatus(t));
        Report("Property Set CC.EN=1", sent && cqeStatus(t) == (NVMEOF_STATUS_MAKE(0, 0) | 1), d);
    }

    // ---- 2. Property Get: poll CSTS until RDY
    {
        bool ready = false;
        for (int i = 0; i < 10 && !ready; i++) {
            uint8_t cap[64];
            memset(cap, 0, sizeof(cap));
            nvmeof_fabrics_property_get* p = (nvmeof_fabrics_property_get*)cap;
            p->opcode = NVMEOF_OPC_FABRICS;
            p->command_id = (uint16_t)(10 + i);
            p->fctype = NVMEOF_FCTYPE_PROPERTY_GET;
            p->offset = NVMEOF_PROP_CSTS;
            Completion c = {};
            if (!adminCommand(t, cap, &c, kWaitMs)) break;
            ready = (c.result & 1u) != 0;
            if (ready) {
                char d[96];
                sprintf_s(d, "CSTS=0x%llX after %d poll(s)", (unsigned long long)c.result, i + 1);
                Report("Property Get CSTS reports RDY", true, d);
            }
        }
        if (!ready) Report("Property Get CSTS reports RDY", false, "never became ready");
    }

    // ---- 3. Connect (admin queue).  The SGL points at the Connect data inside
    //         OUR registered region, and carries OUR rkey - the target pulls it.
    printf("\n-- fabrics Connect\n");
    uint16_t assignedCntlid = 0;
    {
        uint8_t* cd = t.reg + kConnectOff;
        memset(cd, 0, NVMEOF_CONNECT_DATA_SIZE);
        for (int i = 0; i < 16; i++) cd[i] = (uint8_t)(0xA0 + i);   // host id
        nvmeof_wr16(cd + 16, NVMEOF_CNTLID_DYNAMIC);
        const char* subnqn = "nqn.2024-01.local.rdma:windows-nd";
        const char* hostnqn = "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-initiator-0001";
        memcpy(cd + 256, subnqn, strlen(subnqn));
        memcpy(cd + 512, hostnqn, strlen(hostnqn));

        uint8_t cap[64];
        memset(cap, 0, sizeof(cap));
        nvmeof_fabrics_connect* c = (nvmeof_fabrics_connect*)cap;
        c->opcode = NVMEOF_OPC_FABRICS;
        c->command_id = 100;
        c->fctype = NVMEOF_FCTYPE_CONNECT;
        nvmeof_sgl_set_keyed(&c->dptr, (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, t.rkey);
        c->recfmt = 0;
        c->qid = NVMEOF_QID_ADMIN;
        c->sqsize = 31;                     // 0-based: 32 entries
        c->cattr = 0;
        c->kato = 30000;
        Completion cc = {};
        bool sent = adminCommand(t, cap, &cc, kWaitMs);
        assignedCntlid = (uint16_t)cc.result;
        char d[160];
        sprintf_s(d, "cntlid=%u status=0x%04X", assignedCntlid, cqeStatus(t));
        Report("Connect admin queue, cntlid assigned", sent && assignedCntlid != 0, d);
    }

    // ---- 4. Identify Controller.  The TARGET writes the 4096-byte payload into
    //         our region using the rkey in this command's SGL.
    printf("\n-- Identify Controller (data written by the target)\n");
    {
        memset(t.reg + kIdentifyOff, 0xCC, NVMEOF_IDENTIFY_SIZE);   // poison
        uint8_t cap[64];
        memset(cap, 0, sizeof(cap));
        nvmeof_sqe* s = (nvmeof_sqe*)cap;
        s->opcode = NVMEOF_OPC_IDENTIFY;
        s->command_id = 200;
        s->nsid = 0;
        nvmeof_sgl_set_keyed(&s->dptr, (uint64_t)(uintptr_t)(t.reg + kIdentifyOff),
                             NVMEOF_IDENTIFY_SIZE, t.rkey);
        cap[40] = NVMEOF_ID_CNS_CTRL;       // cdw10 byte 0 = CNS
        Completion c = {};
        bool sent = adminCommand(t, cap, &c, kWaitMs);
        char d[128];
        sprintf_s(d, "status=0x%04X", cqeStatus(t));
        bool idOk = sent && cqeStatus(t) == (NVMEOF_STATUS_MAKE(0, 0) | 1);
        Report("Identify Controller command completed", idOk, d);
        if (!idOk) {
            // Everything below inspects the payload, and the payload is poison
            // unless the command actually succeeded.  Checking it anyway is how
            // a 0xCC-filled buffer "passed" two of these assertions before.
            printf("  [skip] payload checks: the command did not succeed\n");
            t.close();
            printf("\ninitiator failures: %d\n", g_failures);
            return g_failures;
        }

        // Verify the payload the TARGET wrote, field by field against what the
        // target's buildIdentifyController() is known to produce.  A short or
        // mis-based transfer shows up as poison still present.
        const uint8_t* id = t.reg + kIdentifyOff;
        nvmeof_id_ctrl_buf idb;
        memcpy(idb.raw, id, NVMEOF_IDENTIFY_SIZE);
        // Copy out and terminate: these fields are fixed-width and NOT
        // NUL-terminated on the wire, so printing them in place runs off the end
        // of the buffer and prints whatever follows.  (Seen, in this project, as
        // a screenful of 0xCC.)
        char sn[21] = {}, mn[41] = {}, subnqn[257] = {};
        memcpy(sn, nvmeof_idc_str(&idb, NVMEOF_ID_CTRL_OFF_SN), 20);
        memcpy(mn, nvmeof_idc_str(&idb, NVMEOF_ID_CTRL_OFF_MN), 40);
        memcpy(subnqn, nvmeof_idc_str(&idb, NVMEOF_ID_CTRL_OFF_SUBNQN), 256);
        printf("  identify: sn='%s'\n", sn);
        printf("            mn='%s'\n", mn);
        printf("            subnqn='%s'\n", subnqn);
        printf("            cntlid=%u nn=%u mdts=%u sqes=%u cqes=%u\n",
               nvmeof_idc_u16(&idb, NVMEOF_ID_CTRL_OFF_CNTLID),
               nvmeof_idc_u32(&idb, NVMEOF_ID_CTRL_OFF_NN),
               idb.raw[NVMEOF_ID_CTRL_OFF_MDTS],
               idb.raw[NVMEOF_ID_CTRL_OFF_SQES],
               idb.raw[NVMEOF_ID_CTRL_OFF_CQES]);

        {
            char d2[128];
            sprintf_s(d2, "cntlid=%u matches the Connect response (%u)",
                      nvmeof_idc_u16(&idb, NVMEOF_ID_CTRL_OFF_CNTLID), assignedCntlid);
            Report("Identify reports the assigned controller id",
                   nvmeof_idc_u16(&idb, NVMEOF_ID_CTRL_OFF_CNTLID) == assignedCntlid, d2);
        }
        Report("Identify serial number came through intact",
               memcmp(sn, "NDVMEOF0000000000001", 20) == 0, sn);
        Report("Identify model string came through intact",
               memcmp(mn, "NetworkDirect NVMe-oF prototype          ", 40) == 0, nullptr);
        Report("payload is fully written (no poison byte left)",
               id[NVMEOF_IDENTIFY_SIZE - 1] != 0xCC, nullptr);

        // The initiator is REQUIRED to read these before issuing SGLs; a
        // controller that does not advertise keyed data-block SGLs cannot be
        // driven by the code above at all.
        uint32_t sgls = nvmeof_idc_u32(&idb, NVMEOF_ID_CTRL_OFF_SGLS);
        {
            char d3[192];
            sprintf_s(d3, "sgls=0x%08X (keyed=%s, byte-aligned=%s, in-capsule=%s)",
                      sgls,
                      (sgls & NVMEOF_CTRL_SGLS_KEYED) ? "yes" : "no",
                      (sgls & NVMEOF_CTRL_SGLS_BYTE_ALIGNED) ? "yes" : "no",
                      (sgls & NVMEOF_CTRL_SGLS_SAOS) ? "claimed" : "not claimed");
            // Keyed descriptors are required.  In-capsule data (SAOS) must NOT be
            // claimed: this controller rejects offset descriptors, so advertising
            // the capability would invite a host to send a request it is told is
            // legal and get an error back.
            Report("controller advertises keyed SGLs and does not claim in-capsule data",
                   (sgls & NVMEOF_CTRL_SGLS_KEYED) != 0 &&
                       (sgls & NVMEOF_CTRL_SGLS_SAOS) == 0, d3);
        }
        {
            uint32_t ioccsz = nvmeof_idc_u32(&idb, NVMEOF_ID_CTRL_OFF_IOCCSZ);
            uint32_t iorcsz = nvmeof_idc_u32(&idb, NVMEOF_ID_CTRL_OFF_IORCSZ);
            uint16_t icdoff = nvmeof_idc_u16(&idb, NVMEOF_ID_CTRL_OFF_ICDOFF);
            char d4[160];
            sprintf_s(d4, "ioccsz=%u (%u B) iorcsz=%u (%u B) icdoff=%u",
                      ioccsz, ioccsz * 16, iorcsz, iorcsz * 16, icdoff);
            // These are read, never assumed - a hardcoded capsule size works
            // against exactly one target.  The lower bounds are the Linux host's
            // own acceptance rule (nvme_check_ctrl_fabric_info rejects a fabrics
            // controller with ioccsz < 4 or iorcsz < 1), and icdoff must be 0
            // while no in-capsule data is advertised - the two fields have to
            // agree with each other and with the receive buffer.
            Report("capsule sizes are readable, sane, and agree with the SGL claim",
                   ioccsz >= 4 && iorcsz >= 1 && icdoff == 0 &&
                       (ioccsz * 16 == 64) == ((sgls & NVMEOF_CTRL_SGLS_SAOS) == 0), d4);
        }
    }

    t.close();
    printf("\ninitiator failures: %d\n", g_failures);
    return g_failures;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage:\n");
        printf("  %s -target    <serverIp> <port>\n", argv[0]);
        printf("  %s -initiator <serverIp> <port> <localIp>\n", argv[0]);
        return 2;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    HRESULT hr = NdStartup();
    if (FAILED(hr)) { printf("NdStartup 0x%08X\n", (unsigned)hr); WSACleanup(); return 1; }

    int rc = 2;
    if (strcmp(argv[1], "-target") == 0) {
        rc = runTarget(argv[2], (uint16_t)atoi(argv[3]));
    } else if (strcmp(argv[1], "-initiator") == 0 && argc >= 5) {
        rc = runInitiator(argv[2], (uint16_t)atoi(argv[3]), argv[4]);
    } else {
        printf("unknown mode %s\n", argv[1]);
    }

    NdCleanup();
    WSACleanup();
    return rc;
}
