// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// f4_pipeline.cpp - F4/F5 milestone: many commands in flight, and a throughput
// number for the whole NVMe-oF path.
//
// WHY PIPELINING IS THE NEXT STEP, NOT AN OPTIMISATION
//
// f3_io.cpp issues one command and waits for its completion.  That works, and it
// hides every bug that only appears when several commands overlap:
//
//   * command id matching, when completions arrive out of order
//   * per-command capsule and response buffers, instead of one shared pair
//   * the target issuing several RDMA Reads before reaping any of them, which
//     is exactly what outboundReadLimit governs
//   * the phase bit, which only means anything once completions wrap a queue
//
// A target that is only ever asked for one command at a time can be wrong about
// all of these and still look correct.  So the point of this file is to be the
// test that a serial implementation cannot pass by accident.
//
// It also produces the number the project actually cares about: what fraction of
// the bare RDMA Write rate (2.53 GB/s measured on this card, see
// RDMA_TOOLBOX.md) survives the capsule and SGL layering.
//
// USAGE
//   f4_pipeline.exe -target    192.168.100.2 54360 [-xfer 32768] [-rounds 64]
//   f4_pipeline.exe -initiator 192.168.100.2 54360 192.168.100.3 [-xfer ...] [-rounds ...]
//
// -xfer exists so one binary can sweep the command size.  A single size cannot
// answer whether the pipeline is limited by per-command cost or by bandwidth,
// and the two have completely different fixes.

#include "nvmeof_wire.h"
#include "nvmeof_rdma.h"
#include <stdlib.h>

using namespace nvmeof;

// The data pattern is buf[k] = (k*7 + round*31 + slot*131) mod 256.  Because
// gcd(7,256) == 1 the term k*7 mod 256 is periodic with period 256, so the whole
// pattern is one 256-byte template repeated.  Filling or checking it byte at a
// time costs several cycles per byte, and at these transfer rates that cost was
// what the benchmark measured: throughput came out flat at ~550 MiB/s across
// command sizes from 32 KiB to 512 KiB, which is the signature of a per-byte CPU
// cost rather than a per-command or per-transfer one.  Building the template
// once and memcpy/memcmp-ing it in 256-byte steps keeps the pattern exactly the
// same - so the correctness check is not weakened - while taking the harness out
// of the measurement.
static void buildPattern(uint8_t tpl[256], int round, int slot) {
    for (int k = 0; k < 256; k++) {
        tpl[k] = (uint8_t)((k * 7u + (unsigned)round * 31u + (unsigned)slot * 131u) & 0xFF);
    }
}

// Returns the offset of the first mismatch, or bytes if the buffer is correct.
static size_t patternMismatch(const uint8_t* buf, size_t bytes, const uint8_t tpl[256]) {
    for (size_t off = 0; off + 256 <= bytes; off += 256) {
        if (memcmp(buf + off, tpl, 256) != 0) {
            for (size_t k = off; k < off + 256; k++) if (buf[k] != tpl[k - off]) return k;
        }
    }
    return bytes;
}

static int g_failures = 0;
static void Report(const char* what, int ok, const char* detail) {
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           (detail && *detail) ? " - " : "", (detail && *detail) ? detail : "");
    if (!ok) g_failures++;
}

// ---------------------------------------------------------------------------
//  Region layout
// ---------------------------------------------------------------------------
static const int      kMaxInFlight = 8;          // <= outboundReadLimit (16)
static const uint32_t kBlockSize   = 512;
static const uint16_t kSqSize      = 31;         // 0-based: 32 entries

static const size_t kCapInOff      = 0;                              // kMaxInFlight * 64
static const size_t kRespOutOff    = kCapInOff      + kMaxInFlight * 64;
static const size_t kRespInOff     = kRespOutOff    + kMaxInFlight * 16;
static const size_t kConnectOff    = kRespInOff     + kMaxInFlight * 16 + 64;
static const size_t kIdCtrlOff     = kConnectOff    + 2048;
static const size_t kIdNsOff       = kIdCtrlOff     + NVMEOF_IDENTIFY_SIZE;
static const size_t kXferOff       = kIdNsOff       + NVMEOF_IDENTIFY_SIZE;

// ---------------------------------------------------------------------------
//  Workload, sized at run time
// ---------------------------------------------------------------------------
// Command size is a run-time parameter so one binary can sweep it.  Whether a
// pipelined implementation is limited by per-command cost or by bandwidth is
// not answerable from a single size: the earlier fixed 32 KiB measured
// 536 MiB/s, which is 55 us per round of 8 commands and therefore says far more
// about per-command overhead than about the fabric.  Both endpoints are started
// by one script with identical arguments, and every offset below is derived
// from these two numbers, so the two sides cannot disagree about the layout.
//
// The namespace has to hold the whole write set.  It did not once, and the LBA
// mapping wrapped with a period that folded the last round exactly onto the
// first, so the read phase read back the *other* round's bytes and the payload
// check failed on a protocol that was in fact working.  The tell was that the
// mismatching bytes differed from the expected ones by a constant
// (63 * 31 == 0xA1, the round term of the pattern generator), which is what
// pointed at "wrong round" rather than "corrupt data".
static size_t   gXferBytes     = 32u * 1024;     // payload per command
static int      gRounds        = 64;             // rounds of kMaxInFlight commands
static uint32_t gBlocksPerXfer = 0;
static size_t   gXferAreaBytes = 0;              // one buffer per command
static size_t   gNsOff         = 0;
static size_t   gNsBytes       = 0;
static uint32_t gNsBlocks      = 0;
static size_t   gRegionBytes   = 0;

static size_t roundUp(size_t v, size_t to) { return (v + to - 1) / to * to; }

static bool layoutInit(size_t xferBytes, int rounds) {
    if (xferBytes == 0 || xferBytes % kBlockSize != 0) {
        printf("xfer size must be a non-zero multiple of %u (got %zu)\n", kBlockSize, xferBytes);
        return false;
    }
    if (rounds <= 0 || rounds > 8192) {
        printf("rounds must be in 1..8192 (got %d)\n", rounds);
        return false;
    }
    if ((xferBytes / kBlockSize) > 0xFFFFu) {
        printf("xfer size too large: NLB is a 16-bit field, so at most %u blocks\n", 0xFFFFu);
        return false;
    }
    gXferBytes     = xferBytes;
    gRounds        = rounds;
    gBlocksPerXfer = (uint32_t)(xferBytes / kBlockSize);
    // Every command in a phase gets its own buffer, and the transfer area is the
    // same size as the namespace the target reads from.  An earlier revision
    // reused kMaxInFlight slots for every round, which made the write phase read
    // its source from a 4 MiB window that stays in cache while the read phase
    // swept the whole namespace out of DRAM - so the write direction looked
    // 93% of the bare-RDMA ceiling and the read direction only 70%, and the gap
    // was the benchmark's cache residency rather than the protocol.  Sizing both
    // areas the same way is what makes the two directions comparable, and it
    // also means no slot is ever reused, so a stale buffer cannot pass.
    uint64_t writeSet = (uint64_t)kMaxInFlight * (uint64_t)rounds * xferBytes;
    gXferAreaBytes = roundUp((size_t)writeSet, 1u << 20);
    gNsOff         = kXferOff + gXferAreaBytes + 4096;
    gNsBytes       = gXferAreaBytes;
    gNsBlocks      = (uint32_t)(gNsBytes / kBlockSize);
    gRegionBytes   = roundUp(gNsOff + gNsBytes, 1u << 20);

    // The same two invariants the compile-time asserts used to carry, now that
    // the sizes are dynamic.
    if ((uint64_t)kMaxInFlight * rounds * gBlocksPerXfer > gNsBlocks) {
        printf("layout error: workload needs %llu blocks, namespace has %u\n",
               (unsigned long long)((uint64_t)kMaxInFlight * rounds * gBlocksPerXfer), gNsBlocks);
        return false;
    }
    if (gNsOff + gNsBytes > gRegionBytes) {
        printf("layout error: namespace does not fit in the registered region\n");
        return false;
    }
    return true;
}

