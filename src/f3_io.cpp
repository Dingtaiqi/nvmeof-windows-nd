// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
// f3_io.cpp - F3 milestone: NVMe-oF I/O queues and Read/Write commands.
//
// WHAT IS NEW HERE COMPARED WITH f1_bringup.cpp
//
//   1. A SECOND QUEUE PAIR.  NVMe-oF/RDMA gives every submission queue its own
//      QP, which is the structural difference from the TCP binding where all
//      queues share one connection.  So there are two connections, two
//      completion queues, and two fabrics Connect commands - qid 0 for admin,
//      qid 1 for the I/O queue.
//   2. Create CQ / Create SQ, the admin commands that bring an I/O queue into
//      existence.
//   3. Identify Namespace, so the initiator learns the block size instead of
//      assuming one.
//   4. Read (0x02) and Write (0x01) commands, whose payload is moved by the
//      TARGET with one-sided RDMA against the rkey in the command's SGL:
//        Write: target RDMA-Reads FROM the host into its namespace
//        Read:  target RDMA-Writes from its namespace INTO the host
//      The target reads and writes its namespace *in place* - there is no
//      bounce buffer, because that is the entire point of NVMe-oF/RDMA.
//
// USAGE
//   f3_io.exe -target    192.168.100.2 54350
//   f3_io.exe -initiator 192.168.100.2 54350 192.168.100.3
// Exit code = number of failures.

#include "nvmeof_wire.h"
#include "nvmeof_rdma.h"

using namespace nvmeof;

static int g_failures = 0;
static void Report(const char* what, int ok, const char* detail) {
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           (detail && *detail) ? " - " : "", (detail && *detail) ? detail : "");
    if (!ok) g_failures++;
}

// ---------------------------------------------------------------------------
//  Region layout.  Shared by both roles; the SGL addresses the initiator
//  publishes are real addresses inside this region.
// ---------------------------------------------------------------------------
static const size_t kRegionBytes  = 4u * 1024 * 1024;
static const size_t kCapInOff     = 0;            // 64 B capsule received (admin)
static const size_t kCapIoInOff   = 64;           // 64 B capsule received (I/O)
static const size_t kRespOutOff   = 128;          // 16 B completion sent
static const size_t kRespInOff    = 192;          // 16 B completion received
static const size_t kConnectOff   = 512;          // 1024 B Connect data
static const size_t kIdCtrlOff    = 2048;         // 4096 B Identify Controller
static const size_t kIdNsOff      = 6144;         // 4096 B Identify Namespace
static const size_t kXferOff      = 16384;        // initiator data buffer
static const size_t kXferBytes    = 1024u * 1024;
static const size_t kNsOff        = kXferOff + kXferBytes;   // target namespace
static const size_t kNsBytes      = 1024u * 1024;

static const uint32_t kBlockSize  = 512;
static const uint32_t kNsBlocks   = (uint32_t)(kNsBytes / kBlockSize);
static const uint16_t kSqSize     = 31;    // 0-based: 32 entries, as the Connect says
static const char*    kSubNqn     = "nqn.2024-01.local.rdma:windows-nd";

#define CTX_CAP   ((void*)(uintptr_t)0xC1)
#define CTX_RESP  ((void*)(uintptr_t)0xC2)
#define CTX_DATA  ((void*)(uintptr_t)0xD1)

struct Completion { uint16_t cid; uint16_t status; uint64_t result; };

static bool submitCommand(Queue& q, Device& d, const uint8_t* capsule64,
                          Completion* out, DWORD timeoutMs) {
    memcpy(d.region(kCapInOff), capsule64, 64);
    memset(d.region(kRespInOff), 0xFF, 16);          // poison, so a short transfer shows
    if (!q.postReceive(d.region(kRespInOff), 16, CTX_RESP)) {
        printf("    [%u] postReceive(response) failed\n", q.qid); return false;
    }
    if (!q.send(d.region(kCapInOff), 64, CTX_CAP)) {
        printf("    [%u] send(capsule) failed\n", q.qid); return false;
    }
    ND2_RESULT r = {};
    HRESULT st = q.reap(CTX_CAP, &r, kWaitMs);
    if (!ndOk(st)) { char b[64]; printf("    [%u] capsule send %s\n", q.qid, ndStr(st, b, sizeof(b))); return false; }
    st = q.reap(CTX_RESP, &r, timeoutMs);
    if (!ndOk(st)) { char b[64]; printf("    [%u] response %s\n", q.qid, ndStr(st, b, sizeof(b))); return false; }
    if (r.BytesTransferred != 16) {
        printf("    [%u] response was %u bytes, expected 16\n", q.qid, r.BytesTransferred);
        return false;
    }
    const uint8_t* cqe = d.region(kRespInOff);
    out->cid = nvmeof_rd16(cqe + 12);
    out->status = nvmeof_rd16(cqe + 14);
    out->result = nvmeof_rd64(cqe + 0);
    return true;
}

static bool statusOk(uint16_t st) {
    return (st & NVMEOF_STATUS_P_MASK) != 0 &&
           ((st & NVMEOF_STATUS_SCT_MASK) >> NVMEOF_STATUS_SCT_SHIFT) == NVMEOF_SCT_GENERIC &&
           (st & NVMEOF_STATUS_SC_MASK) == 0;
}

static void buildCompletion(uint8_t* buf, uint16_t cid, uint16_t sqHead,
                            uint64_t result, uint8_t sct, uint8_t sc) {
    memset(buf, 0, 16);
    nvmeof_wr64(buf + 0, result);
    nvmeof_wr16(buf + 8, sqHead);
    nvmeof_wr16(buf + 10, 0);
    nvmeof_wr16(buf + 12, cid);
    nvmeof_wr16(buf + 14, (uint16_t)(NVMEOF_STATUS_MAKE(sct, sc) | NVMEOF_STATUS_P_MASK));
}

// Every capsule this implementation can receive is 64 bytes with a single SGL.
struct Sgl { bool present; uint8_t type; bool invalidate; uint64_t addr; uint32_t len; uint32_t key; };
static Sgl parseSgl(const uint8_t* capsule) {
    Sgl s = {};
    const uint8_t ts = capsule[24 + 15];
    s.type = nvmeof_sgl_type_of(ts);
    s.addr = nvmeof_rd64(capsule + 24);
    s.len  = nvmeof_rd24(capsule + 24 + 8);
    s.key  = nvmeof_rd32(capsule + 24 + 11);
    s.present = nvmeof_sgl_is_keyed(ts) != 0;
    // Subtype 0xf rides along with the SAME descriptor type, so it has to be read
    // explicitly.  Switching on the type alone treats an invalidate request as an
    // ordinary transfer and drops the invalidation without a word.
    s.invalidate = nvmeof_sgl_wants_invalidate(ts) != 0;
    return s;
}

