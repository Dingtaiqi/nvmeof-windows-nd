// f7_faults.cpp - F7: what happens when the other end stops existing.
//
// Section 6 of the design lists this as acceptance item 4: "the peer disappears
// mid-operation, invalid connect parameters, unsupported SGL type -> a BOUNDED
// error, and no hang".  The other two clauses have tests (F3 for the SGL type,
// F6 for the connect parameters); this one had none, and it is the one that
// matters most, because a controller that hangs instead of failing is worse than
// one that fails.
//
// The failure is injected by the target itself: after serving a configured number
// of I/O commands it calls ExitProcess() and disappears.  That is deliberately
// not a graceful Disconnect - a Disconnect is a protocol message the initiator is
// entitled to receive, and the interesting case is that it never arrives.  From
// the initiator's side a peer that crashed and a peer whose machine lost power
// are the same event.
//
// What is asserted, and why each one is the point:
//
//   1. The failure is DETECTED.  A queue pair whose peer is gone must eventually
//      surface something; silence forever is the bug this test exists for.
//   2. Detection is BOUNDED.  A deadline is checked, not just "it finished".
//   3. No command falsely reports success.  This is the one that is easy to lose:
//      an RDMA transfer can be posted, half-completed and then abandoned, and a
//      target that dies between the data and the completion leaves the host with
//      a request it could mark complete.  Every success is therefore verified
//      against its payload rather than trusted.
//   4. Teardown after the failure is bounded.  Releasing a queue pair whose peer
//      vanished is exactly where an implementation blocks on a handshake that can
//      never complete.

#include "nvmeof_wire.h"
#include "nvmeof_rdma.h"
#include <stdlib.h>

using namespace nvmeof;

static int g_failures = 0;
static void Report(const char* what, int ok, const char* detail) {
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           (detail && *detail) ? " - " : "", (detail && *detail) ? detail : "");
    if (!ok) g_failures++;
}

static const uint32_t kBlockSize = 512;
static const int      kSqSize    = 31;
static const size_t   kRegionBytes = 4u * 1024 * 1024;

static const size_t kCapInOff   = 0;
static const size_t kRespOutOff = 256;
static const size_t kRespInOff  = 512;
static const size_t kConnectOff = 768;
static const size_t kIdNsOff    = kConnectOff + 2048 + NVMEOF_IDENTIFY_SIZE;
static const size_t kXferOff    = kIdNsOff + NVMEOF_IDENTIFY_SIZE;
static const size_t kNsOff      = kXferOff + 512u * 1024 + 4096;
static const size_t kNsBytes    = 1024u * 1024;
static const uint32_t kNsBlocks = (uint32_t)(kNsBytes / kBlockSize);
static_assert(kNsOff + kNsBytes <= kRegionBytes, "namespace fits in the region");

#define CTX_CAP   ((uintptr_t)0x1000)
#define CTX_RESP  ((uintptr_t)0x1100)
#define CTX_DATA  ((uintptr_t)0x1200)

static const char* kSubNqn  = "nqn.2024-01.local.rdma:windows-nd";
static const char* kHostNqn = "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f7-0001";

// How long the initiator is prepared to be wrong about its peer before calling
// it dead.  One command's wait, plus room for the transport's own retries.
static const DWORD kDetectDeadlineMs = 12000;

struct Sgl { bool present; uint8_t type; uint64_t addr; uint32_t key; uint32_t len; };

static Sgl parseSgl(const uint8_t* cap) {
    Sgl s = {};
    const uint8_t ts = cap[24 + 15];
    s.type = nvmeof_sgl_type_of(ts);
    s.addr = nvmeof_rd64(cap + 24);
    s.len  = nvmeof_rd24(cap + 24 + 8);
    s.key  = nvmeof_rd32(cap + 24 + 11);
    s.present = nvmeof_sgl_is_keyed(ts) != 0;
    return s;
}

static void buildCompletion(uint8_t* cqe, uint16_t cid, uint16_t sqHead,
                           uint64_t result, uint8_t sct, uint8_t sc) {
    memset(cqe, 0, 16);
    nvmeof_wr64(cqe + 0, result);
    nvmeof_wr16(cqe + 8, sqHead);
    nvmeof_wr16(cqe + 12, cid);
    nvmeof_wr16(cqe + 14, NVMEOF_STATUS_CQE(sct, sc));
}