static const char*  kSubNqn        = "nqn.2024-01.local.rdma:windows-nd";

// Per-slot completion contexts.  Distinct values are what make slot ownership
// checkable; a shared context would let one command's completion be credited to
// another and the test would still pass.
#define CTX_CAP_BASE  ((uintptr_t)0x1000)
#define CTX_RESP_BASE ((uintptr_t)0x2000)
#define CTX_DATA_BASE ((uintptr_t)0x3000)

struct Completion { uint16_t cid; uint16_t status; uint64_t result; };

static bool statusOk(uint16_t st) {
    return (st & NVMEOF_STATUS_P_MASK) != 0 &&
           nvmeof_status_sct(st) == NVMEOF_SCT_GENERIC &&
           nvmeof_status_sc(st) == 0;
}

static void buildCompletion(uint8_t* buf, uint16_t cid, uint16_t sqHead,
                            uint64_t result, uint8_t sct, uint8_t sc) {
    memset(buf, 0, 16);
    nvmeof_wr64(buf + 0, result);
    nvmeof_wr16(buf + 8, sqHead);
    nvmeof_wr16(buf + 10, 0);
    nvmeof_wr16(buf + 12, cid);
    nvmeof_wr16(buf + 14, NVMEOF_STATUS_CQE(sct, sc));
}

struct Sgl { bool present; uint8_t type; uint64_t addr; uint32_t len; uint32_t key; };
static Sgl parseSgl(const uint8_t* capsule) {
    Sgl s = {};
    s.type = nvmeof_sgl_type_of(capsule[24 + 15]);
    s.addr = nvmeof_rd64(capsule + 24);
    s.len  = nvmeof_rd24(capsule + 24 + 8);
    s.key  = nvmeof_rd32(capsule + 24 + 11);
    s.present = (s.type == NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK);
    return s;
}

static void fillIdentifyCtrl(uint8_t* id, uint16_t cntlid) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_VID, 0x15B3);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SN, "NDVMEOF0000000000001", 20);
    memcpy(id + NVMEOF_ID_CTRL_OFF_MN, "NetworkDirect NVMe-oF prototype          ", 40);
    memcpy(id + NVMEOF_ID_CTRL_OFF_FR, "0.4.0   ", 8);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_CNTLID, cntlid);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_VER, 0x00010400);
    id[NVMEOF_ID_CTRL_OFF_CNTRLTYPE] = NVMEOF_CTRLTYPE_IO;
    // sqes/cqes pack the MAXIMUM entry size in the high nibble and the REQUIRED
    // size in the low nibble.  Writing just 6 advertises a maximum entry size of
    // 2^0 = 1 byte, which is what this used to do.
    id[NVMEOF_ID_CTRL_OFF_SQES] = (uint8_t)((0x6 << 4) | 0x6);
    id[NVMEOF_ID_CTRL_OFF_CQES] = (uint8_t)((0x4 << 4) | 0x4);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_MAXCMD, (uint16_t)(kMaxInFlight * 8));
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_NN, 1);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_SGLS,
                NVMEOF_CTRL_SGLS_ADVERTISED);
    id[NVMEOF_ID_CTRL_OFF_VWC] = 1;
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_KAS, 1);   // 1 s keep-alive granularity
    memcpy(id + NVMEOF_ID_CTRL_OFF_SUBNQN, kSubNqn, strlen(kSubNqn));
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ, 8);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IORCSZ, 1);
    // ICDOFF must be 0: the RDMA transport carries no in-capsule data, and a
    // nonzero value claims there are bytes of data inside the command capsule at
    // an offset that lands on the command fields themselves.
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_ICDOFF, 0);
    id[NVMEOF_ID_CTRL_OFF_MSDBD] = 1;
}

static void fillIdentifyNs(uint8_t* id) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NSZE, gNsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NCAP, gNsBlocks);
    id[NVMEOF_ID_NS_OFF_NLBAF] = 0;
    id[NVMEOF_ID_NS_OFF_FLBAS] = 0;
    id[NVMEOF_ID_NS_OFF_LBAF + 2] = 9;    // ds = 9 -> 512 bytes
}

// ---------------------------------------------------------------------------
//  Target controller state
// ---------------------------------------------------------------------------
struct TargetState {
    bool enabled = false;
    uint16_t cntlid = 1;
    int ioQueuesGranted = 0;     // from Set Features: Number of Queues
    int katoMs = 0;
    uint32_t ioCommands = 0;
    uint64_t bytesWritten = 0;
    uint64_t bytesRead = 0;
};