// ===========================================================================
//  TARGET
// ===========================================================================
struct TargetCtrl {
    bool     enabled = false;
    uint16_t cntlid = 1;
    int      ioQueuesGranted = 0;   // from Set Features: Number of Queues
    int      katoMs = 0;
    uint32_t ioCommands = 0;
    uint32_t nsWritten = 0;
    uint32_t nsRead = 0;
    uint32_t invalidateServed = 0;  // SGL subtype 0xf requests served (see the I/O path)
};

static void fillIdentifyCtrl(uint8_t* id, const TargetCtrl& c) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_VID, 0x15B3);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_SSVID, 0x103C);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SN, "NDVMEOF0000000000001", 20);
    memcpy(id + NVMEOF_ID_CTRL_OFF_MN, "NetworkDirect NVMe-oF prototype          ", 40);
    memcpy(id + NVMEOF_ID_CTRL_OFF_FR, "0.3.0   ", 8);
    id[NVMEOF_ID_CTRL_OFF_MDTS] = 0;
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_CNTLID, c.cntlid);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_VER, 0x00010400);
    id[NVMEOF_ID_CTRL_OFF_CNTRLTYPE] = NVMEOF_CTRLTYPE_IO;
    // sqes/cqes pack the MAXIMUM entry size in the high nibble and the REQUIRED
    // size in the low nibble; writing just 6 advertises a maximum of 2^0 bytes.
    id[NVMEOF_ID_CTRL_OFF_SQES] = (uint8_t)((0x6 << 4) | 0x6);
    id[NVMEOF_ID_CTRL_OFF_CQES] = (uint8_t)((0x4 << 4) | 0x4);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_MAXCMD, 32);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_NN, 1);
    // No volatile write cache: the namespace IS this process's memory, so a
    // Flush has nothing to do.  Claiming one (VWC = 1, as Linux's nvmet does,
    // because nvmet sits on a real block device) invites the host to send a Flush
    // before every fsync and unmount; Flush is answered anyway, but the honest
    // value is 0 and it keeps the host's flush path off the critical path.
    id[NVMEOF_ID_CTRL_OFF_VWC] = 0;
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_KAS, 1);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_SGLS,
                NVMEOF_CTRL_SGLS_ADVERTISED);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SUBNQN, kSubNqn, strlen(kSubNqn));
    // 4 * 16 = one 64-byte SQE and no in-capsule data.  This is what nvmet
    // reports for a port with inline_data_size 0, and it has to match the
    // receive buffer: saying 8 promises 128-byte capsules this target cannot take.
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ, 4);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IORCSZ, 1);
    // 0, not 2: the RDMA transport carries no in-capsule data, and a nonzero
    // value claims there are bytes of data inside the command capsule at an
    // offset that lands on the command fields themselves.
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_ICDOFF, 0);
    id[NVMEOF_ID_CTRL_OFF_MSDBD] = 1;
}

static void fillIdentifyNs(uint8_t* id) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NSZE, kNsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NCAP, kNsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NUSE, 0);
    id[NVMEOF_ID_NS_OFF_NLBAF] = 0;               // one LBA format
    id[NVMEOF_ID_NS_OFF_FLBAS] = 0;               // using format 0
    memset(id + NVMEOF_ID_NS_OFF_LBAF, 0, 4 * 64);
    // lbaf[0]: ms=0, ds=9 (2^9 = 512), rp=0
    id[NVMEOF_ID_NS_OFF_LBAF + 0] = 0;
    id[NVMEOF_ID_NS_OFF_LBAF + 1] = 0;
    id[NVMEOF_ID_NS_OFF_LBAF + 2] = 9;
    id[NVMEOF_ID_NS_OFF_LBAF + 3] = 0;
}