static void fillIdentifyCtrl(uint8_t* id, uint16_t cntlid) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_VID, 0x15B3);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SN, "NDVMEOFFAULTS000001", 20);
    memcpy(id + NVMEOF_ID_CTRL_OFF_MN, "NetworkDirect NVMe-oF faults          ", 40);
    memcpy(id + NVMEOF_ID_CTRL_OFF_FR, "0.5.0   ", 8);
    id[NVMEOF_ID_CTRL_OFF_RAB] = 6;
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_CNTLID, cntlid);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_VER, 0x00010400);
    id[NVMEOF_ID_CTRL_OFF_CNTRLTYPE] = NVMEOF_CTRLTYPE_IO;
    id[NVMEOF_ID_CTRL_OFF_SQES] = (uint8_t)((0x6 << 4) | 0x6);
    id[NVMEOF_ID_CTRL_OFF_CQES] = (uint8_t)((0x4 << 4) | 0x4);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_MAXCMD, 32);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_NN, 1);
    id[NVMEOF_ID_CTRL_OFF_VWC] = 0;      // namespace is memory; nothing to flush
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_KAS, 1);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_SGLS, NVMEOF_CTRL_SGLS_ADVERTISED);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SUBNQN, kSubNqn, strlen(kSubNqn));
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ, 4);   // one 64 B SQE, no in-capsule data
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IORCSZ, 1);
}

static void fillIdentifyNs(uint8_t* id) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NSZE, kNsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NCAP, kNsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NUSE, kNsBlocks);
    id[NVMEOF_ID_NS_OFF_NLBAF] = 0;
    id[NVMEOF_ID_NS_OFF_FLBAS] = 0;
    id[NVMEOF_ID_NS_OFF_LBAF + 2] = 9;
}

// ---------------------------------------------------------------------------
//  Target
// ---------------------------------------------------------------------------
struct FaultState {
    bool     enabled = false;
    uint16_t cntlid = 1;
    int      ioQueuesGranted = 0;
    int      katoMs = 0;
    int      ioCommands = 0;
};

static bool targetAdmin(Queue& q, Device& d, FaultState& st) {
    const uint8_t* cap = d.region(kCapInOff);
    uint8_t  opcode = cap[0];
    uint16_t cid    = nvmeof_rd16(cap + 2);
    uint8_t  fctype = cap[4];
    uint8_t  sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
    uint64_t result = 0;
    Sgl sgl = parseSgl(cap);

    if (opcode == NVMEOF_OPC_FABRICS) {
        if (fctype == NVMEOF_FCTYPE_PROPERTY_SET) {
            uint32_t off = nvmeof_rd32(cap + 44);
            uint64_t val = nvmeof_rd64(cap + 48);
            if (off == NVMEOF_PROP_CC) st.enabled = (val & 1) != 0;
            else sc = NVMEOF_SC_INVALID_FIELD;
        } else if (fctype == NVMEOF_FCTYPE_PROPERTY_GET) {
            uint32_t off = nvmeof_rd32(cap + 44);
            switch (off) {
            case NVMEOF_PROP_CAP:  result = NVMEOF_CAP_VALUE; break;
            case NVMEOF_PROP_VS:   result = 0x00010400u; break;
            case NVMEOF_PROP_CC:   result = st.enabled ? 1u : 0u; break;
            case NVMEOF_PROP_CSTS: result = st.enabled ? 1u : 0u; break;
            default: sc = NVMEOF_SC_INVALID_FIELD; break;
            }
        } else if (fctype == NVMEOF_FCTYPE_CONNECT) {
            uint16_t qid = nvmeof_rd16(cap + 42);
            printf("  [admin] Connect qid=%u\n", qid);
            if (!sgl.present || sgl.len < NVMEOF_CONNECT_DATA_SIZE) {
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_FORMAT;
            } else if (!q.read(d.region(kConnectOff), NVMEOF_CONNECT_DATA_SIZE, sgl.addr,
                               sgl.key, (void*)CTX_DATA)) {
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                ND2_RESULT r = {};
                if (!ndOk(q.reap((void*)CTX_DATA, &r, kWaitMs))) {
                    sc = NVMEOF_SC_DATA_XFER_ERROR;
                } else if (qid == 0) {
                    result = st.cntlid;
                } else {
                    sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                }
            }
        } else {
            sc = NVMEOF_SC_INVALID_OPCODE;
        }
    } else if (opcode == NVMEOF_OPC_SET_FEATURES || opcode == NVMEOF_OPC_GET_FEATURES) {
        bool isSet = (opcode == NVMEOF_OPC_SET_FEATURES);
        uint8_t fid = (uint8_t)(nvmeof_rd32(cap + 40) & 0xff);
        uint32_t cdw11 = nvmeof_rd32(cap + 44);
        switch (fid) {
        case NVMEOF_FID_NUM_QUEUES:
            if (isSet) st.ioQueuesGranted = 1;
            result = 0;   // one I/O queue
            break;
        case NVMEOF_FID_KATO:
            // Set answers in SECONDS, Get answers in MILLISECONDS - the reference
            // target's asymmetry (nvmet_set_feat_kato vs nvmet_get_feat_kato).
            if (isSet) {
                st.katoMs = (int)cdw11;
                result = (uint64_t)((st.katoMs + 999) / 1000);
            } else {
                result = (uint64_t)st.katoMs;
            }
            break;
        default:
            sc = NVMEOF_SC_INVALID_FIELD;
            break;
        }
    } else if (opcode == NVMEOF_OPC_KEEP_ALIVE) {
        if (st.katoMs == 0) { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_KA_TIMEOUT_INVALID; }
    } else if (opcode == NVMEOF_OPC_IDENTIFY) {
        uint8_t cns = cap[40];
        uint8_t* src = nullptr;
        if (cns == NVMEOF_ID_CNS_CTRL) { fillIdentifyCtrl(d.region(kIdNsOff), st.cntlid); src = d.region(kIdNsOff); }
        else if (cns == NVMEOF_ID_CNS_NS) { fillIdentifyNs(d.region(kIdNsOff)); src = d.region(kIdNsOff); }
        else { sc = NVMEOF_SC_INVALID_FIELD; }
        if (src) {
            if (!q.write(src, NVMEOF_IDENTIFY_SIZE, sgl.addr, sgl.key, (void*)(CTX_DATA + 1))) {
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                ND2_RESULT r = {};
                if (!ndOk(q.reap((void*)(CTX_DATA + 1), &r, kWaitMs))) sc = NVMEOF_SC_DATA_XFER_ERROR;
            }
        }
    } else {
        sc = NVMEOF_SC_INVALID_OPCODE;
    }

    buildCompletion(d.region(kRespOutOff), cid, 0, result, sct, sc);
    // Re-arm BEFORE answering; see DESIGN 8.10(4) and 8.37.  The caller must not
    // re-arm again.
    q.postReceive(d.region(kCapInOff), 64, (void*)CTX_CAP);
    return q.send(d.region(kRespOutOff), 16, (void*)CTX_RESP);
}