// Process one admin capsule.  Returns false to stop the target.
static bool targetAdmin(Queue& q, Device& d, TargetState& st) {
    const uint8_t* cap = d.region(kCapInOff);
    uint8_t opcode = cap[0];
    uint16_t cid = nvmeof_rd16(cap + 2);
    uint8_t fctype = cap[4];
    uint8_t sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
    uint64_t result = 0;
    Sgl sgl = parseSgl(cap);

    if (opcode == NVMEOF_OPC_FABRICS) {
        if (fctype == NVMEOF_FCTYPE_PROPERTY_SET) {
            uint32_t off = nvmeof_rd32(cap + 44);
            uint64_t val = nvmeof_rd64(cap + 48);
            printf("  [admin] Property Set off=0x%X val=0x%llX\n", off, (unsigned long long)val);
            if (off == NVMEOF_PROP_CC) st.enabled = (val & 1) != 0;
            else { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD; }
        } else if (fctype == NVMEOF_FCTYPE_PROPERTY_GET) {
            uint32_t off = nvmeof_rd32(cap + 44);
            result = (st.enabled ? 1u : 0u);
            printf("  [admin] Property Get off=0x%X -> %llu\n", off, (unsigned long long)result);
        } else if (fctype == NVMEOF_FCTYPE_CONNECT) {
            uint16_t qid = nvmeof_rd16(cap + 42);
            printf("  [admin] Connect qid=%u\n", qid);
            if (!sgl.present || sgl.len < NVMEOF_CONNECT_DATA_SIZE) {
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_FORMAT;
            } else if (!q.read(d.region(kConnectOff), NVMEOF_CONNECT_DATA_SIZE, sgl.addr, sgl.key,
                               (void*)(CTX_DATA_BASE + 100))) {
                sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                ND2_RESULT r = {};
                if (!ndOk(q.reap((void*)(CTX_DATA_BASE + 100), &r, kWaitMs))) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                } else {
                    const uint8_t* cd = d.region(kConnectOff);
                    char subnqn[257] = {};
                    memcpy(subnqn, cd + 256, 256);
                    if (strcmp(subnqn, kSubNqn) != 0) {
                        printf("        subsysnqn mismatch '%s'\n", subnqn);
                        sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                    } else if (qid == 0) {
                        result = st.cntlid;
                    } else {
                        // An I/O queue's Connect belongs on its own queue pair,
                        // never on the admin queue pair.
                        printf("        qid %u on the admin queue pair is not allowed\n", qid);
                        sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                    }
                }
            }
        } else { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE; }
    } else if (opcode == NVMEOF_OPC_SET_FEATURES || opcode == NVMEOF_OPC_GET_FEATURES) {
        // A host sets Number of Queues before it will connect any I/O queue, so
        // this is what tells the target to start listening for them - not
        // Create SQ, which in NVMe-oF is not a legal command at all.
        bool isSet = (opcode == NVMEOF_OPC_SET_FEATURES);
        uint8_t fid = (uint8_t)(nvmeof_rd32(cap + 40) & 0xff);
        uint32_t cdw11 = nvmeof_rd32(cap + 44);
        printf("  [admin] %s Features fid=0x%02X\n", isSet ? "Set" : "Get", fid);
        switch (fid) {
        case NVMEOF_FID_NUM_QUEUES:
            if (isSet) {
                int want = (int)(cdw11 & 0xffff) + 1;
                st.ioQueuesGranted = want < 1 ? 1 : 1;   // F4 drives a single I/O queue
                printf("        host asked for %d I/O queues, granting %d\n", want,
                       st.ioQueuesGranted);
            }
            result = (uint64_t)((st.ioQueuesGranted - 1) & 0xffff) |
                     ((uint64_t)((st.ioQueuesGranted - 1) & 0xffff) << 16);
            break;
        case NVMEOF_FID_KATO:
            if (isSet) st.katoMs = (int)cdw11;
            result = (uint64_t)((st.katoMs + 999) / 1000);
            break;
        default:
            printf("        unsupported feature\n");
            sc = NVMEOF_SC_INVALID_FIELD;
            break;
        }
    } else if (opcode == NVMEOF_OPC_KEEP_ALIVE) {
        if (st.katoMs == 0) { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_KA_TIMEOUT_INVALID; }
        else printf("  [admin] Keep Alive\n");
    } else if (opcode == NVMEOF_OPC_CREATE_SQ || opcode == NVMEOF_OPC_CREATE_CQ ||
               opcode == NVMEOF_OPC_DELETE_SQ || opcode == NVMEOF_OPC_DELETE_CQ) {
        // Refused on purpose: in NVMe-oF an I/O queue is created by the Connect
        // command and destroyed by dropping its queue pair.  Linux's nvmet
        // answers all four with Invalid Opcode for every fabrics transport.
        printf("  [admin] opcode=0x%02X (Create/Delete I/O SQ/CQ) -> Invalid Opcode\n", opcode);
        sc = NVMEOF_SC_INVALID_OPCODE;
    } else if (opcode == NVMEOF_OPC_IDENTIFY) {
        uint8_t cns = cap[40];
        printf("  [admin] Identify cns=%u\n", cns);
        uint8_t* src = nullptr;
        if (cns == NVMEOF_ID_CNS_CTRL) { fillIdentifyCtrl(d.region(kIdCtrlOff), st.cntlid); src = d.region(kIdCtrlOff); }
        else if (cns == NVMEOF_ID_CNS_NS) { fillIdentifyNs(d.region(kIdNsOff)); src = d.region(kIdNsOff); }
        else { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD; }
        if (src) {
            if (!q.write(src, NVMEOF_IDENTIFY_SIZE, sgl.addr, sgl.key, (void*)(CTX_DATA_BASE + 101))) {
                sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                ND2_RESULT r = {};
                if (!ndOk(q.reap((void*)(CTX_DATA_BASE + 101), &r, kWaitMs))) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                }
            }
        }
    } else { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE; }

    buildCompletion(d.region(kRespOutOff), cid, 0, result, sct, sc);
    // Re-arm the admin Receive BEFORE answering.  The host may send its next
    // command the moment it sees this completion, and a command that arrives with
    // no Receive posted is not retried - it never completes at all
    // (DESIGN 8.10(4)).  The caller must therefore not re-arm again.
    q.postReceive(d.region(kCapInOff), 64, (void*)(CTX_CAP_BASE + 100));
    if (!q.send(d.region(kRespOutOff), 16, (void*)(CTX_RESP_BASE + 100))) return false;
    ND2_RESULT r = {};
    if (!ndOk(q.reap((void*)(CTX_RESP_BASE + 100), &r, kWaitMs))) return false;
    return true;
}