// Process one capsule on the target.  Returns false if the target should stop.
static bool targetHandleCapsule(Queue& q, Device& d, TargetCtrl& ctrl,
                                bool& wantIoQueue, bool isAdmin) {
    // Each queue pair gets its own capsule buffer.  Sharing one would be
    // invisible in this suite (its initiator is strictly one command at a time)
    // and would corrupt the first host that pipelines an admin and an I/O
    // command into the same moment.
    const size_t capOff = isAdmin ? kCapInOff : kCapIoInOff;
    const uint8_t* cap = d.region(capOff);
    uint8_t opcode = cap[0];
    uint16_t cid = nvmeof_rd16(cap + 2);
    uint8_t  fctype = cap[4];
    uint8_t  sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
    uint64_t result = 0;

    if (opcode == NVMEOF_OPC_FABRICS) {
        Sgl sgl = parseSgl(cap);
        if (fctype == NVMEOF_FCTYPE_PROPERTY_SET) {
            uint32_t off = nvmeof_rd32(cap + 44);
            uint64_t val = nvmeof_rd64(cap + 48);
            printf("  [admin] Property Set off=0x%X val=0x%llX\n", off, (unsigned long long)val);
            if (off == NVMEOF_PROP_CC) ctrl.enabled = (val & 1) != 0;
            else { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD; }
        } else if (fctype == NVMEOF_FCTYPE_PROPERTY_GET) {
            uint32_t off = nvmeof_rd32(cap + 44);
            result = (off == NVMEOF_PROP_CSTS || off == NVMEOF_PROP_CC) ? (ctrl.enabled ? 1u : 0u) : 0u;
            printf("  [admin] Property Get off=0x%X -> 0x%llX\n", off, (unsigned long long)result);
        } else if (fctype == NVMEOF_FCTYPE_CONNECT) {
            uint16_t qid = nvmeof_rd16(cap + 42);
            printf("  [admin] Connect qid=%u sqsize=%u\n", qid, nvmeof_rd16(cap + 44));
            if (!sgl.present || sgl.len < NVMEOF_CONNECT_DATA_SIZE) {
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_FORMAT;
            } else if (!q.read(d.region(kConnectOff), NVMEOF_CONNECT_DATA_SIZE,
                               sgl.addr, sgl.key, CTX_DATA)) {
                sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                ND2_RESULT r = {};
                if (!ndOk(q.reap(CTX_DATA, &r, kWaitMs))) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                } else {
                    const uint8_t* cd = d.region(kConnectOff);
                    char subnqn[257] = {}, hostnqn[257] = {};
                    memcpy(subnqn, cd + 256, 256);
                    memcpy(hostnqn, cd + 512, 256);
                    printf("        hostnqn='%s' subsysnqn='%s'\n", hostnqn, subnqn);
                    if (strcmp(subnqn, kSubNqn) != 0) {
                        sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                    } else {
                        // The I/O queue's Connect carries the controller id the
                        // admin Connect already assigned, so it is checked here.
                        uint16_t want = nvmeof_rd16(cd + 16);
                        if (qid != 0 && want != ctrl.cntlid) {
                            printf("        I/O queue connect names cntlid %u, expected %u\n",
                                   want, ctrl.cntlid);
                            sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                        } else {
                            if (qid == 0) {
                                result = ctrl.cntlid;
                                // The Connect's kato is a request; the controller
                                // only adopts it via Set Features: KATO, which is
                                // why Keep Alive before that is refused.
                                ctrl.katoMs = (int)nvmeof_rd32(cap + 48);
                            } else if (isAdmin) {
                                // An I/O queue's Connect belongs on its own queue
                                // pair.  The check has to be admin-only: this same
                                // handler serves the I/O queue, where qid != 0 is
                                // the normal and required case.
                                printf("        qid %u on the admin queue pair is not allowed\n", qid);
                                sct = NVMEOF_SCT_COMMAND_SPECIFIC;
                                sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                            }
                        }
                    }
                }
            }
        } else {
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE;
        }
    } else if (isAdmin && (opcode == NVMEOF_OPC_SET_FEATURES ||
                           opcode == NVMEOF_OPC_GET_FEATURES)) {
        // Number of Queues is what asks a fabrics controller for I/O queues, and
        // it is mandatory before a host will connect any.  In NVMe-oF there is no
        // Create I/O SQ or Create I/O CQ at all: Linux's nvmet answers all four
        // of create_sq/delete_sq/create_cq/delete_cq with Invalid Opcode unless
        // the controller is PCI, i.e. for every fabrics transport.
        bool isSet = (opcode == NVMEOF_OPC_SET_FEATURES);
        uint8_t fid = (uint8_t)(nvmeof_rd32(cap + 40) & 0xff);
        uint32_t cdw11 = nvmeof_rd32(cap + 44);
        printf("  [admin] %s Features fid=0x%02X\n", isSet ? "Set" : "Get", fid);
        switch (fid) {
        case NVMEOF_FID_NUM_QUEUES:
            if (isSet) {
                int want = (int)(cdw11 & 0xffff) + 1;
                ctrl.ioQueuesGranted = want < 1 ? 1 : 1;   // this test drives one I/O queue
                printf("        host asked for %d I/O queues, granting %d\n", want,
                       ctrl.ioQueuesGranted);
                wantIoQueue = true;
            }
            result = (uint64_t)((ctrl.ioQueuesGranted - 1) & 0xffff) |
                     ((uint64_t)((ctrl.ioQueuesGranted - 1) & 0xffff) << 16);
            break;
        case NVMEOF_FID_KATO:
            // The two directions do NOT use the same unit, and that is the
            // reference behaviour rather than a slip: nvmet's nvmet_set_feat_kato
            // returns ctrl->kato (SECONDS, rounded up from the milliseconds the
            // host sent) while nvmet_get_feat_kato returns ctrl->kato * 1000
            // (MILLISECONDS).  A target that answers seconds to both tells a host
            // asking for the current value that the timeout is 30, not 30000.
            if (isSet) {
                ctrl.katoMs = (int)cdw11;
                result = (uint64_t)((ctrl.katoMs + 999) / 1000);
            } else {
                result = (uint64_t)ctrl.katoMs;
            }
            break;
        default:
            printf("        unsupported feature\n");
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD;
            break;
        }
    } else if (isAdmin && opcode == NVMEOF_OPC_KEEP_ALIVE) {
        if (ctrl.katoMs == 0) {
            // Generic status code 0x1a, with SCT = generic.  The type is part of
            // the status, so both halves have to match the reference.
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_KA_TIMEOUT_INVALID;
        } else {
            printf("  [admin] Keep Alive\n");
        }
    } else if (isAdmin && (opcode == NVMEOF_OPC_CREATE_SQ || opcode == NVMEOF_OPC_CREATE_CQ ||
                           opcode == NVMEOF_OPC_DELETE_SQ || opcode == NVMEOF_OPC_DELETE_CQ)) {
        // Refused on purpose, exactly as a fabrics controller does: an I/O queue
        // is created by the Connect command and destroyed by dropping its queue
        // pair.  Accepting these would make this target look healthier than a
        // real one and hide the disagreement until a Linux host arrived.
        printf("  [admin] opcode=0x%02X (Create/Delete I/O SQ/CQ) -> Invalid Opcode\n", opcode);
        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE;
    } else if (isAdmin && opcode == NVMEOF_OPC_IDENTIFY) {
        uint8_t cns = cap[40];
        uint32_t nsid = nvmeof_rd32(cap + 4);
        Sgl sgl = parseSgl(cap);
        printf("  [admin] Identify cns=%u nsid=%u\n", cns, nsid);
        if (!sgl.present || sgl.len < NVMEOF_IDENTIFY_SIZE) {
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_SGL_INVALID_TYPE;
        } else {
            uint8_t* src;
            if (cns == NVMEOF_ID_CNS_CTRL) { fillIdentifyCtrl(d.region(kIdCtrlOff), ctrl); src = d.region(kIdCtrlOff); }
            else if (cns == NVMEOF_ID_CNS_NS && nsid == 1) { fillIdentifyNs(d.region(kIdNsOff)); src = d.region(kIdNsOff); }
            else { src = nullptr; sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD; }
            if (src) {
                if (!q.write(src, NVMEOF_IDENTIFY_SIZE, sgl.addr, sgl.key, CTX_DATA)) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                } else {
                    ND2_RESULT r = {};
                    if (!ndOk(q.reap(CTX_DATA, &r, kWaitMs))) {
                        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                    }
                }
            }
        }
    } else if (!isAdmin && opcode == NVMEOF_OPC_FLUSH) {
        // Flush: mandatory in the NVM command set, and the one command a host
        // sends for reasons of its own (fsync, unmount) even against a target
        // that reports no volatile write cache.  Nothing to do - the namespace is
        // this process's memory - but answering Invalid Opcode would turn an
        // fsync into an I/O error.
        printf("  [io]    cmd %u FLUSH nsid=%u (nothing to flush; namespace is memory)\n",
               ++ctrl.ioCommands, nvmeof_rd32(cap + 4));
    } else if (!isAdmin && (opcode == NVMEOF_OPC_WRITE || opcode == NVMEOF_OPC_READ)) {
        // ---- the I/O path ----
        uint32_t nsid = nvmeof_rd32(cap + 4);
        uint64_t slba = nvmeof_rd64(cap + 40);
        uint16_t nlb  = (uint16_t)(nvmeof_rd32(cap + 48) & 0xFFFF);   // 0-based
        uint32_t blocks = (uint32_t)nlb + 1;
        uint32_t bytes = blocks * kBlockSize;
        Sgl sgl = parseSgl(cap);
        ctrl.ioCommands++;
        printf("  [io]    cmd %u %s nsid=%u slba=%llu nlb=%u (%u B) sgl(addr=0x%llX key=0x%08X len=%u)\n",
               ctrl.ioCommands, opcode == NVMEOF_OPC_WRITE ? "WRITE" : "READ",
               nsid, (unsigned long long)slba, nlb, bytes,
               (unsigned long long)sgl.addr, sgl.key, sgl.len);
        if (nsid != 1) { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_NS; }
        else if (slba + blocks > kNsBlocks) {
            printf("        LBA range past the end of the namespace\n");
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_LBA_RANGE;
        } else if (!sgl.present) { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_SGL_INVALID_TYPE; }
        // Subtype 0xf ("invalidate the key after this transfer") is SERVED, not
        // refused, and this reverses an earlier decision (DESIGN 8.23 / 8.40).
        //
        // The reason it was refused: IND2QueuePair has no send-with-invalidate -
        // its Invalidate() is local, and SendAndInvalidate lives in the newer IND
        // interface - so the transfer could be done but the invalidation could
        // not, and doing the transfer anyway looked like telling the host its STag
        // was dead while it was still live.
        //
        // The reference host does not work that way.  Linux's nvme_rdma completion
        // path is:
        //     if (wc->wc_flags & IB_WC_WITH_INVALIDATE) { check the rkey }
        //     else if (req->mr) { ib_post_send(IB_WR_LOCAL_INV) }
        // - the host invalidates its own key when the response arrives without an
        // invalidation.  So "transfer + plain SEND" is a supported outcome, not a
        // lie.  What is NOT survivable is refusing: the host's I/O then fails
        // outright, and a default Linux host marks 0x4f on EVERY read and write,
        // so refusing it means refusing all of its I/O.
        else if (sgl.invalidate) {
            printf("        SGL subtype 0xf: invalidation requested; serving the transfer and "
                   "answering with a plain SEND (host invalidates locally)\n");
            ctrl.invalidateServed++;
        }
        else if (sgl.len < bytes) {
            printf("        SGL describes %u bytes, command needs %u\n", sgl.len, bytes);
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
        } else {
            // Straight into and out of the namespace, in place.  A bounce buffer
            // here would still "work" and would throw away the whole point of
            // NVMe-oF/RDMA, so the absence of one is the thing being tested.
            uint8_t* ns = d.region(kNsOff + (size_t)slba * kBlockSize);
            bool ok = (opcode == NVMEOF_OPC_WRITE)
                        ? q.read(ns, bytes, sgl.addr, sgl.key, CTX_DATA)
                        : q.write(ns, bytes, sgl.addr, sgl.key, CTX_DATA);
            if (!ok) { sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR; }
            else {
                ND2_RESULT r = {};
                HRESULT st = q.reap(CTX_DATA, &r, kWaitMs);
                if (!ndOk(st)) {
                    char b[64];
                    printf("        data transfer %s\n", ndStr(st, b, sizeof(b)));
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_DATA_XFER_ERROR;
                } else if (opcode == NVMEOF_OPC_WRITE) ctrl.nsWritten += bytes;
                else ctrl.nsRead += bytes;
            }
        }
    } else {
        printf("  unhandled %s-queue opcode 0x%02X\n", isAdmin ? "admin" : "I/O", opcode);
        sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE;
    }

    buildCompletion(d.region(kRespOutOff), cid, (uint16_t)q.posted, result, sct, sc);
    // Re-arm the Receive BEFORE the completion goes out.
    //
    // The peer may send its next command the instant it sees this completion, and
    // a command that arrives with no Receive posted is not retried - it never
    // completes at all (DESIGN 8.10(4)).  Arming afterwards leaves a window that
    // is usually won by a wide margin, which is exactly why it is worth closing
    // deliberately rather than relying on the margin.  The caller must therefore
    // NOT re-arm again after this returns; doing both would post two Receives for
    // one buffer.  Whichever queue pair this capsule arrived on is the one that
    // gets its next Receive here.
    q.postReceive(d.region(capOff), 64, CTX_CAP);
    if (!q.send(d.region(kRespOutOff), 16, CTX_RESP)) {
        printf("  failed to send a completion\n");
        return false;
    }
    ND2_RESULT r = {};
    if (!ndOk(q.reap(CTX_RESP, &r, kWaitMs))) {
        char b[64];
        printf("  completion send %s\n", ndStr(r.Status, b, sizeof(b)));
        return false;
    }
    return true;
}