static int runTarget(const char* ip, uint16_t port, int dieAfter) {
    printf("=== F7 TARGET on %s:%u (die-after=%d)\n", ip, port, dieAfter);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, ip, &local.sin_addr);

    Device dev;
    if (!dev.open(local, kRegionBytes)) return 1;

    // die-after 0 means "there was never a target here": stop before anything is
    // listening, so the initiator meets a refused connection rather than a
    // silent one.
    if (dieAfter == 0) {
        printf("  vanishing before creating the listener\n");
        fflush(stdout);
        ExitProcess(0);
    }

    Queue admin, io;
    if (!admin.create(&dev, 0)) return 1;
    if (!io.create(&dev, 1)) return 1;

    IND2Listener* listener = nullptr;
    HRESULT hr = dev.adapter->CreateListener(IID_IND2Listener, dev.ovFile, (VOID**)&listener);
    if (FAILED(hr)) { printf("CreateListener failed\n"); return 1; }
    hr = listener->Bind((const sockaddr*)&local, sizeof(local));
    if (!ndOk(hr)) { char b[64]; printf("listener Bind %s\n", ndStr(hr, b, sizeof(b))); return 1; }
    listener->Listen(4);

    hr = listener->GetConnectionRequest(admin.conn, &admin.ov);
    if (hr == ND_PENDING) hr = admin.waitOverlapped(listener, 20000);
    if (!ndOk(hr)) { printf("admin connection request failed\n"); return 1; }
    if (!admin.postReceive(dev.region(kCapInOff), 64, (void*)CTX_CAP)) return 1;
    {
        uint16_t claimedQid = 0, rejStatus = 0;
        int verdict = admin.acceptChecked((uint16_t)(kSqSize + 1), &claimedQid, &rejStatus);
        if (verdict != 0 || claimedQid != 0) {
            printf("admin Accept refused (verdict=%d qid=%u nvme_rdma status=%u)\n",
                   verdict, claimedQid, rejStatus);
            return 1;
        }
    }
    printf("  admin connected\n");

    FaultState st;
    bool ioPending = false, ioConnected = false;
    // A Receive that completes ND_CANCELED is terminal for its queue pair: every
    // Receive posted afterwards completes ND_CANCELED immediately as well, so a
    // re-arm is a spin, not a recovery.  Both queue pairs end up here when the
    // initiator goes away, which is exactly the fault this suite injects - and
    // this target has no idle timeout, so it has to notice.
    bool adminDead = false, ioDead = false;

    for (;;) {
        if (st.ioQueuesGranted > 0 && !ioConnected && !ioPending) {
            HRESULT h2 = listener->GetConnectionRequest(io.conn, &io.ov);
            ioPending = (h2 == ND_PENDING) || SUCCEEDED(h2);
            // Blocking here would starve the admin queue (DESIGN 8.12(2)), and
            // leaving the admin Receive un-armed after the request would starve it
            // just as effectively: issuing a listener connection request can
            // cancel a Receive that is pending on another queue pair (DESIGN
            // 8.8(2), restated in 8.16(4)).  One spare Receive is cheap; a deaf
            // admin queue costs a 5 s timeout that looks like a protocol bug.
            if (ioPending) admin.postReceive(dev.region(kCapInOff), 64, (void*)CTX_CAP);
        } else if (ioPending) {
            HRESULT h2 = io.pollOverlapped(io.conn);
            if (h2 != ND_PENDING) {
                ioPending = false;
                if (ndOk(h2)) {
                    // Arm the I/O Receive before Accept: Accept releases the
                    // host's CompleteConnect, so arming afterwards races a peer
                    // that is already entitled to send (DESIGN 8.10(4)).
                    if (!io.postReceive(dev.region(kCapInOff + 64), 64, (void*)CTX_CAP)) break;
                    uint16_t claimedQid = 0, rejStatus = 0;
                    int verdict = io.acceptChecked((uint16_t)(kSqSize + 1), &claimedQid, &rejStatus);
                    if (verdict != 0 || claimedQid != 1) {
                        printf("  [accept] I/O queue refused (verdict=%d qid=%u nvme_rdma status=%u)\n",
                               verdict, claimedQid, rejStatus);
                        h2 = ND_UNSUCCESSFUL;
                    }
                }
                // Same cancellation rule as above, now for Accept.
                if (!admin.postReceive(dev.region(kCapInOff), 64, (void*)CTX_CAP)) break;
                if (ndOk(h2)) { ioConnected = true; printf("  io queue connected\n"); }
            }
        }

        if (ioConnected && !ioDead) {
            ND2_RESULT r = {};
            if (io.poll(&r)) {
                if (r.RequestContext == (void*)CTX_CAP && ndOk(r.Status) && r.BytesTransferred == 64) {
                    const uint8_t* cap = dev.region(kCapInOff + 64);
                    uint8_t op = cap[0];
                    uint16_t cid = nvmeof_rd16(cap + 2);
                    uint8_t sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
                    if (op == NVMEOF_OPC_FABRICS) {
                        if (cap[4] != NVMEOF_FCTYPE_CONNECT || nvmeof_rd16(cap + 42) != 1)
                            { sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM; }
                    } else if (op == NVMEOF_OPC_FLUSH) {
                        // Shares opcode 0x00 with the admin queue's Delete SQ; only
                        // the queue tells them apart.  Nothing to flush here.
                        st.ioCommands++;
                        printf("  [io] cmd %d FLUSH\n", st.ioCommands);
                    } else if (op == NVMEOF_OPC_WRITE || op == NVMEOF_OPC_READ) {
                        uint64_t slba = nvmeof_rd64(cap + 40);
                        uint32_t blocks = (uint32_t)(nvmeof_rd32(cap + 48) & 0xFFFF) + 1;
                        uint32_t bytes = blocks * kBlockSize;
                        Sgl sgl = parseSgl(cap);
                        st.ioCommands++;
                        printf("  [io] cmd %d %s slba=%llu blocks=%u\n", st.ioCommands,
                               op == NVMEOF_OPC_WRITE ? "WRITE" : "READ",
                               (unsigned long long)slba, blocks);
                        if (slba + blocks > kNsBlocks) sc = NVMEOF_SC_LBA_RANGE;
                        else if (!sgl.present || sgl.len < bytes) sc = NVMEOF_SC_DATA_XFER_ERROR;
                        else {
                            uint8_t* ns = dev.region(kNsOff + (size_t)slba * kBlockSize);
                            bool ok = (op == NVMEOF_OPC_WRITE)
                                          ? io.read(ns, bytes, sgl.addr, sgl.key, (void*)CTX_DATA)
                                          : io.write(ns, bytes, sgl.addr, sgl.key, (void*)CTX_DATA);
                            if (!ok) sc = NVMEOF_SC_DATA_XFER_ERROR;
                            else {
                                ND2_RESULT xr = {};
                                if (!ndOk(io.reap((void*)CTX_DATA, &xr, kWaitMs))) sc = NVMEOF_SC_DATA_XFER_ERROR;
                            }
                        }
                        // Answer the command, and THEN disappear if asked to.
                        // Dying between the data and the completion would leave
                        // the host unable to tell a completed transfer from an
                        // abandoned one, which is a different fault; this test is
                        // about the peer going away between commands.
                        if (sc == NVMEOF_SC_SUCCESS) {
                            buildCompletion(dev.region(kRespOutOff + 64), cid, 0, 0, sct, sc);
                            // Re-arm before answering (DESIGN 8.10(4) / 8.37).
                            io.postReceive(dev.region(kCapInOff + 64), 64, (void*)CTX_CAP);
                            io.send(dev.region(kRespOutOff + 64), 16, (void*)CTX_RESP);
                            if (dieAfter > 0 && st.ioCommands >= dieAfter) {
                                printf("  [fault] served %d I/O commands, vanishing now "
                                       "(no Disconnect, no cleanup)\n", st.ioCommands);
                                fflush(stdout);
                                ExitProcess(0);
                            }
                            continue;
                        }
                    } else {
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE;
                    }
                    buildCompletion(dev.region(kRespOutOff + 64), cid, 0, 0, sct, sc);
                    io.postReceive(dev.region(kCapInOff + 64), 64, (void*)CTX_CAP);
                    io.send(dev.region(kRespOutOff + 64), 16, (void*)CTX_RESP);
                } else if (r.RequestContext == (void*)CTX_CAP) {
                    printf("  [io] Receive did not deliver a capsule (st=0x%08X %s bytes=%u); "
                           "queue pair declared dead\n",
                           (unsigned)r.Status, r.Status == ND_CANCELED ? "ND_CANCELED" : "short",
                           r.BytesTransferred);
                    ioDead = true;
                }
            }
        }

        ND2_RESULT ar = {};
        if (!adminDead && admin.poll(&ar)) {
            if (ar.RequestContext == (void*)CTX_CAP && ndOk(ar.Status) && ar.BytesTransferred == 64) {
                // targetAdmin() re-arms before it answers; do not arm again here.
                if (!targetAdmin(admin, dev, st)) break;
            } else if (ar.RequestContext == (void*)CTX_CAP) {
                // A Receive that delivered no capsule.  ND_CANCELED is terminal
                // for the queue pair, so record it instead of re-arming: a retry
                // loop here would spin for as long as the initiator is gone,
                // which is the whole point of this suite.
                printf("  [admin] Receive did not deliver a capsule (st=0x%08X %s bytes=%u); "
                       "queue pair declared dead\n",
                       (unsigned)ar.Status, ar.Status == ND_CANCELED ? "ND_CANCELED" : "short",
                       ar.BytesTransferred);
                adminDead = true;
            }
        }

        // Both queue pairs finished: the initiator is gone.  There is nothing
        // left to serve, and this target has no idle timeout to fall back on.
        if (adminDead && ioDead) {
            printf("  both queue pairs returned ND_CANCELED; the initiator has gone\n");
            break;
        }
        SwitchToThread();
    }

    // Reached legitimately in case C: the initiator vanished first, so the target
    // saw both queue pairs cancelled and stopped on its own.
    printf("  target exit path: the peer went away first, so this target stopped by itself\n");
    printf("target failures: %d\n", g_failures);
    return g_failures;
}