// ===========================================================================
//  TARGET
// ===========================================================================
static int runTarget(const char* ip, uint16_t port) {
    printf("=== F4 TARGET on %s:%u (pipelined, %d commands in flight)\n", ip, port, kMaxInFlight);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, ip, &local.sin_addr);

    Device dev;
    if (!dev.open(local, gRegionBytes)) return 1;
    Queue admin, io;
    if (!admin.create(&dev, 0)) return 1;
    if (!io.create(&dev, 1)) return 1;

    IND2Listener* listener = nullptr;
    HRESULT hr = dev.adapter->CreateListener(IID_IND2Listener, dev.ovFile, (VOID**)&listener);
    if (FAILED(hr)) { printf("CreateListener failed\n"); return 1; }
    // IND2Listener::Bind is synchronous - it takes no OVERLAPPED and cannot
    // pend, so there is nothing to wait for here.
    hr = listener->Bind((const sockaddr*)&local, sizeof(local));
    if (!ndOk(hr)) { char b[64]; printf("listener Bind %s\n", ndStr(hr, b, sizeof(b))); return 1; }
    listener->Listen(2);

    hr = listener->GetConnectionRequest(admin.conn, &admin.ov);
    if (hr == ND_PENDING) hr = admin.waitOverlapped(listener, 20000);
    if (!ndOk(hr)) { printf("admin connection request failed\n"); return 1; }

    // Arm the admin Receive BEFORE Accept, not after it.
    //
    // Accept is what makes the queue pair usable, so it is also what releases
    // the initiator's CompleteConnect.  A Receive posted after Accept is
    // therefore racing a peer that is already entitled to send, and the window
    // contains a printf.  Losing that race is not a retryable hiccup here: the
    // initiator's Send simply never completes at all (no RNR retry, no error
    // completion, just a 5 s timeout), and the target reports its Receive as
    // ND_CANCELED when the initiator finally exits.  That reads like a broken
    // connection and is really just an unarmed Receive.
    //
    // GetConnectionRequest is the reason this is posted here rather than
    // earlier: issuing it while a Receive is already pending cancels that
    // Receive, so the Receive has to come after it.
    if (!admin.postReceive(dev.region(kCapInOff), 64, (void*)(CTX_CAP_BASE + 100))) {
        printf("  FATAL: initial admin Receive could not be posted\n");
        return 1;
    }
    {
        // Same private-data contract as a Linux target: this suite's initiator
        // used to send none at all, which nvmet refuses outright.
        uint16_t claimedQid = 0, rejStatus = 0;
        int verdict = admin.acceptChecked((uint16_t)(kSqSize + 1), &claimedQid, &rejStatus);
        if (verdict != 0 || claimedQid != 0) {
            printf("admin Accept refused (verdict=%d qid=%u nvme_rdma status=%u)\n",
                   verdict, claimedQid, rejStatus);
            return 1;
        }
    }
    printf("  admin connected (rkey=0x%08X), initial Receive already armed (ctx=0x%llX)\n",
           dev.rkey, (unsigned long long)(CTX_CAP_BASE + 100));

    TargetState st;
    int dbgPolls = 0;
    bool ioConnected = false;
    bool ioRequestPending = false;
    bool adminDead = false;
    ULONGLONG lastActivity = GetTickCount64();

    // ---- per-slot state for the pipelined I/O path ----
    struct Slot { bool used; uint16_t cid; uint32_t bytes; uint64_t slba; bool isWrite; };
    Slot slots[kMaxInFlight] = {};
    bool capPosted[kMaxInFlight] = {};

    for (;;) {
        bool didWork = false;

        // ---- admin ----
        if (!adminDead) {
            ND2_RESULT r = {};
            if (admin.poll(&r)) {
                if (dbgPolls < 8) {
                    printf("  [dbg] admin cqe ctx=0x%llX type=%d st=0x%08X bytes=%u\n",
                           (unsigned long long)(uintptr_t)r.RequestContext,
                           (int)r.RequestType, (unsigned)r.Status, r.BytesTransferred);
                    dbgPolls++;
                }
                if (r.RequestContext == (void*)(CTX_CAP_BASE + 100) && ndOk(r.Status) &&
                    r.BytesTransferred == 64) {
                    if (!targetAdmin(admin, dev, st)) break;
                    // No re-arm here: targetAdmin arms the next admin Receive
                    // before it sends its completion.
                    didWork = true;
                } else if (r.RequestContext == (void*)(CTX_CAP_BASE + 100)) {
                    // A Receive that delivered no capsule.  ND_CANCELED is
                    // terminal: every Receive posted afterwards completes
                    // ND_CANCELED immediately too, so re-arming here does not
                    // revive the queue pair, it only spins (this branch used to
                    // emit ~1000 lines of it).  Declare the admin queue pair dead
                    // and let the Keep Alive check at the end of the run say
                    // whether that matters.
                    printf("  [admin] Receive did not deliver a capsule (st=0x%08X %s); "
                           "admin queue pair declared dead\n",
                           (unsigned)r.Status,
                           r.Status == ND_CANCELED ? "ND_CANCELED" : "short");
                    adminDead = true;
                } else { didWork = true; }
            }
        }

        // ---- I/O queue pair: Set Features: Number of Queues asks for it ----
        // Non-blocking on purpose.  Blocking in GetConnectionRequest stops the
        // target answering the admin queue, and the host is normally still doing
        // admin work at exactly this moment.
        if (st.ioQueuesGranted > 0 && !ioConnected && !ioRequestPending) {
            HRESULT h2 = listener->GetConnectionRequest(io.conn, &io.ov);
            ioRequestPending = (h2 == ND_PENDING) || SUCCEEDED(h2);
            if (ioRequestPending) {
                printf("  [accept] Number of Queues granted; waiting for the I/O queue pair\n");
                // Issuing the request can cancel the admin Receive; re-arm it.
                admin.postReceive(dev.region(kCapInOff), 64, (void*)(CTX_CAP_BASE + 100));
            }
        } else if (ioRequestPending) {
            HRESULT h2 = io.pollOverlapped(io.conn);
            if (h2 != ND_PENDING) {
                ioRequestPending = false;
                char b0[64];
                printf("  [accept] GetConnectionRequest -> %s\n", ndStr(h2, b0, sizeof(b0)));
                // Arm the I/O receives before Accept: the host is entitled to
                // send its fabrics Connect the moment its CompleteConnect
                // returns.
                int armed = 0;
                for (int i = 0; i < kMaxInFlight; i++) {
                    capPosted[i] = io.postReceive(dev.region(kCapInOff + i * 64), 64,
                                                  (void*)(CTX_CAP_BASE + i));
                    if (capPosted[i]) armed++;
                }
                if (ndOk(h2)) {
                    uint16_t claimedQid = 0, rejStatus = 0;
                    int verdict = io.acceptChecked((uint16_t)(kSqSize + 1), &claimedQid, &rejStatus);
                    if (verdict != 0 || claimedQid != 1) {
                        printf("  [accept] I/O queue refused (verdict=%d qid=%u nvme_rdma status=%u)\n",
                               verdict, claimedQid, rejStatus);
                        h2 = ND_UNSUCCESSFUL;
                    }
                }
                printf("  [accept] Accept -> %s\n", ndStr(h2, b0, sizeof(b0)));
                // Re-arm the admin Receive unconditionally: the connection
                // request above cancels a Receive pending on another queue pair.
                if (!admin.postReceive(dev.region(kCapInOff), 64,
                                       (void*)(CTX_CAP_BASE + 100))) break;
                if (ndOk(h2)) {
                    ioConnected = true;
                    printf("  [accept] I/O queue connected; %d/%d capsule receives armed\n",
                           armed, kMaxInFlight);
                }
            }
        }

        if (ioConnected) {
            // ---- data transfers finishing: send their completions ----
            ND2_RESULT r = {};
            while (io.poll(&r)) {
                didWork = true;
                uintptr_t ctx = (uintptr_t)r.RequestContext;
                if (ctx >= CTX_DATA_BASE && ctx < CTX_DATA_BASE + kMaxInFlight) {
                    int j = (int)(ctx - CTX_DATA_BASE);
                    uint8_t sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
                    if (!ndOk(r.Status)) {
                        char b[64];
                        printf("  [io] slot %d data transfer %s\n", j, ndStr(r.Status, b, sizeof(b)));
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                    } else if (slots[j].isWrite) st.bytesWritten += slots[j].bytes;
                    else st.bytesRead += slots[j].bytes;
                    buildCompletion(dev.region(kRespOutOff + j * 16), slots[j].cid, 0, 0, sct, sc);
                    if (!io.send(dev.region(kRespOutOff + j * 16), 16, (void*)(CTX_RESP_BASE + j))) {
                        printf("  [io] failed to send a completion\n");
                        goto done;
                    }
                    slots[j].used = false;
                } else if (ctx >= CTX_RESP_BASE && ctx < CTX_RESP_BASE + kMaxInFlight) {
                    // completion sent; nothing to do
                } else if (ctx >= CTX_CAP_BASE && ctx < CTX_CAP_BASE + kMaxInFlight) {
                    int i = (int)(ctx - CTX_CAP_BASE);
                    capPosted[i] = false;
                    if (!ndOk(r.Status) || r.BytesTransferred != 64) {
                        printf("  [io] capsule receive slot %d: st=0x%08X bytes=%u\n",
                               i, (unsigned)r.Status, r.BytesTransferred);
                        continue;
                    }
                    const uint8_t* cap = dev.region(kCapInOff + i * 64);
                    uint8_t opcode = cap[0];
                    uint16_t cid = nvmeof_rd16(cap + 2);
                    uint64_t slba = nvmeof_rd64(cap + 40);
                    uint32_t blocks = (uint32_t)(nvmeof_rd32(cap + 48) & 0xFFFF) + 1;
                    uint32_t bytes = blocks * kBlockSize;
                    Sgl sgl = parseSgl(cap);
                    uint8_t sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;

                    int j = -1;
                    for (int k = 0; k < kMaxInFlight; k++) if (!slots[k].used) { j = k; break; }

                    // A fabrics Connect arrives on the I/O queue too: it is how
                    // the host announces which queue pair this connection is.
                    // Handling only Read/Write here rejects it as an invalid
                    // opcode, and the host's Connect then fails while the target
                    // reports nothing wrong - it rejected a command it should
                    // have known, using the I/O command set instead of fabrics.
                    if (opcode == NVMEOF_OPC_FABRICS) {
                        uint8_t fctype = cap[4];
                        uint16_t qid = nvmeof_rd16(cap + 42);
                        printf("  [io] fabrics Connect qid=%u fctype=0x%02X\n", qid, fctype);
                        uint8_t csct = NVMEOF_SCT_GENERIC, csc = NVMEOF_SC_SUCCESS;
                        if (fctype != NVMEOF_FCTYPE_CONNECT) {
                            csct = NVMEOF_SCT_GENERIC; csc = NVMEOF_SC_INVALID_OPCODE;
                        } else if (qid != 1) {
                            csct = NVMEOF_SCT_COMMAND_SPECIFIC; csc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                        }
                        // No data transfer: a fabrics Connect's payload is
                        // pulled by the target only for the admin queue, and
                        // this one carries none that this implementation reads.
                        buildCompletion(dev.region(kRespOutOff + 0), cid, 0, 0, csct, csc);
                        // Re-arm before answering (DESIGN 8.10(4) / 8.37).
                        capPosted[i] = io.postReceive(dev.region(kCapInOff + i * 64), 64,
                                                      (void*)(CTX_CAP_BASE + i));
                        io.send(dev.region(kRespOutOff + 0), 16, (void*)(CTX_RESP_BASE + 100));
                        continue;
                    }

                    if (j < 0) {
                        printf("  [io] no free data slot; the host exceeded %d in flight\n", kMaxInFlight);
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_CMDID_CONFLICT;
                        buildCompletion(dev.region(kRespOutOff + 0), cid, 0, 0, sct, sc);
                        // Re-arm before answering (DESIGN 8.10(4) / 8.37).
                        capPosted[i] = io.postReceive(dev.region(kCapInOff + i * 64), 64,
                                                      (void*)(CTX_CAP_BASE + i));
                        io.send(dev.region(kRespOutOff + 0), 16, (void*)(CTX_RESP_BASE + 100));
                        continue;
                    }
                    st.ioCommands++;
                    if (opcode != NVMEOF_OPC_WRITE && opcode != NVMEOF_OPC_READ) {
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE;
                        buildCompletion(dev.region(kRespOutOff + j * 16), cid, 0, 0, sct, sc);
                        io.send(dev.region(kRespOutOff + j * 16), 16, (void*)(CTX_RESP_BASE + j));
                    } else if (slba + blocks > gNsBlocks) {
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_LBA_RANGE;
                        buildCompletion(dev.region(kRespOutOff + j * 16), cid, 0, 0, sct, sc);
                        io.send(dev.region(kRespOutOff + j * 16), 16, (void*)(CTX_RESP_BASE + j));
                    } else if (!sgl.present || sgl.len < bytes) {
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                        buildCompletion(dev.region(kRespOutOff + j * 16), cid, 0, 0, sct, sc);
                        io.send(dev.region(kRespOutOff + j * 16), 16, (void*)(CTX_RESP_BASE + j));
                    } else {
                        uint8_t* ns = dev.region(gNsOff + (size_t)slba * kBlockSize);
                        slots[j].used = true;
                        slots[j].cid = cid;
                        slots[j].bytes = bytes;
                        slots[j].slba = slba;
                        slots[j].isWrite = (opcode == NVMEOF_OPC_WRITE);
                        bool posted = slots[j].isWrite
                            ? io.read(ns, bytes, sgl.addr, sgl.key, (void*)(CTX_DATA_BASE + j))
                            : io.write(ns, bytes, sgl.addr, sgl.key, (void*)(CTX_DATA_BASE + j));
                        // Re-arm this capsule slot immediately - before anything
                        // is answered.  Re-arming only after the data transfer
                        // completes would cap concurrency at one, and answering
                        // before re-arming leaves a window in which the peer's
                        // next command has nowhere to land.
                        capPosted[i] = io.postReceive(dev.region(kCapInOff + i * 64), 64,
                                                      (void*)(CTX_CAP_BASE + i));
                        if (!posted) {
                            printf("  [io] slot %d: could not post the data transfer\n", j);
                            slots[j].used = false;
                            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                            buildCompletion(dev.region(kRespOutOff + j * 16), cid, 0, 0, sct, sc);
                            io.send(dev.region(kRespOutOff + j * 16), 16, (void*)(CTX_RESP_BASE + j));
                        }
                    }              // end: this slot's command is on its way
                }                  // end: a capsule arrived in slot i
            }                      // end: while (io.poll(&r))
        }                          // end: if (ioConnected)

        if (didWork) { lastActivity = GetTickCount64(); continue; }
        if (GetTickCount64() - lastActivity > 10000) { printf("  idle for 10 s; stopping\n"); break; }
        SwitchToThread();
    }