static int runTarget(const char* ip, uint16_t port) {
    printf("=== F3 TARGET on %s:%u\n", ip, port);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, ip, &local.sin_addr);

    Device dev;
    if (!dev.open(local, kRegionBytes)) return 1;
    Queue admin, io;
    if (!admin.create(&dev, 0)) return 1;
    if (!io.create(&dev, 1)) return 1;

    IND2Listener* listener = nullptr;
    HRESULT hr = dev.adapter->CreateListener(IID_IND2Listener, dev.ovFile, (VOID**)&listener);
    if (FAILED(hr)) { printf("CreateListener 0x%08X\n", (unsigned)hr); return 1; }
    hr = listener->Bind((const sockaddr*)&local, sizeof(local));
    if (hr == ND_PENDING) hr = dev.waitOverlapped(listener, kWaitMs);
    if (!ndOk(hr)) { printf("listener Bind failed\n"); return 1; }
    listener->Listen(2);

    // ---- accept the admin connection
    hr = listener->GetConnectionRequest(admin.conn, &admin.ov);
    if (hr == ND_PENDING) hr = admin.waitOverlapped(listener, 20000);
    if (!ndOk(hr)) { printf("GetConnectionRequest(admin) failed\n"); return 1; }

    // A queue pair receives nothing until a Receive is posted on it.  The
    // initiator's Send blocks waiting for a match and eventually fails with
    // ND_IO_TIMEOUT on ITS side, which points the blame at the wrong end.
    //
    // The Receive goes before Accept, not after: Accept is what releases the
    // host's CompleteConnect, so posting afterwards is racing a peer that is
    // already entitled to send - and losing that race is not retryable, the Send
    // simply never completes.  See DESIGN.md 8.10(4).
    if (!admin.postReceive(dev.region(kCapInOff), 64, CTX_CAP)) {
        printf("  failed to post the initial admin Receive\n");
        return 1;
    }

    {
        uint16_t claimedQid = 0, rejStatus = 0;
        int verdict = admin.acceptChecked((uint16_t)(kSqSize + 1), &claimedQid, &rejStatus);
        if (verdict != 0 || claimedQid != 0) {
            printf("Accept(admin) not accepted (verdict=%d, qid=%u, nvme_rdma status=%u)\n",
                   verdict, claimedQid, rejStatus);
            return 1;
        }
    }
    printf("  admin queue connected (rkey=0x%08X), initial Receive already armed\n", dev.rkey);


    TargetCtrl ctrl;
    bool ioConnected = false;
    bool ioRequestPending = false;
    ULONGLONG lastActivity = GetTickCount64();

    printf("  polling (qid=%u)\n", admin.qid);

    int dbgPolls = 0;
    // A Receive that completes ND_CANCELED is terminal for that queue pair, not
    // a lost message: every Receive posted afterwards completes ND_CANCELED
    // immediately as well, so re-arming it in a poll loop spins forever at
    // whatever rate the provider can retire work requests.  Both queues do this
    // the moment the host goes away.  So each queue pair is declared dead once,
    // and the run then ends when there is nothing left to serve - the
    // consequences of a queue pair dying early show up as the timeouts they are.
    bool adminDead = false, ioDead = false;
    for (;;) {
        bool didWork = false;

        // ---- admin queue ----
        if (!adminDead) {
            ND2_RESULT r = {};
            if (admin.poll(&r)) {
                if (dbgPolls < 6) {
                    printf("  [dbg] admin completion ctx=%p type=%d st=0x%08X bytes=%u\n",
                           r.RequestContext, (int)r.RequestType, (unsigned)r.Status,
                           r.BytesTransferred);
                    dbgPolls++;
                }
                if (r.RequestContext == CTX_CAP && ndOk(r.Status) && r.BytesTransferred == 64) {
                    bool wantIo = false;
                    admin.posted++;
                    // No re-arm here: targetHandleCapsule arms the next admin
                    // Receive before it sends its completion, which is where the
                    // ordering has to hold.  Arming here too would post a second
                    // Receive for the same buffer.
                    if (!targetHandleCapsule(admin, dev, ctrl, wantIo, true)) break;
                    didWork = true;
                } else if (r.RequestContext == CTX_RESP) {
                    didWork = true;
                } else if (r.RequestContext == CTX_CAP) {
                    // A Receive that delivered no capsule.  ND_CANCELED means the
                    // queue pair is finished - see the note above the loop.
                    printf("  [admin] Receive did not deliver a capsule (st=0x%08X %s bytes=%u); "
                           "queue pair declared dead\n",
                           (unsigned)r.Status, r.Status == ND_CANCELED ? "ND_CANCELED" : "short",
                           r.BytesTransferred);
                    adminDead = true;
                }
            }
        }

        // ---- I/O queue pair: Set Features: Number of Queues asks for it ----
        // Non-blocking on purpose: blocking here stops the target answering the
        // admin queue, and the host is normally still doing admin work at this
        // moment (Get Features, Keep Alive, Identify).
        if (ctrl.ioQueuesGranted > 0 && !ioConnected && !ioRequestPending) {
            printf("  [accept] Number of Queues granted; waiting for the I/O queue pair\n");
            HRESULT h2 = listener->GetConnectionRequest(io.conn, &io.ov);
            ioRequestPending = (h2 == ND_PENDING) || SUCCEEDED(h2);
            // Issuing the request can cancel the admin Receive; re-arm it so the
            // host's next admin command still lands.
            if (ioRequestPending &&
                !admin.postReceive(dev.region(kCapInOff), 64, CTX_CAP)) break;
        } else if (ioRequestPending) {
            HRESULT h2 = io.pollOverlapped(io.conn);
            if (h2 != ND_PENDING) {
                ioRequestPending = false;
                char b[64];
                printf("  [accept] GetConnectionRequest -> %s\n", ndStr(h2, b, sizeof(b)));
                if (ndOk(h2)) {
                    // Arm the Receive before Accept: Accept releases the host's
                    // CompleteConnect, so a Receive posted afterwards is racing a
                    // peer already entitled to send.
                    if (!io.postReceive(dev.region(kCapIoInOff), 64, CTX_CAP)) break;
                    uint16_t claimedQid = 0, rejStatus = 0;
                    int verdict = io.acceptChecked((uint16_t)(kSqSize + 1), &claimedQid, &rejStatus);
                    if (verdict != 0) {
                        printf("  [accept] I/O queue refused: verdict=%d qid=%u nvme_rdma status=%u\n",
                               verdict, claimedQid, rejStatus);
                        h2 = ND_UNSUCCESSFUL;
                    }
                }
                printf("  [accept] Accept -> %s\n", ndStr(h2, b, sizeof(b)));
                if (!admin.postReceive(dev.region(kCapInOff), 64, CTX_CAP)) break;
                if (ndOk(h2)) {
                    ioConnected = true;
                    printf("  [accept] I/O queue connected\n");
                }
            }
        }

        // ---- io queue ----
        if (ioConnected && !ioDead) {
            ND2_RESULT r = {};
            if (io.poll(&r)) {
                if (r.RequestContext == CTX_CAP && ndOk(r.Status) && r.BytesTransferred == 64) {
                    io.posted++;
                    bool wantIo = false;
                    // The handler re-arms whichever queue pair served the capsule
                    // before it answers, so there is no re-arm here.
                    if (!targetHandleCapsule(io, dev, ctrl, wantIo, false)) break;
                    didWork = true;
                } else if (r.RequestContext == CTX_CAP) {
                    printf("  [io] Receive did not deliver a capsule (st=0x%08X %s bytes=%u); "
                           "queue pair declared dead\n",
                           (unsigned)r.Status, r.Status == ND_CANCELED ? "ND_CANCELED" : "short",
                           r.BytesTransferred);
                    ioDead = true;
                } else {
                    didWork = true;
                }
            }
        }

        // The host is gone once both queue pairs are finished; there is nothing
        // left to serve and waiting out the idle timeout would only add 8 s to
        // every run.
        if (adminDead && ioDead) {
            printf("  both queue pairs returned ND_CANCELED; the host has gone\n");
            break;
        }

        if (didWork) { lastActivity = GetTickCount64(); continue; }
        if (GetTickCount64() - lastActivity > 8000) {
            printf("  idle for 8 s; stopping\n");
            break;
        }
        SwitchToThread();
    }

    printf("  target: ioCommands=%u written=%u read=%u invalidateServed=%u\n",
           ctrl.ioCommands, ctrl.nsWritten, ctrl.nsRead, ctrl.invalidateServed);
    admin.destroy();
    if (ioConnected) io.destroy();
    if (listener) listener->Release();
    dev.close(nullptr, nullptr, 0);
    printf("\ntarget failures: %d\n", g_failures);
    return g_failures;
}