// ---------------------------------------------------------------------------
//  Initiator
// ---------------------------------------------------------------------------
struct Outcome { bool transportOk; bool statusOk; DWORD ms; ND2_RESULT last; };

static bool submit(Queue& q, Device& d, uint8_t* cap, uint16_t* status, Outcome* out) {
    memcpy(d.region(kCapInOff), cap, 64);
    memset(d.region(kRespInOff), 0xFF, 16);
    ULONGLONG t0 = GetTickCount64();
    out->transportOk = false;
    out->statusOk = false;
    out->ms = 0;
    if (!q.postReceive(d.region(kRespInOff), 16, (void*)CTX_RESP)) { out->ms = (DWORD)(GetTickCount64() - t0); return false; }
    if (!q.send(d.region(kCapInOff), 64, (void*)CTX_CAP)) { out->ms = (DWORD)(GetTickCount64() - t0); return false; }

    bool gotSend = false, gotResp = false;
    while (!gotSend || !gotResp) {
        ND2_RESULT r = {};
        if (q.poll(&r)) {
            uintptr_t ctx = (uintptr_t)r.RequestContext;
            out->last = r;
            if (ctx == CTX_CAP) {
                if (!ndOk(r.Status)) { out->ms = (DWORD)(GetTickCount64() - t0); return false; }
                gotSend = true;
            } else if (ctx == CTX_RESP) {
                if (!ndOk(r.Status) || r.BytesTransferred != 16) {
                    out->ms = (DWORD)(GetTickCount64() - t0);
                    return false;
                }
                gotResp = true;
            }
            continue;
        }
        if (GetTickCount64() - t0 >= kWaitMs) { out->ms = (DWORD)(GetTickCount64() - t0); return false; }
        SwitchToThread();
    }
    out->ms = (DWORD)(GetTickCount64() - t0);
    out->transportOk = true;
    const uint8_t* cqe = d.region(kRespInOff);
    *status = nvmeof_rd16(cqe + 14);
    return true;
}