done:
    printf("  target: ioCommands=%u written=%llu read=%llu\n",
           st.ioCommands, (unsigned long long)st.bytesWritten, (unsigned long long)st.bytesRead);
    admin.destroy();
    if (ioConnected) io.destroy();
    listener->Release();
    dev.close(nullptr, nullptr, 0);
    printf("\ntarget failures: %d\n", g_failures);
    return g_failures;
}

// ===========================================================================
//  INITIATOR
// ===========================================================================
static void fabricHeader(uint8_t* cap, uint8_t opcode, uint16_t cid, uint8_t fctype) {
    memset(cap, 0, 64);
    cap[0] = opcode;
    nvmeof_wr16(cap + 2, cid);
    cap[4] = fctype;
}

// One admin command, serial.  Returns false only on transport failure; the
// completion status is handed back so the caller can assert on it.
// One command, serial, on any queue.  Used for admin commands and for the I/O
// queue's own fabrics Connect.  The response MUST be read back from the same
// buffer the receive was posted into: an earlier revision posted into one offset
// and parsed another, so a successful Connect was reported as a failure and the
// "status" it printed came from unrelated bytes.
static bool submitCommand(Queue& q, Device& d, uint8_t* cap, Completion* out) {
    const bool trace = false;   // flip on when a serial command needs localising
    memcpy(d.region(kCapInOff), cap, 64);
    memset(d.region(kRespInOff), 0xFF, 16);
    if (trace) printf("    [t] q%u capsule opcode=0x%02X fctype=0x%02X cid=%u\n",
                      q.qid, cap[0], cap[4], nvmeof_rd16(cap + 2));
    if (!q.postReceive(d.region(kRespInOff), 16, (void*)(CTX_RESP_BASE + 100))) {
        if (trace) printf("    [t] postReceive FAILED\n");
        return false;
    }
    if (!q.send(d.region(kCapInOff), 64, (void*)(CTX_CAP_BASE + 100))) {
        if (trace) printf("    [t] send FAILED\n");
        return false;
    }
    ND2_RESULT r = {};
    HRESULT st = q.reap((void*)(CTX_CAP_BASE + 100), &r, kWaitMs);
    if (trace) { char b[64]; printf("    [t] send completion %s bytes=%u\n",
                                    ndStr(st, b, sizeof(b)), r.BytesTransferred); }
    if (!ndOk(st)) return false;
    st = q.reap((void*)(CTX_RESP_BASE + 100), &r, kWaitMs);
    if (trace) { char b[64]; printf("    [t] resp completion %s bytes=%u\n",
                                    ndStr(st, b, sizeof(b)), r.BytesTransferred); }
    if (!ndOk(st)) return false;
    const uint8_t* cqe = d.region(kRespInOff);
    out->cid = nvmeof_rd16(cqe + 12);
    out->status = nvmeof_rd16(cqe + 14);
    out->result = nvmeof_rd64(cqe + 0);
    return true;
}