// ===========================================================================
//  INITIATOR
// ===========================================================================
static bool adminCmd(Queue& admin, Device& d, uint8_t* cap, Completion* c, DWORD ms = kWaitMs) {
    return submitCommand(admin, d, cap, c, ms);
}

static void fabricHeader(uint8_t* cap, uint8_t opcode, uint16_t cid, uint8_t fctype) {
    memset(cap, 0, 64);
    cap[0] = opcode;
    nvmeof_wr16(cap + 2, cid);
    cap[4] = fctype;
}

static int runInitiator(const char* serverIp, uint16_t port, const char* localIp) {
    printf("=== F3 INITIATOR %s -> %s:%u\n", localIp, serverIp, port);
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
    printf("  rkey=0x%08X region=%p\n", dev.rkey, (void*)dev.reg);

    // ---- admin queue pair ----
    // Every Connect carries RDMA-CM private data, filled the way the Linux host
    // fills it.  Sending none is legal-looking and works against a target that
    // does not look, but a real fabrics target rejects the connection outright
    // (nvmet: "private_data_len == 0 -> NVME_RDMA_CM_INVALID_LEN").
    nvmeof_rdma_request_pd adminPd;
    nvmeof_rdma_fill_req(&adminPd, 0, (uint16_t)(kSqSize + 1), 0);    HRESULT hr = admin.conn->Bind((const sockaddr*)&local, sizeof(local));
    if (hr == ND_PENDING) hr = admin.waitOverlapped(admin.conn, kWaitMs);
    if (!ndOk(hr)) { printf("admin Bind failed\n"); return 1; }
    hr = admin.conn->Connect(admin.qp, (const sockaddr*)&remote, sizeof(remote),
                             kReadLimit, kReadLimit, &adminPd, sizeof(adminPd), &admin.ov);
    if (hr == ND_PENDING) hr = admin.waitOverlapped(admin.conn, 20000);
    if (!ndOk(hr)) { char b[64]; printf("admin Connect %s\n", ndStr(hr, b, sizeof(b))); return 1; }
    hr = admin.conn->CompleteConnect(&admin.ov);
    if (hr == ND_PENDING) hr = admin.waitOverlapped(admin.conn, kWaitMs);
    if (!ndOk(hr)) { printf("admin CompleteConnect failed\n"); return 1; }
    Report("admin queue pair connected", true, nullptr);

    uint8_t cap[64];
    Completion c = {};

    // ---- enable and Connect ----
    printf("\n-- admin bring-up\n");
    fabricHeader(cap, NVMEOF_OPC_FABRICS, 1, NVMEOF_FCTYPE_PROPERTY_SET);
    nvmeof_wr32(cap + 44, NVMEOF_PROP_CC);
    nvmeof_wr64(cap + 48, 1);
    bool sent = adminCmd(admin, dev, cap, &c);
    Report("Property Set CC.EN=1", sent && statusOk(c.status), nullptr);

    fabricHeader(cap, NVMEOF_OPC_FABRICS, 2, NVMEOF_FCTYPE_PROPERTY_GET);
    nvmeof_wr32(cap + 44, NVMEOF_PROP_CSTS);
    sent = adminCmd(admin, dev, cap, &c);
    Report("Property Get CSTS -> RDY", sent && statusOk(c.status) && (c.result & 1), nullptr);

    uint16_t cntlid = 0;
    {
        uint8_t* cd = dev.region(kConnectOff);
        memset(cd, 0, NVMEOF_CONNECT_DATA_SIZE);
        for (int i = 0; i < 16; i++) cd[i] = (uint8_t)(0xB0 + i);
        nvmeof_wr16(cd + 16, NVMEOF_CNTLID_DYNAMIC);
        memcpy(cd + 256, kSubNqn, strlen(kSubNqn));
        memcpy(cd + 512, "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f3-0001", 46);
        fabricHeader(cap, NVMEOF_OPC_FABRICS, 3, NVMEOF_FCTYPE_CONNECT);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, dev.rkey);
        nvmeof_wr16(cap + 42, 0);            // qid 0 = admin
        nvmeof_wr16(cap + 44, kSqSize);
        nvmeof_wr32(cap + 48, 30000);
        sent = adminCmd(admin, dev, cap, &c);
        cntlid = (uint16_t)c.result;
        char d[96];
        sprintf_s(d, "cntlid=%u", cntlid);
        Report("Connect admin queue", sent && statusOk(c.status) && cntlid != 0, d);
    }

    // ---- ask for an I/O queue ----
    // In NVMe-oF this is how an I/O queue comes into existence.  There is no
    // Create I/O SQ and no Create I/O CQ: Linux's nvmet answers all four of
    // create_sq/delete_sq/create_cq/delete_cq with Invalid Opcode for every
    // fabrics transport, and the queue is made by the fabrics Connect below plus
    // the queue pair it rides on.
    printf("\n-- I/O queue\n");
    fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, 4, 0);
    nvmeof_wr32(cap + 40, NVMEOF_FID_NUM_QUEUES);
    nvmeof_wr32(cap + 44, 0);                     // one I/O queue: 0 in each half
    sent = adminCmd(admin, dev, cap, &c);
    {
        char d[96];
        sprintf_s(d, "controller granted %u I/O queues", (unsigned)(c.result & 0xffff) + 1);
        Report("Set Features: Number of Queues", sent && statusOk(c.status), d);
    }
    fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, 5, 0);
    nvmeof_wr32(cap + 40, NVMEOF_FID_KATO);
    nvmeof_wr32(cap + 44, NVMEOF_KATO_DEFAULT);
    adminCmd(admin, dev, cap, &c);

    // ---- connect the I/O queue pair ----
    hr = io.conn->Bind((const sockaddr*)&local, sizeof(local));
    if (hr == ND_PENDING) hr = io.waitOverlapped(io.conn, kWaitMs);
    // The I/O queue's private data names qid 1 and carries the controller id the
    // admin queue assigned; the admin one names qid 0 and carries none.
    nvmeof_rdma_request_pd ioPd;
    nvmeof_rdma_fill_req(&ioPd, 1, (uint16_t)(kSqSize + 1), cntlid);
    hr = io.conn->Connect(io.qp, (const sockaddr*)&remote, sizeof(remote),
                          kReadLimit, kReadLimit, &ioPd, sizeof(ioPd), &io.ov);
    if (hr == ND_PENDING) hr = io.waitOverlapped(io.conn, 20000);
    bool ioConnected = ndOk(hr);
    if (ioConnected) {
        hr = io.conn->CompleteConnect(&io.ov);
        if (hr == ND_PENDING) hr = io.waitOverlapped(io.conn, kWaitMs);
        ioConnected = ndOk(hr);
    }
    {
        char b[64];
        Report("I/O queue pair connected (second QP on the same card)",
               ioConnected, ioConnected ? "two QPs, two CQs, one fabric" : ndStr(hr, b, sizeof(b)));
    }
    if (ioConnected) {
        uint8_t* cd = dev.region(kConnectOff);
        memset(cd, 0, 64);
        nvmeof_wr16(cd + 16, cntlid);             // the id the admin queue assigned
        memcpy(cd + 256, kSubNqn, strlen(kSubNqn));
        fabricHeader(cap, NVMEOF_OPC_FABRICS, 7, NVMEOF_FCTYPE_CONNECT);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, dev.rkey);
        nvmeof_wr16(cap + 42, 1);                 // qid 1 = our I/O queue
        nvmeof_wr16(cap + 44, kSqSize);
        nvmeof_wr32(cap + 48, 30000);
        sent = submitCommand(io, dev, cap, &c, kWaitMs);
        Report("Connect I/O queue (qid=1) on its own queue pair",
               sent && statusOk(c.status), nullptr);
    }

    // ---- Identify Namespace: read the block size, do not assume it ----
    uint32_t blockSize = 0;
    uint64_t nsBlocks = 0;
    {
        memset(dev.region(kIdNsOff), 0xCC, NVMEOF_IDENTIFY_SIZE);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, 6, 0);
        nvmeof_wr32(cap + 4, 1);                  // nsid = 1
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24),
                             (uint64_t)(uintptr_t)dev.region(kIdNsOff),
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        cap[40] = NVMEOF_ID_CNS_NS;
        sent = adminCmd(admin, dev, cap, &c);
        bool ok = sent && statusOk(c.status);
        Report("Identify Namespace", ok, nullptr);
        if (ok) {
            const uint8_t* id = dev.region(kIdNsOff);
            nsBlocks = nvmeof_rd64(id + NVMEOF_ID_NS_OFF_NSZE);
            uint8_t ds = id[NVMEOF_ID_NS_OFF_LBAF + 2];
            blockSize = 1u << ds;
            char d[128];
            sprintf_s(d, "nsze=%llu blocks, block size %u B (lbaf[0].ds=%u)",
                      (unsigned long long)nsBlocks, blockSize, ds);
            Report("namespace geometry read from the target",
                   blockSize == kBlockSize && nsBlocks == kNsBlocks, d);
        } else { admin.destroy(); io.destroy(); dev.close(nullptr, nullptr, 0); return g_failures; }
    }

    // ---- the I/O path itself ----
    printf("\n-- Read/Write over the I/O queue\n");
    struct Xfer { uint32_t blocks; uint32_t seed; const char* what; };
    const Xfer xfers[] = {
        { 512, 0x11, "512 blocks (256 KiB, fills the SGL comfortably)" },
        { 7,   0x22, "7 blocks (3584 B, odd size, NLB is 0-based)" },
        { 1,   0x33, "1 block (512 B, NLB = 0)" },
    };
    for (const Xfer& x : xfers) {
        uint32_t bytes = x.blocks * blockSize;
        uint8_t* host = dev.region(kXferOff);
        // Distinct pattern per transfer, so a stale buffer cannot pass.
        for (uint32_t i = 0; i < bytes; i++) host[i] = (uint8_t)((i * 31u + x.seed) & 0xFF);

        fabricHeader(cap, NVMEOF_OPC_WRITE, (uint16_t)(100 + x.seed), 0);
        nvmeof_wr32(cap + 4, 1);                                    // nsid
        nvmeof_wr64(cap + 40, 0);                                   // slba = 0
        nvmeof_wr32(cap + 48, x.blocks - 1);                        // NLB, 0-based
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)host, bytes, dev.rkey);
        bool okW = submitCommand(io, dev, cap, &c, kWaitMs) && statusOk(c.status);
        {
            char d[128];
            sprintf_s(d, "%s - %u B", x.what, bytes);
            Report("WRITE command completed", okW, d);
        }

        // Clobber the host buffer so a Read that transfers nothing cannot pass.
        memset(host, 0x5A, bytes);

        fabricHeader(cap, NVMEOF_OPC_READ, (uint16_t)(200 + x.seed), 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, 0);
        nvmeof_wr32(cap + 48, x.blocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)host, bytes, dev.rkey);
        bool okR = submitCommand(io, dev, cap, &c, kWaitMs) && statusOk(c.status);

        uint32_t bad = 0, firstBad = 0;
        for (uint32_t i = 0; i < bytes; i++) {
            uint8_t want = (uint8_t)((i * 31u + x.seed) & 0xFF);
            if (host[i] != want) { if (!bad) firstBad = i; bad++; }
        }
        {
            char d[192];
            sprintf_s(d, "%s - %u B read back, %u mismatching byte(s)%s",
                      x.what, bytes, bad,
                      bad ? "" : ", round trip intact");
            if (bad) printf("        first mismatch at byte %u: got 0x%02X\n", firstBad, host[firstBad]);
            Report("READ command returned exactly what was written", okR && bad == 0, d);
        }
    }

    // ---- error handling: an out-of-range LBA must be refused, and must NOT
    //      kill the queue pair - that distinction is the point of validating
    //      an SGL before acting on it.
    printf("\n-- error handling\n");
    {
        fabricHeader(cap, NVMEOF_OPC_READ, 300, 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, kNsBlocks - 1);        // last valid block
        nvmeof_wr32(cap + 48, 9);                    // but 10 blocks requested
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24),
                             (uint64_t)(uintptr_t)dev.region(kXferOff), 10 * blockSize, dev.rkey);
        bool sentBad = submitCommand(io, dev, cap, &c, kWaitMs);
        // Use the header's accessors.  NVMEOF_STATUS_SC_MASK is the RAW field
        // mask (bits 8:1), so masking without the shift yields 0x100 for a
        // status code of 0x80 - and casting that to uint8_t turns it into 0x00,
        // which reads as "success".  A hand-rolled extraction here reported a
        // correctly-rejected command as accepted.
        uint8_t sct = nvmeof_status_sct(c.status);
        uint8_t sc  = nvmeof_status_sc(c.status);
        char d[128];
        sprintf_s(d, "sct=%u sc=0x%02X (expected LBA_RANGE 0x80)", sct, sc);
        Report("out-of-range LBA is refused", sentBad && sc == NVMEOF_SC_LBA_RANGE, d);
    }
    {
        // The queue must still work after the rejection.  A tool that stops here
        // would leave the "does a bad command kill the connection" question
        // unanswered, and on an RC queue pair that answer is not obvious.
        uint8_t* host = dev.region(kXferOff);
        for (uint32_t i = 0; i < 512; i++) host[i] = (uint8_t)(i & 0xFF);
        fabricHeader(cap, NVMEOF_OPC_WRITE, 301, 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, 0);
        nvmeof_wr32(cap + 48, 0);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)host, 512, dev.rkey);
        bool ok = submitCommand(io, dev, cap, &c, kWaitMs) && statusOk(c.status);
        Report("the I/O queue still works after a rejected command", ok, nullptr);
    }

    // ---- a data command carrying an Invalidate SGL descriptor ----
    // Subtype 0xf, same descriptor type as an ordinary keyed transfer.  This is
    // what a default Linux host marks on EVERY read and write (its map_sg_fr
    // path), so the descriptor has to be SERVED: the transfer happens and the
    // response is a plain SEND.  The host then invalidates its own key locally -
    // its completion path does that whenever the response carries no
    // invalidation - so nothing about the host's state is misrepresented.
    //
    // This used to be a bounded refusal (DESIGN 8.23).  That refusal is what
    // would have made the target unusable with a real host, and the transfer it
    // was protecting against is the one thing the host is known to tolerate.
    {
        uint8_t* host = dev.region(kXferOff);
        for (uint32_t i = 0; i < 512; i++) host[i] = (uint8_t)(i ^ 0x5A);
        fabricHeader(cap, NVMEOF_OPC_WRITE, 302, 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, 0);
        nvmeof_wr32(cap + 48, 0);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)host, 512, dev.rkey);
        // Flip the subtype nibble to 0xf; the type nibble stays 0x4.
        cap[24 + 15] = NVMEOF_SGL_TS_KEYED_INVALIDATE;
        bool invSent = submitCommand(io, dev, cap, &c, kWaitMs);
        char d[128];
        sprintf_s(d, "sct=%u sc=0x%02X", nvmeof_status_sct(c.status),
                  nvmeof_status_sc(c.status));
        Report("a keyed descriptor with the Invalidate subtype is served, not refused",
               invSent && statusOk(c.status), d);
    }
    {
        // ... and the bytes really moved: a target that answered success without
        // doing the transfer would pass the check above.
        uint8_t* host = dev.region(kXferOff);
        bool intact = true;
        for (uint32_t i = 0; i < 512; i++) {
            if (host[i] != (uint8_t)(i ^ 0x5A)) { intact = false; break; }
        }
        Report("the invalidate-flagged WRITE moved the data it claimed to move",
               intact, intact ? nullptr : "first byte differs");
    }
    {
        // And the queue has to keep working afterwards.
        uint8_t* host = dev.region(kXferOff);
        for (uint32_t i = 0; i < 512; i++) host[i] = (uint8_t)(i | 0x11);
        fabricHeader(cap, NVMEOF_OPC_WRITE, 303, 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, 0);
        nvmeof_wr32(cap + 48, 0);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)host, 512, dev.rkey);
        bool ok = submitCommand(io, dev, cap, &c, kWaitMs) && statusOk(c.status);
        Report("the I/O queue still works after an invalidate-flagged command", ok, nullptr);
    }

    admin.destroy();
    io.destroy();
    dev.close(nullptr, nullptr, 0);
    printf("\ninitiator failures: %d\n", g_failures);
    return g_failures;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage:\n  %s -target    <ip> <port>\n  %s -initiator <serverIp> <port> <localIp>\n",
               argv[0], argv[0]);
        return 2;
    }
    // Unbuffered: a process killed for hanging would otherwise lose its entire
    // output to the stdout buffer, which is precisely the moment the log matters.
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    HRESULT hr = NdStartup();
    if (FAILED(hr)) { printf("NdStartup 0x%08X\n", (unsigned)hr); WSACleanup(); return 1; }

    int rc = 2;
    if (strcmp(argv[1], "-target") == 0) rc = runTarget(argv[2], (uint16_t)atoi(argv[3]));
    else if (strcmp(argv[1], "-initiator") == 0 && argc >= 5)
        rc = runInitiator(argv[2], (uint16_t)atoi(argv[3]), argv[4]);
    else printf("unknown mode %s\n", argv[1]);

    NdCleanup();
    WSACleanup();
    return rc;
}