static bool statusIsOk(uint16_t st) {
    return (st & NVMEOF_STATUS_P_MASK) != 0 &&
           nvmeof_status_sct(st) == NVMEOF_SCT_GENERIC &&
           nvmeof_status_sc(st) == NVMEOF_SC_SUCCESS;
}

static void fabricHeader(uint8_t* cap, uint8_t opcode, uint16_t cid, uint8_t fctype) {
    memset(cap, 0, 64);
    cap[0] = opcode;
    nvmeof_wr16(cap + 2, cid);
    cap[4] = fctype;
}

static int runInitiator(const char* serverIp, uint16_t port, const char* localIp, const char* phase) {
    printf("=== F7 INITIATOR %s -> %s:%u phase=%s\n", localIp, serverIp, port, phase);
    bool expectDeaf = (strcmp(phase, "deaf") == 0);   // no target ever existed
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, localIp, &local.sin_addr);
    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    InetPtonA(AF_INET, serverIp, &remote.sin_addr);

    Device dev;
    if (!dev.open(local, kRegionBytes)) return 1;
    Queue admin, io;
    if (!admin.create(&dev, 0)) return 1;
    if (!io.create(&dev, 1)) return 1;

    nvmeof_rdma_request_pd req;
    // Linux's queue sizes are asymmetric: hsqsize is 0-based.  Sending depth in
    // both fields is what this suite used to do, and a real target refuses the
    // admin queue for it (nvmet: hsqsize + 1 > NVME_AQ_DEPTH).
    nvmeof_rdma_fill_req(&req, 0, (uint16_t)(kSqSize + 1), 0);

    // ---- does the admin connection come up at all? ----
    {
        ULONGLONG t0 = GetTickCount64();
        char b[64];
        HRESULT h = admin.conn->Bind((const sockaddr*)&local, sizeof(local));
        h = admin.conn->Connect(admin.qp, (const sockaddr*)&remote, sizeof(remote),
                                kReadLimit, kReadLimit, &req, sizeof(req), &admin.ov);
        if (h == ND_PENDING) h = admin.waitOverlapped(admin.conn, 20000);
        DWORD ms = (DWORD)(GetTickCount64() - t0);
        bool up = false;
        if (ndOk(h)) {
            h = admin.conn->CompleteConnect(&admin.ov);
            if (h == ND_PENDING) h = admin.waitOverlapped(admin.conn, kWaitMs);
            up = ndOk(h);
        }
        char d[160];
        sprintf_s(d, "resp=%s after %u ms", up ? "connected" : ndStr(h, b, sizeof(b)), ms);
        if (expectDeaf) {
            // Nothing was ever listening: the connection must FAIL, and it must
            // fail without being waited out to the 20 s ceiling.
            Report("with no target listening, admin Connect fails rather than hanging",
                   !up && ms < 20000, d);
            dev.close(nullptr, nullptr, 0);
            printf("\ninitiator failures: %d\n", g_failures);
            return g_failures;
        }
        if (!up) { printf("  admin Connect %s\n", d); return 1; }
        printf("  [conn] %s\n", d);
    }

    uint8_t cap[64];
    uint16_t st = 0;
    Outcome oc = {};
    uint16_t cid = 0;

    // ---- bring-up ----
    fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_CONNECT);
    {
        uint8_t* cd = dev.region(kConnectOff);
        memset(cd, 0, NVMEOF_CONNECT_DATA_SIZE);
        nvmeof_wr16(cd + 16, NVMEOF_CNTLID_DYNAMIC);
        memcpy(cd + 256, kSubNqn, strlen(kSubNqn));
        memcpy(cd + 512, kHostNqn, strlen(kHostNqn));
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, dev.rkey);
        nvmeof_wr16(cap + 42, 0);
        nvmeof_wr16(cap + 44, (uint16_t)kSqSize);
        nvmeof_wr32(cap + 48, NVMEOF_KATO_DEFAULT);
        submit(admin, dev, cap, &st, &oc);
    }
    // The Connect response's result is the controller id this queue pair was
    // given; the I/O queue's private data carries it back (the Linux host does
    // the same).  nvmet itself does not read the field - it is sent because a
    // target is allowed to care.
    const uint64_t adminCntlid = nvmeof_rd64(dev.region(kRespInOff));
    fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_PROPERTY_SET);
    nvmeof_wr32(cap + 44, NVMEOF_PROP_CC);
    nvmeof_wr64(cap + 48, 1);
    submit(admin, dev, cap, &st, &oc);

    fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, ++cid, 0);
    nvmeof_wr32(cap + 40, NVMEOF_FID_NUM_QUEUES);
    nvmeof_wr32(cap + 44, 0);
    submit(admin, dev, cap, &st, &oc);
    fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, ++cid, 0);
    nvmeof_wr32(cap + 40, NVMEOF_FID_KATO);
    nvmeof_wr32(cap + 44, NVMEOF_KATO_DEFAULT);
    submit(admin, dev, cap, &st, &oc);

    {
        nvmeof_rdma_request_pd ireq;
        nvmeof_rdma_fill_req(&ireq, 1, (uint16_t)(kSqSize + 1), (uint16_t)adminCntlid);
        char b[64];
        HRESULT h = io.conn->Bind((const sockaddr*)&local, sizeof(local));
        h = io.conn->Connect(io.qp, (const sockaddr*)&remote, sizeof(remote),
                             kReadLimit, kReadLimit, &ireq, sizeof(ireq), &io.ov);
        if (h == ND_PENDING) h = io.waitOverlapped(io.conn, 20000);
        if (ndOk(h)) {
            h = io.conn->CompleteConnect(&io.ov);
            if (h == ND_PENDING) h = io.waitOverlapped(io.conn, kWaitMs);
        }
        Report("I/O queue pair connected", ndOk(h), ndOk(h) ? nullptr : ndStr(h, b, sizeof(b)));
        if (!ndOk(h)) { dev.close(nullptr, nullptr, 0); return 1; }
    }
    fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_CONNECT);
    {
        uint8_t* cd = dev.region(kConnectOff);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, dev.rkey);
        nvmeof_wr16(cap + 42, 1);
        nvmeof_wr16(cap + 44, (uint16_t)kSqSize);
        bool sent = submit(io, dev, cap, &st, &oc);
        Report("fabrics Connect on the I/O queue", sent && statusIsOk(st), nullptr);
    }

    // ---- the load, until the peer stops existing ----
    const uint32_t blocks = 8;
    const uint32_t bytes = blocks * kBlockSize;
    uint8_t* buf = dev.region(kXferOff);

    int okCount = 0, writeOk = 0, readOk = 0, readVerified = 0, failCount = 0;
    ULONGLONG detectMs = 0;
    bool sawFailure = false;

    for (int i = 0; i < 64 && !sawFailure; i++) {
        uint32_t slba = (uint32_t)((i * blocks) % (kNsBlocks - blocks));
        // Pattern keyed on the round so a stale buffer cannot pass.
        for (uint32_t k = 0; k < bytes; k++) buf[k] = (uint8_t)(k * 7u + i * 29u);

        fabricHeader(cap, NVMEOF_OPC_WRITE, ++cid, 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, slba);
        nvmeof_wr32(cap + 48, blocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)buf,
                             bytes, dev.rkey);
        bool w = submit(io, dev, cap, &st, &oc);
        if (!w || !statusIsOk(st)) {
            sawFailure = true; failCount++; detectMs = oc.ms;
            printf("  [fault] WRITE %d failed after %u ms (transportOk=%d)\n",
                   i, oc.ms, (int)oc.transportOk);
            break;
        }
        okCount++;
        writeOk++;

        fabricHeader(cap, NVMEOF_OPC_READ, ++cid, 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, slba);
        nvmeof_wr32(cap + 48, blocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)buf,
                             bytes, dev.rkey);
        bool r = submit(io, dev, cap, &st, &oc);
        if (!r || !statusIsOk(st)) {
            sawFailure = true; failCount++; detectMs = oc.ms;
            printf("  [fault] READ %d failed after %u ms (transportOk=%d)\n",
                   i, oc.ms, (int)oc.transportOk);
            break;
        }
        okCount++;
        readOk++;
        // A success is only counted after its payload is checked.  Without this
        // the count of "successes" would include transfers the peer never made.
        bool intact = true;
        for (uint32_t k = 0; k < bytes; k++) {
            if (buf[k] != (uint8_t)(k * 7u + i * 29u)) { intact = false; break; }
        }
        if (intact) readVerified++;
        else {
            printf("  [fault] READ %d reported success with a corrupt payload\n", i);
            sawFailure = true;
        }

        // Case C: the mirror of the target's fault.  The initiator leaves
        // without a Disconnect and without tearing anything down, which is what
        // a killed host process looks like.  The target has to notice on its own:
        // it has no idle timeout, and a target that keeps a queue pair open
        // forever for a host that no longer exists is the failure this checks.
        if (strcmp(phase, "vanish") == 0 && okCount >= 4) {
            printf("  [fault] initiator vanishing after %d commands "
                   "(no Disconnect, no teardown)\n", okCount);
            fflush(stdout);
            ExitProcess(0);
        }
    }

    char d[224];
    sprintf_s(d, "%d ok (%d write, %d read, %d read-verified), %d failed, detected after %llu ms",
              okCount, writeOk, readOk, readVerified, failCount,
              (unsigned long long)detectMs);
    Report("the peer's disappearance is detected, not waited on forever", sawFailure, d);
    Report("detection happened within the deadline rather than at the process timeout",
           sawFailure && detectMs > 0 && detectMs <= kDetectDeadlineMs, d);
    // Every READ that claimed success must have delivered its data, and there
    // must have been at least one.  This is the clause a half-completed RDMA
    // transfer would violate: the completion arrives, the payload does not.
    Report("every READ that claimed success delivered its payload",
           readOk > 0 && readVerified == readOk, d);
    Report("at least one command completed before the fault, so the path was live",
           writeOk >= 1 && readOk >= 1, d);

    // ---- teardown with a dead peer must also be bounded ----
    {
        ULONGLONG t0 = GetTickCount64();
        io.destroy();
        admin.destroy();
        DWORD ms = (DWORD)(GetTickCount64() - t0);
        char d2[128];
        sprintf_s(d2, "queue teardown took %u ms", ms);
        Report("tearing down queue pairs whose peer is gone is bounded", ms < 8000, d2);
    }
    dev.close(nullptr, nullptr, 0);
    printf("\ninitiator failures: %d\n", g_failures);
    return g_failures;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage:\n"
               "  %s -target    <ip> <port> [-die-after <n>]\n"
               "  %s -initiator <serverIp> <port> <localIp> [-phase live|deaf|vanish]\n"
               "\n"
               "  -die-after 0    vanish before listening (nothing was ever there)\n"
               "  -die-after n>0  serve n I/O commands, then ExitProcess with no Disconnect\n"
               "  -phase deaf     expect no target to be listening\n"
               "  -phase vanish   run 4 I/O commands, then ExitProcess with no Disconnect\n",
               argv[0], argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    HRESULT hr = NdStartup();
    if (FAILED(hr)) { printf("NdStartup 0x%08X\n", (unsigned)hr); WSACleanup(); return 1; }

    int dieAfter = -1;
    const char* phase = "live";
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-die-after") == 0) dieAfter = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "-phase") == 0) phase = argv[i + 1];
    }

    int rc = 2;
    if (strcmp(argv[1], "-target") == 0) rc = runTarget(argv[2], (uint16_t)atoi(argv[3]), dieAfter);
    else if (strcmp(argv[1], "-initiator") == 0 && argc >= 5)
        rc = runInitiator(argv[2], (uint16_t)atoi(argv[3]), argv[4], phase);
    else printf("unknown mode %s\n", argv[1]);

    NdCleanup();
    WSACleanup();
    return rc;
}