static int runInitiator(const char* serverIp, uint16_t port, const char* localIp) {
    printf("=== F4 INITIATOR %s -> %s:%u\n", localIp, serverIp, port);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, localIp, &local.sin_addr);
    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    InetPtonA(AF_INET, serverIp, &remote.sin_addr);

    Device dev;
    if (!dev.open(local, gRegionBytes)) return 1;
    Queue admin, io;
    if (!admin.create(&dev, 0)) return 1;
    if (!io.create(&dev, 1)) return 1;

    // ---- admin connection ----
    // The Connect carries RDMA-CM private data filled the way the Linux host
    // fills it.  This suite used to send none: legal-looking, and accepted by our
    // own target, but a real fabrics target refuses the connection outright
    // (nvmet: private_data_len == 0 -> NVME_RDMA_CM_INVALID_LEN).
    nvmeof_rdma_request_pd adminPd;
    nvmeof_rdma_fill_req(&adminPd, 0, (uint16_t)(kSqSize + 1), 0);
    HRESULT hr = admin.conn->Bind((const sockaddr*)&local, sizeof(local));
    if (hr == ND_PENDING) hr = admin.waitOverlapped(admin.conn, kWaitMs);
    hr = admin.conn->Connect(admin.qp, (const sockaddr*)&remote, sizeof(remote),
                             kReadLimit, kReadLimit, &adminPd, sizeof(adminPd), &admin.ov);
    if (hr == ND_PENDING) hr = admin.waitOverlapped(admin.conn, 20000);
    if (!ndOk(hr)) { char b[64]; printf("admin Connect %s\n", ndStr(hr, b, sizeof(b))); return 1; }
    hr = admin.conn->CompleteConnect(&admin.ov);
    if (hr == ND_PENDING) hr = admin.waitOverlapped(admin.conn, kWaitMs);
    if (!ndOk(hr)) { printf("admin CompleteConnect failed\n"); return 1; }

    uint8_t cap[64];
    Completion c = {};
    fabricHeader(cap, NVMEOF_OPC_FABRICS, 1, NVMEOF_FCTYPE_PROPERTY_SET);
    nvmeof_wr32(cap + 44, NVMEOF_PROP_CC);
    nvmeof_wr64(cap + 48, 1);
    submitCommand(admin, dev, cap, &c);

    fabricHeader(cap, NVMEOF_OPC_FABRICS, 2, NVMEOF_FCTYPE_PROPERTY_GET);
    nvmeof_wr32(cap + 44, NVMEOF_PROP_CSTS);
    submitCommand(admin, dev, cap, &c);

    uint16_t cntlid = 0;
    {
        uint8_t* cd = dev.region(kConnectOff);
        memset(cd, 0, NVMEOF_CONNECT_DATA_SIZE);
        for (int i = 0; i < 16; i++) cd[i] = (uint8_t)(0xC0 + i);
        nvmeof_wr16(cd + 16, NVMEOF_CNTLID_DYNAMIC);
        memcpy(cd + 256, kSubNqn, strlen(kSubNqn));
        memcpy(cd + 512, "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f4-0001", 46);
        fabricHeader(cap, NVMEOF_OPC_FABRICS, 3, NVMEOF_FCTYPE_CONNECT);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, dev.rkey);
        nvmeof_wr16(cap + 44, 31);
        nvmeof_wr32(cap + 48, 30000);
        bool ok = submitCommand(admin, dev, cap, &c) && statusOk(c.status);
        cntlid = (uint16_t)c.result;
        Report("admin bring-up (enable, RDY, Connect)", ok && cntlid != 0, nullptr);
    }

    // ---- Set Features: Number of Queues ----
    // This is what asks the controller for an I/O queue.  There is no
    // Create I/O SQ or Create I/O CQ in NVMe-oF: Linux's nvmet answers both with
    // Invalid Opcode for every fabrics transport, and an I/O queue comes into
    // existence from the fabrics Connect below plus the queue pair it rides on.
    {
        fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, 4, 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_NUM_QUEUES);
        nvmeof_wr32(cap + 44, 0);            // one I/O queue: 0 in each half
        bool sent = submitCommand(admin, dev, cap, &c);
        char d[96];
        sprintf_s(d, "controller granted %u I/O queues", (unsigned)(c.result & 0xffff) + 1);
        Report("Set Features: Number of Queues", sent && statusOk(c.status), d);
    }
    {
        fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, 5, 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_KATO);
        nvmeof_wr32(cap + 44, NVMEOF_KATO_DEFAULT);
        submitCommand(admin, dev, cap, &c);
    }

    // ---- I/O queue pair (its own QP, as NVMe-oF/RDMA requires) ----
    {
        char b[64];
        hr = io.conn->Bind((const sockaddr*)&local, sizeof(local));
        if (hr == ND_PENDING) hr = io.waitOverlapped(io.conn, kWaitMs);
        printf("  [io] Bind            -> %s\n", ndStr(hr, b, sizeof(b)));
        if (!ndOk(hr)) { printf("I/O connector Bind failed\n"); return 1; }
        nvmeof_rdma_request_pd ioPd;
        nvmeof_rdma_fill_req(&ioPd, 1, (uint16_t)(kSqSize + 1), cntlid);
        hr = io.conn->Connect(io.qp, (const sockaddr*)&remote, sizeof(remote),
                              kReadLimit, kReadLimit, &ioPd, sizeof(ioPd), &io.ov);
        printf("  [io] Connect         -> %s\n", ndStr(hr, b, sizeof(b)));
        if (hr == ND_PENDING) hr = io.waitOverlapped(io.conn, 20000);
        printf("  [io] Connect wait    -> %s\n", ndStr(hr, b, sizeof(b)));
        if (ndOk(hr)) {
            hr = io.conn->CompleteConnect(&io.ov);
            if (hr == ND_PENDING) hr = io.waitOverlapped(io.conn, kWaitMs);
        }
        printf("  [io] CompleteConnect -> %s\n", ndStr(hr, b, sizeof(b)));
        if (!ndOk(hr)) { printf("I/O queue pair connect failed\n"); return 1; }
    }
    {
        uint8_t* cd = dev.region(kConnectOff);
        memset(cd, 0, 64);
        nvmeof_wr16(cd + 16, cntlid);
        memcpy(cd + 256, kSubNqn, strlen(kSubNqn));
        fabricHeader(cap, NVMEOF_OPC_FABRICS, 6, NVMEOF_FCTYPE_CONNECT);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, dev.rkey);
        nvmeof_wr16(cap + 42, 1);
        nvmeof_wr16(cap + 44, 31);
        nvmeof_wr32(cap + 48, 30000);
        Completion ioc = {};
        bool ok = submitCommand(io, dev, cap, &ioc);
        Report("Connect I/O queue qid=1", ok && statusOk(ioc.status), nullptr);
    }

    // ---- Identify Namespace: the geometry comes from the target ----
    uint32_t blockSize = kBlockSize;
    {
        memset(dev.region(kIdNsOff), 0xCC, NVMEOF_IDENTIFY_SIZE);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, 7, 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24),
                             (uint64_t)(uintptr_t)dev.region(kIdNsOff),
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        cap[40] = NVMEOF_ID_CNS_NS;
        bool ok = submitCommand(admin, dev, cap, &c) && statusOk(c.status);
        Report("Identify Namespace", ok, nullptr);
        if (ok) blockSize = 1u << dev.region(kIdNsOff)[NVMEOF_ID_NS_OFF_LBAF + 2];
    }

    // =======================================================================
    //  The pipelined load
    // =======================================================================
    printf("\n-- pipelined load: %d commands in flight, %d rounds, %zu KiB each\n",
           kMaxInFlight, gRounds, gXferBytes / 1024);

    const uint32_t blocksPerXfer = (uint32_t)(gXferBytes / blockSize);
    // Command ids are unique across BOTH phases, not just within one.  Numbering
    // each phase 1..512 made the duplicate-id check report 512 duplicates on a
    // run where nothing was wrong, because the read phase legitimately reused
    // the ids the write phase had already consumed.
    const uint16_t kCidPhaseBase = (uint16_t)(gRounds * kMaxInFlight);
    uint32_t dupCids = 0, missingCids = 0, badStatus = 0, wrongCid = 0;
    // Sized for the largest round count layoutInit accepts, because the id
    // space is a run-time quantity now and MSVC will not take a variable-length
    // array.  2 * 8192 * 8 bits = 16 KiB of stack, which is the honest cost of
    // keeping the exactly-once check exact at every size.
    uint64_t cidsSeen[(2 * 8192 * kMaxInFlight + 63) / 64] = {};

    auto runPhase = [&](bool isWrite, const char* what) -> double {
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        printf("  [phase] %s on the I/O queue: %d commands in flight\n", what, kMaxInFlight);
        for (int round = 0; round < gRounds; round++) {
            // ---- post every response receive and send every capsule first:
            //      that is what makes this pipelined rather than a loop of
            //      request/response pairs.
            for (int i = 0; i < kMaxInFlight; i++) {
                if (!io.postReceive(dev.region(kRespInOff + i * 16), 16,
                                    (void*)(CTX_RESP_BASE + i))) {
                    printf("    postReceive slot %d failed\n", i); return 0.0;
                }
            }
            for (int i = 0; i < kMaxInFlight; i++) {
                const int    cmd  = round * kMaxInFlight + i;
                uint32_t slba = (uint32_t)((uint64_t)cmd * blocksPerXfer);
                // One buffer per command, sweeping the whole transfer area, so the
                // source of a write and the destination of a read are touched
                // equally often and the two directions can be compared honestly.
                uint8_t* buf = dev.region(kXferOff + (size_t)cmd * gXferBytes);
                if (isWrite) {
                    // Pattern keyed on (round, i) so a slot reused across rounds
                    // cannot pass by holding the previous round's bytes.
                    uint8_t tpl[256];
                    buildPattern(tpl, round, i);
                    for (size_t off = 0; off < gXferBytes; off += 256) {
                        memcpy(buf + off, tpl, 256);
                    }
                } else {
                    memset(buf, 0xA5, gXferBytes);
                }
                uint8_t* cc = dev.region(kCapInOff + i * 64);
                fabricHeader(cc, (uint8_t)(isWrite ? NVMEOF_OPC_WRITE : NVMEOF_OPC_READ),
                             (uint16_t)((isWrite ? 0 : kCidPhaseBase) +
                                        round * kMaxInFlight + i + 1), 0);
                nvmeof_wr32(cc + 4, 1);
                nvmeof_wr64(cc + 40, slba);
                nvmeof_wr32(cc + 48, blocksPerXfer - 1);
                nvmeof_sgl_set_keyed((nvmeof_sgl*)(cc + 24), (uint64_t)(uintptr_t)buf,
                                     (uint32_t)gXferBytes, dev.rkey);
                if (!io.send(cc, 64, (void*)(CTX_CAP_BASE + i))) {
                    printf("    send slot %d failed\n", i); return 0.0;
                }
            }
            // ---- drain sends and responses together ----
            // Queue::reap(expect, ...) is wrong here and cost a debugging round:
            // it SILENTLY DISCARDS a completion whose context does not match the
            // one being waited for.  In a pipelined phase the response for slot
            // i+1 routinely lands while slot i is still being reaped, so the
            // matching reap throws away exactly the completion the next
            // iteration is about to wait for - and then waits 5 s to time out on
            // a response that had already arrived.  Poll once and dispatch on
            // the context instead, so nothing is ever dropped.
            int sendsDone = 0, respDone = 0;
            ULONGLONG drainStart = GetTickCount64();
            while (respDone < kMaxInFlight) {
                ND2_RESULT r = {};
                if (io.poll(&r)) {
                    uintptr_t ctx = (uintptr_t)r.RequestContext;
                    if (ctx >= CTX_CAP_BASE && ctx < CTX_CAP_BASE + kMaxInFlight) {
                        if (!ndOk(r.Status)) {
                            char b[64];
                            printf("    capsule send slot %d: %s\n",
                                   (int)(ctx - CTX_CAP_BASE), ndStr(r.Status, b, sizeof(b)));
                            return 0.0;
                        }
                        sendsDone++;
                    } else if (ctx >= CTX_RESP_BASE && ctx < CTX_RESP_BASE + kMaxInFlight) {
                        int s = (int)(ctx - CTX_RESP_BASE);
                        if (!ndOk(r.Status) || r.BytesTransferred != 16) {
                            char b[64];
                            printf("    response slot %d: %s bytes=%u\n", s,
                                   ndStr(r.Status, b, sizeof(b)), r.BytesTransferred);
                            return 0.0;
                        }
                        const uint8_t* cqe = dev.region(kRespInOff + s * 16);
                        uint16_t cid = nvmeof_rd16(cqe + 12);
                        uint16_t st  = nvmeof_rd16(cqe + 14);
                        if (!statusOk(st)) {
                            if (badStatus < 5) {
                                printf("    cid %u failed: sct=%u sc=0x%02X\n", cid,
                                       nvmeof_status_sct(st), nvmeof_status_sc(st));
                            }
                            badStatus++;
                        }
                        uint16_t want = (uint16_t)((isWrite ? 0 : kCidPhaseBase) +
                                                   round * kMaxInFlight + s + 1);
                        if (cid != want) wrongCid++;
                        // The id space is gRounds*kMaxInFlight wide, not 64: a
                        // 64-bit mask only covered the first 64 ids, so every
                        // round after the eighth looked like a duplicate command
                        // id and the check failed on a correct run.
                        if (cid >= 1 && cid <= 2 * kCidPhaseBase) {
                            uint32_t b = (uint32_t)(cid - 1);
                            uint64_t bit = 1ull << (b & 63);
                            if (cidsSeen[b >> 6] & bit) dupCids++;
                            cidsSeen[b >> 6] |= bit;
                        }
                        respDone++;
                    } else {
                        printf("    unexpected completion ctx=%p\n", r.RequestContext);
                    }
                    continue;
                }
                if (GetTickCount64() - drainStart >= kWaitMs) {
                    printf("    timed out: %d/%d responses, %d/%d sends completed\n",
                           respDone, kMaxInFlight, sendsDone, kMaxInFlight);
                    return 0.0;
                }
                SwitchToThread();
            }
            // The sends are already covered by RC ordering (the target cannot
            // answer a capsule it never received), but reap them explicitly so a
            // send-side fault cannot hide behind a response that did arrive.
            while (sendsDone < kMaxInFlight) {
                ND2_RESULT r = {};
                if (io.poll(&r)) {
                    uintptr_t ctx = (uintptr_t)r.RequestContext;
                    if (ctx >= CTX_CAP_BASE && ctx < CTX_CAP_BASE + kMaxInFlight) {
                        if (!ndOk(r.Status)) {
                            char b[64];
                            printf("    capsule send slot %d: %s\n",
                                   (int)(ctx - CTX_CAP_BASE), ndStr(r.Status, b, sizeof(b)));
                            return 0.0;
                        }
                        sendsDone++;
                    } else {
                        printf("    unexpected completion after responses ctx=%p\n", r.RequestContext);
                    }
                    continue;
                }
                if (GetTickCount64() - drainStart >= kWaitMs) {
                    printf("    send completion missing: %d/%d\n", sendsDone, kMaxInFlight);
                    return 0.0;
                }
                SwitchToThread();
            }
            // ---- read phase: verify the bytes actually came back ----
            if (!isWrite) {
                for (int i = 0; i < kMaxInFlight; i++) {
                    const int cmd = round * kMaxInFlight + i;
                    uint8_t* buf = dev.region(kXferOff + (size_t)cmd * gXferBytes);
                    uint8_t tpl[256];
                    buildPattern(tpl, round, i);
                    size_t bad = patternMismatch(buf, gXferBytes, tpl);
                    if (bad != gXferBytes) {
                        // Print the first few so a systematic error (wrong LBA,
                        // stale slot, half a transfer) is distinguishable from
                        // genuine data corruption.
                        if (missingCids < 6) {
                            uint32_t slba = (uint32_t)((uint64_t)cmd * blocksPerXfer);
                            printf("    MISMATCH round=%d slot=%d slba=%u off=%zu "
                                   "got=0x%02X want=0x%02X\n",
                                   round, i, slba, bad, buf[bad], tpl[bad % 256]);
                        }
                        missingCids++;
                    }
                }
            }
        }
        QueryPerformanceCounter(&t1);
        return (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    };

    // Writes first (which also populates the namespace), then reads of the same
    // regions so the read phase is verified against what the write phase put
    // there - not against a pattern the read phase generated itself.
    double wSecs = runPhase(true,  "write");
    double rSecs = runPhase(false, "read");

    // ---- the admin queue pair has to still be there after the burst ----
    //
    // A real host keeps the controller alive between I/O bursts, and this suite
    // has a reason to check: the target's log shows its admin Receive coming back
    // ND_CANCELED from the moment the I/O phase starts, and re-arming it does not
    // bring it back.  If that really is a dead queue pair, the controller would
    // stop answering Keep Alive and every host would abort it after one KATO -
    // a failure that heavy pipelined I/O hides and a long-lived connection does
    // not.  Asking the question here turns "an odd completion in the target's
    // log" into a pass or a failure.
    {
        fabricHeader(cap, NVMEOF_OPC_KEEP_ALIVE, 900, 0);
        bool sent = submitCommand(admin, dev, cap, &c);
        char d[160];
        sprintf_s(d, "status=0x%04X%s", c.status,
                  sent ? "" : " (no answer: the admin queue pair is gone)");
        Report("admin queue still answers Keep Alive after the pipelined load",
               sent && statusOk(c.status), d);
    }

    const double perPhaseBytes = (double)gRounds * kMaxInFlight * gXferBytes;

    // Every correctness assertion below is gated on the phases having actually
    // run.  Without this an early bail leaves the counters at zero, and "no
    // duplicate ids, no failures, the whole load ran" reads as success - a
    // vacuous pass, the same failure mode as inspecting a poisoned payload
    // after the command that would have filled it already failed.  A test that
    // passes when nothing happened is worse than one that fails, because it
    // certifies a path nobody exercised.
    const bool phasesRan = (wSecs > 0.0) && (rSecs > 0.0);
    {
        char d[192];
        sprintf_s(d, "write %.2f MiB in %.3f s, read %.2f MiB in %.3f s",
                  perPhaseBytes / 1048576.0, wSecs, perPhaseBytes / 1048576.0, rSecs);
        Report("both pipelined phases ran to completion", phasesRan, d);
    }
    if (!phasesRan) {
        printf("  [skip] the correctness assertions below: nothing ran, so they\n"
               "         could only pass vacuously\n");
    } else {
        {
            char d[160];
            sprintf_s(d, "badStatus=%u", badStatus);
            Report("every command completed with success status", badStatus == 0, d);
        }
        {
            // Every command id sent must have been completed exactly once, across
            // both phases.  A duplicate means a completion was mis-attributed; a
            // gap means one went missing.
            const uint32_t expectedCmds = 2u * gRounds * kMaxInFlight;
            uint32_t completed = 0;
            for (uint32_t b = 0; b < expectedCmds; b++) {
                if (cidsSeen[b >> 6] & (1ull << (b & 63))) completed++;
            }
            char d[192];
            sprintf_s(d, "duplicates=%u, mismatching read data=%u, wrong-cid=%u, cids seen=%u/%u",
                      dupCids, missingCids, wrongCid, completed, expectedCmds);
            Report("every command id completed exactly once, no corrupt payloads",
                   dupCids == 0 && missingCids == 0 && wrongCid == 0 &&
                   completed == expectedCmds, d);
        }
    }

    printf("\n-- throughput\n");
    printf("  write: %7.2f MiB/s  (%.3f s for %.1f MiB)\n",
           perPhaseBytes / 1048576.0 / wSecs, wSecs, perPhaseBytes / 1048576.0);
    printf("  read : %7.2f MiB/s  (%.3f s for %.1f MiB)\n",
           perPhaseBytes / 1048576.0 / rSecs, rSecs, perPhaseBytes / 1048576.0);
    printf("  bare RDMA Write on this card measured 2.53 GB/s = 2413 MiB/s\n");
    if (wSecs > 0) {
        printf("  => write is %.1f%% of the bare-RDMA ceiling\n",
               100.0 * (perPhaseBytes / wSecs) / 2.53e9);
    }
    if (rSecs > 0) {
        printf("  => read  is %.1f%% of the bare-RDMA ceiling\n",
               100.0 * (perPhaseBytes / rSecs) / 2.53e9);
    }

    admin.destroy();
    io.destroy();
    dev.close(nullptr, nullptr, 0);
    printf("\ninitiator failures: %d\n", g_failures);
    return g_failures;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage:\n"
               "  %s -target    <ip> <port> [-xfer <bytes>] [-rounds <n>]\n"
               "  %s -initiator <serverIp> <port> <localIp> [-xfer <bytes>] [-rounds <n>]\n"
               "\n"
               "  -xfer    payload bytes per command, multiple of 512 (default 32768)\n"
               "  -rounds  rounds of %d in-flight commands      (default 64)\n"
               "\n"
               "  Both endpoints must be given the same -xfer and -rounds: the\n"
               "  namespace layout is derived from them.\n",
               argv[0], argv[0], kMaxInFlight);
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Parse the workload options before anything touches the adapter, and size
    // the layout from them.  A layout error here is a usage error, not a
    // protocol failure.
    size_t xferBytes = 32u * 1024;
    int    rounds    = 64;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-xfer") == 0)        xferBytes = (size_t)_strtoui64(argv[i + 1], nullptr, 0);
        else if (strcmp(argv[i], "-rounds") == 0) rounds    = atoi(argv[i + 1]);
    }
    if (!layoutInit(xferBytes, rounds)) { WSACleanup(); return 2; }

    HRESULT hr = NdStartup();
    if (FAILED(hr)) { printf("NdStartup 0x%08X\n", (unsigned)hr); WSACleanup(); return 1; }

    printf("workload: %zu KiB per command x %d in flight x %d rounds = %.1f MiB per phase\n",
           xferBytes / 1024, kMaxInFlight, gRounds,
           (double)gRounds * kMaxInFlight * xferBytes / 1048576.0);
    printf("layout  : transfer %.1f MiB + namespace %u blocks (%.1f MiB), region %.1f MiB\n",
           gXferAreaBytes / 1048576.0, gNsBlocks, gNsBytes / 1048576.0,
           gRegionBytes / 1048576.0);

    int rc = 2;
    if (strcmp(argv[1], "-target") == 0) rc = runTarget(argv[2], (uint16_t)atoi(argv[3]));
    else if (strcmp(argv[1], "-initiator") == 0 && argc >= 5)
        rc = runInitiator(argv[2], (uint16_t)atoi(argv[3]), argv[4]);
    else printf("unknown mode %s\n", argv[1]);

    NdCleanup();
    WSACleanup();
    return rc;
}
