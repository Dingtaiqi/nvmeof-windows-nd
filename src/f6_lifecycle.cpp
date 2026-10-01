// f6_lifecycle.cpp - F6: bringing a controller up and taking it down the way a
// real NVMe-oF host does it.
//
// This file replaced an earlier revision that established I/O queues with
// Create I/O Submission Queue and Create I/O Completion Queue.  That is the PCI
// model, not the fabrics model, and it cannot work against a real host:
// Linux's nvmet answers all four of create_sq/delete_sq/create_cq/delete_cq with
// Invalid Opcode unless nvmet_is_pci_ctrl() is true, i.e. for every fabrics
// transport.  NVMe-oF creates I/O queues out of the Connect command instead -
// one queue pair per I/O queue, each carrying its own fabrics Connect with qid
// 1..N, and no Create SQ/CQ anywhere in the sequence.
//
// So the flow exercised here is the one a Linux host actually performs:
//
//   1. Connect (qid 0) on the admin queue pair, with the RDMA private data
//      handshake - the transport negotiates in the connection's private data,
//      not in a capsule.
//   2. Set Features: Number of Queues.  Mandatory before any I/O queue; the
//      controller answers with how many it will give.
//   3. Property Set CC.EN = 1, then Get Features to confirm the timer/queue
//      settings took.
//   4. For each I/O queue: a fresh queue pair, then a fabrics Connect on THAT
//      queue pair carrying qid = N and the queue size.
//   5. I/O on each queue independently.
//   6. Create/Delete I/O SQ and CQ must be refused with Invalid Opcode, because
//      that is what a real fabrics controller does.  Asserting the refusal is
//      how this test pins the difference rather than quietly relying on it.
//   7. Keep Alive, then Disconnect.
//
// Deliberately out of scope: Authentication (fctype 0x05/0x06), and the
// discovery subsystem.  The subset this project targets does not implement them.

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

// ---------------------------------------------------------------------------
//  Region layout - small; F6 is about the control plane, not throughput.
// ---------------------------------------------------------------------------
static const uint32_t kBlockSize   = 512;
static const int      kSqSize      = 31;        // 0-based, so 32 entries
static const int      kMaxIoQueues = 2;
static const size_t   kRegionBytes = 4u * 1024 * 1024;

static const size_t kCapInOff      = 0;      // 2 slots of 64 B: admin at +0, io at +64
static const size_t kRespOutOff    = 256;    // 2 slots of 16 B
static const size_t kRespInOff     = 512;
static const size_t kConnectOff    = 768;
static const size_t kIdCtrlOff     = kConnectOff + 2048;
static const size_t kIdNsOff       = kIdCtrlOff  + NVMEOF_IDENTIFY_SIZE;
static const size_t kIdNsListOff   = kIdNsOff    + NVMEOF_IDENTIFY_SIZE;
static const size_t kIdNsDescOff   = kIdNsListOff + NVMEOF_IDENTIFY_SIZE;
static const size_t kXferOff       = kIdNsDescOff + NVMEOF_IDENTIFY_SIZE;
static const size_t kNsOff         = kXferOff    + 512u * 1024 + 4096;
static const size_t kNsBytes       = 1024u * 1024;
static const uint32_t kNsBlocks    = (uint32_t)(kNsBytes / kBlockSize);
static_assert(kNsOff + kNsBytes <= kRegionBytes, "namespace fits in the region");

#define CTX_CAP   ((uintptr_t)0x1000)
#define CTX_RESP  ((uintptr_t)0x1100)
#define CTX_DATA  ((uintptr_t)0x1200)
#define CTX_XFER  ((uintptr_t)0x1300)
#define CTX_CONN  ((uintptr_t)0x1400)   // io queue connection handshake

static const char* kSubNqn  = "nqn.2024-01.local.rdma:windows-nd";
static const char* kHostNqn = "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f6-0001";

struct Completion { uint16_t cid; uint16_t status; uint64_t result; };

static bool statusOk(uint16_t st) {
    return (st & NVMEOF_STATUS_P_MASK) != 0 &&
           nvmeof_status_sct(st) == NVMEOF_SCT_GENERIC &&
           nvmeof_status_sc(st) == NVMEOF_SC_SUCCESS;
}

static void buildCompletion(uint8_t* cqe, uint16_t cid, uint16_t sqHead,
                           uint64_t result, uint8_t sct, uint8_t sc) {
    memset(cqe, 0, 16);
    nvmeof_wr64(cqe + 0, result);
    nvmeof_wr16(cqe + 8, sqHead);
    nvmeof_wr16(cqe + 12, cid);
    // NVMEOF_STATUS_CQE, not NVMEOF_STATUS_MAKE: a CQE carries the phase bit,
    // and omitting it makes every success read as a failure.
    nvmeof_wr16(cqe + 14, NVMEOF_STATUS_CQE(sct, sc));
}

struct Sgl { bool present; uint8_t type; uint64_t addr; uint32_t key; uint32_t len; };

static Sgl parseSgl(const uint8_t* cap) {
    // Read the descriptor through byte offsets rather than struct fields: the
    // keyed variant packs a 24-bit length next to a 32-bit key, and the byte
    // order must not depend on bitfield allocation rules.
    Sgl s = {};
    s.type = nvmeof_sgl_type_of(cap[24 + 15]);
    s.addr = nvmeof_rd64(cap + 24);
    s.len  = nvmeof_rd24(cap + 24 + 8);
    s.key  = nvmeof_rd32(cap + 24 + 11);
    s.present = (s.type == NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK);
    return s;
}

// ---------------------------------------------------------------------------
//  Identify payloads
// ---------------------------------------------------------------------------
// Field values follow Linux nvmet's nvmet_execute_identify_ctrl, which is the
// implementation this has to interoperate with.  Three of them are easy to get
// wrong in ways nothing local notices:
//
//   * sqes/cqes pack the MAXIMUM entry size in the high nibble and the REQUIRED
//     size in the low nibble.  Writing 6 instead of 0x66 advertises a maximum
//     entry size of 2^0 = 1 byte.
//   * icdoff is not set by nvmet at all, i.e. it is 0.  The RDMA transport does
//     not use in-capsule data, so a nonzero icdoff claims there are bytes of
//     data inside the command capsule - at an offset that lands on the command
//     fields themselves.
//   * kas = 0 literally means "Keep Alive is not supported", which contradicts
//     implementing it.
static void fillIdentifyCtrl(uint8_t* id, uint16_t cntlid) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_VID, 0x15B3);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_SSVID, 0x15B3);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SN, "NDVMEOF0000000000001", 20);
    memcpy(id + NVMEOF_ID_CTRL_OFF_MN, "NetworkDirect NVMe-oF prototype          ", 40);
    memcpy(id + NVMEOF_ID_CTRL_OFF_FR, "0.5.0   ", 8);
    id[NVMEOF_ID_CTRL_OFF_RAB] = 6;
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_CNTLID, cntlid);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_VER, 0x00010400);
    id[NVMEOF_ID_CTRL_OFF_CNTRLTYPE] = NVMEOF_CTRLTYPE_IO;
    id[NVMEOF_ID_CTRL_OFF_SQES] = (uint8_t)((0x6 << 4) | 0x6);
    id[NVMEOF_ID_CTRL_OFF_CQES] = (uint8_t)((0x4 << 4) | 0x4);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_MAXCMD, (uint16_t)(kSqSize + 1));
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_NN, 1);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_ONCS, 0);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_KAS, 1);      // keep-alive granularity: 1 s
    // No volatile write cache: the namespace is this process's memory, so a Flush
    // has nothing to persist.  Claiming one (as Linux's nvmet does, because it
    // sits on a real block device) makes the host flush before every fsync and
    // unmount.  Flush is answered either way; this keeps it off the path.
    id[NVMEOF_ID_CTRL_OFF_VWC] = 0;
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_SGLS,
                NVMEOF_CTRL_SGLS_ADVERTISED);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SUBNQN, kSubNqn, strlen(kSubNqn));
    // ioccsz = 8 (a 128-byte command capsule).  Linux nvmet reports 4 here,
    // because it advertises no in-capsule data and sizes the field as
    // sizeof(struct nvme_command)/16.  It is not consumed by the RDMA host, so
    // either is safe; this one follows the fabrics floor instead of nvmet.
    // 4 * 16 = one 64-byte SQE and no in-capsule data: what nvmet reports for a
    // port with inline_data_size 0, and what the 64-byte receive buffer accepts.
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ, 4);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IORCSZ, 1);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_ICDOFF, 0);
    id[NVMEOF_ID_CTRL_OFF_MSDBD] = 1;
}

static void fillIdentifyNs(uint8_t* id) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NSZE, kNsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NCAP, kNsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NUSE, kNsBlocks);
    id[NVMEOF_ID_NS_OFF_NLBAF] = 0;
    id[NVMEOF_ID_NS_OFF_FLBAS] = 0;
    id[NVMEOF_ID_NS_OFF_LBAF + 2] = 9;    // ds = 9 -> 512 bytes
}

// Active Namespace ID list (CNS 2): a 32-bit count, then the ids, then zeros.
// The count is the number of valid ids, not the list length.
static void fillIdentifyNsList(uint8_t* id) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr32(id + 0, 1);            // one namespace
    nvmeof_wr32(id + 4, 1);            // its NSID
}

// Namespace Identification Descriptors (CNS 3).  Each descriptor is a 4-byte
// header (type, length, reserved[2]) followed by the value.  The CSI descriptor
// is the one a host needs: without it the host has to assume the namespace is
// NVM, and Linux treats a missing CSI descriptor as a reason to fall back rather
// than to fail - but emitting it is what a real target does, so emit it.
static void fillIdentifyNsDescs(uint8_t* id) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    id[0] = NVMEOF_NIDT_CSI;
    id[1] = 1;                          // descriptor length
    id[2] = 0; id[3] = 0;               // reserved
    id[4] = NVMEOF_CSI_NVM;
}

// ---------------------------------------------------------------------------
//  Target
// ---------------------------------------------------------------------------
struct TargetState {
    bool     enabled = false;
    uint16_t cntlid  = 1;
    int      ioQueuesWanted  = 0;   // what the host asked for
    int      ioQueuesGranted = 0;   // what we told it it can have
    int      katoMs = 0;            // 0 means Keep Alive is not yet configured
    uint32_t aenMask = 0;
    int      ioCommands = 0;
    int      keepAlives = 0;
    bool     disconnected = false;
    bool     qidConnected[2] = {};   // admin + one I/O queue
    bool     sqhdDisabled = false;   // host set cattr DISABLE_SQFLOW

    // 0xffff when the host said it is not tracking SQ head, 0 otherwise.  A head
    // pointer is meaningless over fabrics, and reporting one the host explicitly
    // declined to maintain is worse than reporting the reserved value.
    uint16_t sqhd() const { return sqhdDisabled ? 0xFFFF : 0; }
};

// The admin dispatcher.  Returns false only on a transport failure worth
// aborting for.
static bool targetAdmin(Queue& q, Device& d, TargetState& st) {
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
            printf("  [admin] Property Set off=0x%X val=0x%llX\n", off, (unsigned long long)val);
            if (off == NVMEOF_PROP_CC) {
                bool wasEnabled = st.enabled;
                st.enabled = (val & 1) != 0;
                // CC.EN 1 -> 0 is how a host disables a controller before it tears
                // the queue pairs down: the real "I am leaving" signal, and the one
                // this target now keys its teardown on (DESIGN 8.44).
                if (wasEnabled && !st.enabled) {
                    st.disconnected = true;
                    printf("        controller disabled by the host (CC.EN=0)\n");
                }
            } else { sc = NVMEOF_SC_INVALID_FIELD; }
        } else if (fctype == NVMEOF_FCTYPE_PROPERTY_GET) {
            // Answer the offset that was actually asked for.  Returning one
            // value for every offset is not a shortcut, it is a wrong answer to
            // three of the four registers a host reads during bring-up.
            //
            // The attrib byte selects the access size, and Linux's target splits
            // its register table on it: an 8-byte read may only name CAP, a
            // 4-byte read may only name VS/CC/CSTS/CRTO.  Validating it is what
            // keeps a target from answering a question nobody asked.
            uint32_t off = nvmeof_rd32(cap + 44);
            uint8_t attrib = cap[40];
            uint8_t size = (uint8_t)(attrib & NVMEOF_PROP_ATTRIB_SIZE_MASK);
            bool eightByte = (size == NVMEOF_PROP_SIZE_8);
            if (eightByte && off != NVMEOF_PROP_CAP) {
                printf("  [admin] Property Get 8-byte read of non-CAP offset 0x%X\n", off);
                sc = NVMEOF_SC_INVALID_FIELD;
            } else if (!eightByte && off == NVMEOF_PROP_CAP) {
                printf("  [admin] Property Get CAP needs an 8-byte read\n");
                sc = NVMEOF_SC_INVALID_FIELD;
            } else {
                switch (off) {
                case NVMEOF_PROP_CAP:  result = NVMEOF_CAP_VALUE; break;
                case NVMEOF_PROP_VS:   result = 0x00010400u; break;   // NVMe 1.4
                case NVMEOF_PROP_CC:   result = st.enabled ? 1u : 0u; break;
                case NVMEOF_PROP_CSTS: result = st.enabled ? 1u : 0u; break;   // RDY
                case NVMEOF_PROP_NSSR: result = 0; break;
                default:
                    printf("  [admin] Property Get unsupported offset 0x%X\n", off);
                    sc = NVMEOF_SC_INVALID_FIELD;
                    break;
                }
            }
            printf("  [admin] Property Get off=0x%X size=%u -> 0x%llX\n", off, size,
                   (unsigned long long)result);
        } else if (fctype == NVMEOF_FCTYPE_CONNECT) {
            uint16_t qid = nvmeof_rd16(cap + 42);
            uint16_t sqsize = nvmeof_rd16(cap + 44);
            uint8_t  cattr = cap[46];
            uint16_t recfmt = nvmeof_rd16(cap + 40);
            printf("  [admin] Connect qid=%u sqsize=%u cattr=0x%02X recfmt=%u\n",
                   qid, sqsize, cattr, recfmt);
            // Every one of these is checked by Linux's nvmet for the same
            // command, in the same order.  A target that skips them answers
            // "success" to requests a real one refuses, which is how a host and a
            // controller end up disagreeing about what was agreed.
            if (recfmt != 0) {
                printf("        connect recfmt must be 0\n");
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_FORMAT;
            } else if (sqsize == 0) {
                printf("        sqsize 0 is not a queue\n");
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
            } else if (qid != 0 && sqsize > NVMEOF_CAP_MQES) {
                // For fabrics, sqsize applies to I/O Submission Queues only, and
                // the ceiling is CAP.MQES - the same number the host read.
                printf("        sqsize %u exceeds CAP.MQES %u\n", sqsize, NVMEOF_CAP_MQES);
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
            } else if (qid < 2 && st.qidConnected[qid]) {
                printf("        qid %u is already connected\n", qid);
                sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_CMD_SEQ_ERROR;
            } else if (!sgl.present || sgl.len < NVMEOF_CONNECT_DATA_SIZE) {
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_FORMAT;
            } else if (!q.read(d.region(kConnectOff), NVMEOF_CONNECT_DATA_SIZE, sgl.addr,
                               sgl.key, (void*)CTX_DATA)) {
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                ND2_RESULT r = {};
                if (!ndOk(q.reap((void*)CTX_DATA, &r, kWaitMs))) {
                    sc = NVMEOF_SC_DATA_XFER_ERROR;
                } else {
                    const uint8_t* cd = d.region(kConnectOff);
                    char subnqn[257] = {};
                    memcpy(subnqn, cd + 256, 256);
                    uint16_t wantCntlid = nvmeof_rd16(cd + 16);
                    if (strcmp(subnqn, kSubNqn) != 0) {
                        printf("        subsysnqn mismatch '%s'\n", subnqn);
                        sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                    } else if (qid == 0 && wantCntlid != NVMEOF_CNTLID_DYNAMIC) {
                        // The admin Connect must ask for a dynamic controller id;
                        // nvmet refuses anything else outright.
                        printf("        admin connect cntlid 0x%04X is not 0xFFFF (dynamic)\n",
                               wantCntlid);
                        sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                    } else if (qid == 0) {
                        result = st.cntlid;
                        st.qidConnected[0] = true;
                    } else {
                        // A Connect with qid != 0 arriving on the ADMIN queue is
                        // the one thing in this flow that must not be accepted:
                        // an I/O queue's Connect belongs on its own queue pair.
                        printf("        qid %u on the admin queue pair is not allowed\n", qid);
                        sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                    }
                    // The host may declare that it is not tracking SQ head; the
                    // controller then reports SQHD as 0xffff rather than a head
                    // pointer nobody maintains.
                    if (cattr & NVMEOF_CONNECT_CATTR_DISABLE_SQFLOW) {
                        st.sqhdDisabled = true;
                        printf("        host disabled SQ flow control; reporting SQHD=0xffff\n");
                    }
                }
            }
        } else if (fctype == 0x08) {
            // 0x08 is not a fabrics command (DESIGN 8.44).  It used to be treated
            // here as "the host is leaving", and that willingness to accept it is
            // the only reason this suite's final step ever passed.
            printf("  [admin] fctype 0x08 is not a fabrics command -> Invalid Opcode\n");
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_OPCODE;
        } else {
            printf("  [admin] unsupported fctype=0x%02X\n", fctype);
            sc = NVMEOF_SC_INVALID_OPCODE;
        }
    } else if (opcode == NVMEOF_OPC_SET_FEATURES || opcode == NVMEOF_OPC_GET_FEATURES) {
        // cdw10 bits 7:0 are the Feature Identifier; a host sends Number of
        // Queues before it will connect any I/O queue.
        bool isSet = (opcode == NVMEOF_OPC_SET_FEATURES);
        uint8_t fid = (uint8_t)(nvmeof_rd32(cap + 40) & 0xff);
        uint32_t cdw11 = nvmeof_rd32(cap + 44);
        printf("  [admin] %s Features fid=0x%02X cdw11=0x%08X\n",
               isSet ? "Set" : "Get", fid, cdw11);
        switch (fid) {
        case NVMEOF_FID_NUM_QUEUES: {
            if (isSet) {
                uint16_t nsqr = (uint16_t)(cdw11 & 0xffff);
                uint16_t ncqr = (uint16_t)(cdw11 >> 16);
                if (nsqr == 0xffff || ncqr == 0xffff) {
                    sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_INVALID_FIELD;
                    break;
                }
                st.ioQueuesWanted  = (int)nsqr + 1;
                st.ioQueuesGranted = st.ioQueuesWanted < kMaxIoQueues
                                         ? st.ioQueuesWanted : kMaxIoQueues;
                printf("        host asked for %d I/O queues, granting %d\n",
                       st.ioQueuesWanted, st.ioQueuesGranted);
            }
            // Both Set and Get answer with (max_qid-1) in each half, which is
            // how the host learns the real number it may use.
            result = (uint64_t)((st.ioQueuesGranted - 1) & 0xffff) |
                     ((uint64_t)((st.ioQueuesGranted - 1) & 0xffff) << 16);
            break;
        }
        case NVMEOF_FID_KATO:
            if (isSet) {
                st.katoMs = (int)cdw11;
                // The answer is the granularity the controller actually uses:
                // KAS is in seconds, so round up to a whole second.
                result = (uint64_t)((st.katoMs + 999) / 1000);
            } else {
                result = (uint64_t)st.katoMs;
            }
            break;
        case NVMEOF_FID_ASYNC_EVENT:
            if (isSet) st.aenMask = cdw11;
            result = st.aenMask;
            break;
        default:
            printf("        unsupported feature\n");
            sc = NVMEOF_SC_INVALID_FIELD;
            break;
        }
    } else if (opcode == NVMEOF_OPC_KEEP_ALIVE) {
        // Admin opcode 0x18, not a fabrics ftype.  A controller with no
        // keep-alive timeout configured answers KA_TIMEOUT_INVALID rather than
        // success - which is exactly how a host discovers it never set the
        // timer, and why the initiator below sets KATO before sending this.
        if (st.katoMs == 0) {
            printf("  [admin] Keep Alive before KATO was set -> KA_TIMEOUT_INVALID\n");
            // Generic status code 0x1a, NOT a command-specific one: the type is
            // what a host uses to read the code, so the SCT has to match too.
            sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_KA_TIMEOUT_INVALID;
        } else {
            st.keepAlives++;
            printf("  [admin] Keep Alive #%d (kato=%d ms)\n", st.keepAlives, st.katoMs);
        }
    } else if (opcode == NVMEOF_OPC_CREATE_SQ || opcode == NVMEOF_OPC_CREATE_CQ ||
               opcode == NVMEOF_OPC_DELETE_SQ || opcode == NVMEOF_OPC_DELETE_CQ) {
        // Refused on purpose, and this is the point of the test.
        //
        // Linux's nvmet answers all four of these with Invalid Opcode unless
        // nvmet_is_pci_ctrl() is true, i.e. for every fabrics transport: in
        // NVMe-oF an I/O queue is created by the Connect command and destroyed
        // by dropping its queue pair.  Accepting them would make this target
        // look healthier than a real one and hide the disagreement until a real
        // host arrived - which is precisely the failure mode F5 exists to catch.
        printf("  [admin] opcode=0x%02X (Create/Delete I/O SQ/CQ) -> Invalid Opcode, "
               "as a fabrics controller does\n", opcode);
        sc = NVMEOF_SC_INVALID_OPCODE;
    } else if (opcode == NVMEOF_OPC_IDENTIFY) {
        uint8_t cns = cap[40];
        printf("  [admin] Identify cns=%u\n", cns);
        uint8_t* src = nullptr;
        if (cns == NVMEOF_ID_CNS_CTRL) {
            fillIdentifyCtrl(d.region(kIdCtrlOff), st.cntlid);
            src = d.region(kIdCtrlOff);
        } else if (cns == NVMEOF_ID_CNS_NS) {
            fillIdentifyNs(d.region(kIdNsOff));
            src = d.region(kIdNsOff);
        } else if (cns == NVMEOF_ID_CNS_NS_ACTIVE_LIST) {
            // Active Namespace ID list: a 4-byte count followed by the ids.  The
            // host reads this to find out which namespaces to ask about at all;
            // without it the scan stops here and the controller looks empty.
            fillIdentifyNsList(d.region(kIdNsListOff));
            src = d.region(kIdNsListOff);
        } else if (cns == NVMEOF_ID_CNS_NS_DESC_LIST) {
            fillIdentifyNsDescs(d.region(kIdNsDescOff));
            src = d.region(kIdNsDescOff);
        } else {
            printf("        unsupported CNS\n");
            sc = NVMEOF_SC_INVALID_FIELD;
        }
        if (src) {
            if (!q.write(src, NVMEOF_IDENTIFY_SIZE, sgl.addr, sgl.key, (void*)(CTX_DATA + 1))) {
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                ND2_RESULT r = {};
                if (!ndOk(q.reap((void*)(CTX_DATA + 1), &r, kWaitMs))) sc = NVMEOF_SC_DATA_XFER_ERROR;
            }
        }
    } else {
        printf("  [admin] unsupported opcode=0x%02X\n", opcode);
        sc = NVMEOF_SC_INVALID_OPCODE;
    }

    buildCompletion(d.region(kRespOutOff), cid, st.sqhd(), result, sct, sc);
    // Re-arm the admin Receive BEFORE the response goes out.  The host is
    // entitled to send its next command the moment it sees this completion, and
    // a command that arrives with no Receive posted is not retried - it never
    // completes at all.  Every handler in this file answers in that order now.
    // The caller must therefore NOT re-arm again after this returns.
    q.postReceive(d.region(kCapInOff), 64, (void*)CTX_CAP);
    return q.send(d.region(kRespOutOff), 16, (void*)CTX_RESP);
}

// One I/O queue pair: its own QP, CQ and connector, established by a fabrics
// Connect on that queue pair and carrying no Create SQ/CQ of any kind.
struct IoQueue {
    Queue    q;
    bool     connected = false;
    bool     pending = false;      // a connection request is outstanding
    bool     dead = false;         // its Receive came back ND_CANCELED
    uint16_t qid = 0;
    int      commands = 0;

    // Ask the listener for a connection WITHOUT waiting for one.
    //
    // A target that blocks in GetConnectionRequest stops answering the admin
    // queue, and a host that is still doing admin work - Get Features, Keep
    // Alive, Identify - then sees every one of those commands time out.  It is
    // exactly what a real host does between Set Features: Number of Queues and
    // connecting its first I/O queue, and it is what broke this test first.
    bool requestConnection(IND2Listener* listener) {
        HRESULT hr = listener->GetConnectionRequest(q.conn, &q.ov);
        pending = (hr == ND_PENDING) || SUCCEEDED(hr);
        return pending;
    }

    // Finish the handshake if the host has connected.  Returns true once done.
    bool pollConnection(Device& dev) {
        if (!pending) return false;
        HRESULT hr = q.pollOverlapped(q.conn);
        if (hr == ND_PENDING) return false;
        pending = false;
        if (!ndOk(hr)) { printf("  [conn] io GetConnectionRequest failed\n"); return true; }

        uint8_t buf[512] = {};
        nvmeof_rdma_request_pd req = {};
        ULONG len = sizeof(buf);
        hr = q.conn->GetPrivateData(buf, &len);
        memcpy(&req, buf, sizeof(req));
        printf("  [conn] io request private data: hr=0x%08X len=%u qid=%u hsqsize=%u\n",
               (unsigned)hr, len, req.qid, req.hsqsize);
        if (FAILED(hr) || req.qid == 0 || req.qid > kMaxIoQueues) {
            printf("  [conn] rejecting io connect: qid %u unusable\n", req.qid);
            nvmeof_rdma_reject_pd rej = {};
            rej.recfmt = 0; rej.sts = NVMEOF_RDMA_ERROR_INVALID_QID;
            q.conn->Reject(&rej, sizeof(rej));
            return true;
        }
        qid = req.qid;

        // Arm the Receive before Accept: Accept releases the host's
        // CompleteConnect, so a Receive posted afterwards is racing a peer that
        // is already entitled to send.
        if (!q.postReceive(dev.region(kCapInOff + 64), 64, (void*)CTX_CONN)) return false;

        nvmeof_rdma_accept_pd acc = {};
        acc.recfmt = 0; acc.crqsize = (uint16_t)(kSqSize + 1);
        hr = q.conn->Accept(q.qp, kReadLimit, kReadLimit, &acc, sizeof(acc), &q.ov);
        if (hr == ND_PENDING) hr = q.waitOverlapped(q.conn, kWaitMs);
        if (!ndOk(hr)) { printf("  [conn] io Accept failed\n"); return true; }
        connected = true;
        return true;
    }

    // The fabrics Connect that names this queue, then I/O commands.
    bool serve(Device& dev, TargetState& st) {
        // Poll once, dispatching on the completion context rather than on "the
        // one completion I was expecting": a Send completion for a response this
        // queue posted earlier is a normal thing to find here, and treating it
        // as an unexpected completion would throw away the poll that had the
        // capsule in it.
        ND2_RESULT r = {};
        bool gotCapsule = false;
        for (int spins = 0; spins < 8; spins++) {
            if (!q.poll(&r)) break;
            if (r.RequestContext == (void*)CTX_RESP) continue;      // our own send
            if (r.RequestContext != (void*)CTX_CONN) {
                printf("  [io] unexpected completion ctx=%p type=%d st=0x%08X bytes=%u\n",
                       r.RequestContext, (int)r.RequestType, (unsigned)r.Status,
                       r.BytesTransferred);
                continue;
            }
            if (!ndOk(r.Status) || r.BytesTransferred != 64) {
                // A Receive that delivered no capsule.  ND_CANCELED is terminal
                // for the queue pair: every Receive posted afterwards completes
                // ND_CANCELED immediately too, so re-arming here would spin, not
                // recover.  Report it and stop serving this queue pair.
                printf("  [io] Receive did not deliver a capsule (st=0x%08X %s bytes=%u); "
                       "queue pair declared dead\n",
                       (unsigned)r.Status, r.Status == ND_CANCELED ? "ND_CANCELED" : "short",
                       r.BytesTransferred);
                dead = true;
                return false;
            }
            gotCapsule = true;
            break;
        }
        if (!gotCapsule) return false;
        const uint8_t* cap = dev.region(kCapInOff + 64);
        uint8_t  op = cap[0];
        uint16_t cid = nvmeof_rd16(cap + 2);
        uint8_t  sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
        uint64_t result = 0;

        if (op == NVMEOF_OPC_FABRICS) {
            uint8_t fctype = cap[4];
            uint16_t capQid = nvmeof_rd16(cap + 42);
            printf("  [io] fabrics Connect qid=%u sqsize=%u\n", capQid, nvmeof_rd16(cap + 44));
            // The Connect must name the queue it arrived on: a Connect for qid 2
            // turning up on qid 1's queue pair means the host and the target
            // disagree about which queue pair is which, and every later command
            // would be answered on the wrong queue.
            if (fctype != NVMEOF_FCTYPE_CONNECT || capQid != qid) {
                printf("        expected qid %u on this queue pair\n", qid);
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
            } else if (st.qidConnected[qid & 1] && qid < 2) {
                printf("        qid %u is already connected\n", qid);
                sct = NVMEOF_SCT_GENERIC; sc = NVMEOF_SC_CMD_SEQ_ERROR;
            } else if (nvmeof_rd16(cap + 40) != 0) {
                printf("        connect recfmt must be 0\n");
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_FORMAT;
            } else {
                if (qid < 2) st.qidConnected[qid] = true;
                if (cap[46] & NVMEOF_CONNECT_CATTR_DISABLE_SQFLOW) st.sqhdDisabled = true;
            }
        } else if (op == NVMEOF_OPC_KEEP_ALIVE) {
            // Admin command on an I/O queue: refused.  Dispatching on the
            // opcode alone instead of on the queue is the bug this pins.
            printf("  [io] Keep Alive on an I/O queue -> Invalid Opcode\n");
            sc = NVMEOF_SC_INVALID_OPCODE;
        } else if (op == NVMEOF_OPC_FLUSH) {
            // Flush is in the NVM command set and shares opcode 0x00 with the
            // admin queue's Delete SQ: only the queue tells them apart, which is
            // the dispatch rule this suite exists to pin.  Nothing to do - the
            // namespace is memory - but Invalid Opcode here would turn a host's
            // fsync into an I/O error.
            printf("  [io] qid=%u FLUSH nsid=%u\n", this->qid, nvmeof_rd32(cap + 4));
            commands++;
        } else if (op == NVMEOF_OPC_WRITE || op == NVMEOF_OPC_READ) {
            uint64_t slba = nvmeof_rd64(cap + 40);
            uint32_t blocks = (uint32_t)(nvmeof_rd32(cap + 48) & 0xFFFF) + 1;
            uint32_t bytes = blocks * kBlockSize;
            Sgl sgl = parseSgl(cap);
            commands++;
            printf("  [io] qid=%u %s slba=%llu blocks=%u\n", this->qid,
                   op == NVMEOF_OPC_WRITE ? "WRITE" : "READ",
                   (unsigned long long)slba, blocks);
            if (slba + blocks > kNsBlocks) {
                sc = NVMEOF_SC_LBA_RANGE;
            } else if (!sgl.present || sgl.len < bytes) {
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                uint8_t* ns = dev.region(kNsOff + (size_t)slba * kBlockSize);
                bool ok = (op == NVMEOF_OPC_WRITE)
                              ? q.read(ns, bytes, sgl.addr, sgl.key, (void*)CTX_XFER)
                              : q.write(ns, bytes, sgl.addr, sgl.key, (void*)CTX_XFER);
                if (!ok) { sc = NVMEOF_SC_DATA_XFER_ERROR; }
                else {
                    ND2_RESULT xr = {};
                    if (!ndOk(q.reap((void*)CTX_XFER, &xr, kWaitMs))) sc = NVMEOF_SC_DATA_XFER_ERROR;
                }
            }
        } else {
            printf("  [io] unsupported opcode=0x%02X\n", op);
            sc = NVMEOF_SC_INVALID_OPCODE;
        }
        buildCompletion(dev.region(kRespOutOff + 64), cid, st.sqhd(), result, sct, sc);
        // Re-arm BEFORE answering.  The host may send its next command the
        // instant it sees this response; a command that arrives with no Receive
        // posted is not retried, it simply never completes, and the host reports
        // a response it never got.  The handler above has already consumed the
        // capsule, so the buffer can be re-armed here safely.
        q.postReceive(dev.region(kCapInOff + 64), 64, (void*)CTX_CONN);
        q.send(dev.region(kRespOutOff + 64), 16, (void*)CTX_RESP);
        return true;
    }
};

static int runTarget(const char* ip, uint16_t port) {
    printf("=== F6 TARGET on %s:%u\n", ip, port);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, ip, &local.sin_addr);

    Device dev;
    if (!dev.open(local, kRegionBytes)) return 1;
    Queue admin;
    if (!admin.create(&dev, 0)) return 1;
    static IoQueue io[kMaxIoQueues];
    for (int i = 0; i < kMaxIoQueues; i++) if (!io[i].q.create(&dev, (uint16_t)(i + 1))) return 1;

    IND2Listener* listener = nullptr;
    HRESULT hr = dev.adapter->CreateListener(IID_IND2Listener, dev.ovFile, (VOID**)&listener);
    if (FAILED(hr)) { printf("CreateListener failed\n"); return 1; }
    hr = listener->Bind((const sockaddr*)&local, sizeof(local));
    if (!ndOk(hr)) { char b[64]; printf("listener Bind %s\n", ndStr(hr, b, sizeof(b))); return 1; }
    listener->Listen(4);

    // ---- admin connection, with the private-data handshake ----
    bool accepted = false;
    for (int attempt = 0; attempt < 4 && !accepted; attempt++) {
        hr = listener->GetConnectionRequest(admin.conn, &admin.ov);
        if (hr == ND_PENDING) hr = admin.waitOverlapped(listener, 20000);
        if (!ndOk(hr)) { printf("GetConnectionRequest failed\n"); return 1; }

        // GetPrivateData is SYNCHRONOUS - no OVERLAPPED - and only valid after
        // the request arrives and before Accept.  The buffer is far larger than
        // the 32-byte record because given exactly 32 bytes the provider returns
        // ND_BUFFER_OVERFLOW while filling all 32 bytes correctly.
        uint8_t pdbuf[512] = {};
        nvmeof_rdma_request_pd req = {};
        ULONG pdLen = sizeof(pdbuf);
        hr = admin.conn->GetPrivateData(pdbuf, &pdLen);
        memcpy(&req, pdbuf, sizeof(req));
        printf("  [conn] request private data: hr=0x%08X len=%u recfmt=%u qid=%u "
               "hrqsize=%u hsqsize=%u cntlid=0x%04X\n",
               (unsigned)hr, pdLen, req.recfmt, req.qid, req.hrqsize, req.hsqsize, req.cntlid);

        const char* why = nullptr;
        if (FAILED(hr))                                   why = "GetPrivateData failed";
        else if (pdLen < NVMEOF_RDMA_PRIVATE_DATA_SIZE)   why = "private data shorter than a record";
        else if (req.recfmt != 0)                         why = "unsupported recfmt";
        else if (req.qid != 0)                            why = "admin connect must carry qid 0";

        if (why) {
            printf("  [conn] rejecting: %s\n", why);
            nvmeof_rdma_reject_pd rej = {};
            rej.recfmt = 0;
            rej.sts = (uint16_t)((FAILED(hr) || pdLen < NVMEOF_RDMA_PRIVATE_DATA_SIZE)
                                     ? NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH
                                     : NVMEOF_RDMA_ERROR_INVALID_RECFMT);
            admin.conn->Reject(&rej, sizeof(rej));
            // A connector is spent once it has rejected; the listener will not
            // hand it another request.
            if (!admin.newConnector()) return 1;
            continue;
        }

        if (!admin.postReceive(dev.region(kCapInOff), 64, (void*)CTX_CAP)) {
            printf("  FATAL: could not post the initial admin Receive\n");
            return 1;
        }
        nvmeof_rdma_accept_pd acc = {};
        acc.recfmt = 0; acc.crqsize = (uint16_t)(kSqSize + 1);
        hr = admin.conn->Accept(admin.qp, kReadLimit, kReadLimit, &acc, sizeof(acc), &admin.ov);
        if (hr == ND_PENDING) hr = admin.waitOverlapped(admin.conn, kWaitMs);
        if (!ndOk(hr)) { char b[64]; printf("admin Accept %s\n", ndStr(hr, b, sizeof(b))); return 1; }
        accepted = true;
        printf("  [conn] accepted; advertised crqsize=%u\n", acc.crqsize);
    }
    if (!accepted) { printf("no acceptable admin connection arrived\n"); return 1; }

    TargetState st;
    int ioAccepted = 0, ioConnected = 0;
    // A Receive that comes back ND_CANCELED is terminal for its queue pair - see
    // the note in IoQueue::serve - so a cancelled admin Receive is recorded
    // rather than retried.
    bool adminDead = false;

    for (;;) {
        // ---- I/O queue pairs: one at a time, and never blocking ----
        // A listener connection request cancels a Receive pending on a DIFFERENT
        // queue pair, so the handshake is still done strictly in sequence:
        // request, accept, consume that queue's Connect capsule, then the next.
        // What changed is that waiting for the request no longer stops the world.
        if (ioAccepted < st.ioQueuesGranted && !io[ioAccepted].pending &&
            !io[ioAccepted].connected) {
            io[ioAccepted].requestConnection(listener);
            // Issuing the request may have cancelled the admin Receive; re-arm it
            // so the host's next admin command still lands.
            admin.postReceive(dev.region(kCapInOff), 64, (void*)CTX_CAP);
        }
        if (ioAccepted < st.ioQueuesGranted && io[ioAccepted].pending) {
            if (io[ioAccepted].pollConnection(dev)) {
                if (!admin.postReceive(dev.region(kCapInOff), 64, (void*)CTX_CAP)) {
                    printf("  FATAL: could not re-arm the admin Receive\n");
                    return 1;
                }
                ioAccepted++;
            }
        }
        for (int i = 0; i < ioAccepted; i++) {
            if (io[i].connected && !io[i].dead) {
                if (io[i].serve(dev, st)) { ioConnected++; }
            }
        }

        ND2_RESULT r = {};
        if (!adminDead && admin.poll(&r)) {
            uintptr_t ctx = (uintptr_t)r.RequestContext;
            if (ctx == CTX_CAP && ndOk(r.Status) && r.BytesTransferred == 64) {
                if (!targetAdmin(admin, dev, st)) break;
                if (st.disconnected) {
                    int spins = 0;
                    while (spins++ < 400) {
                        ND2_RESULT sr = {};
                        if (admin.poll(&sr)) break;
                        SwitchToThread();
                    }
                    printf("  [admin] controller disconnected; tearing down\n");
                    break;
                }
                // No re-arm here: targetAdmin arms the next Receive before it
                // sends its response, which is where the ordering has to hold.
            } else if (ctx == CTX_CAP) {
                // A Receive that delivered no capsule.  ND_CANCELED is terminal
                // for the queue pair - every later Receive completes ND_CANCELED
                // immediately too - so re-arming in a loop is not recovery, it is
                // a spin.  Declare it dead; a command that still needed the queue
                // pair will then fail as the timeout it is, rather than being
                // hidden behind an unbounded retry.
                printf("  [admin] Receive did not deliver a capsule (st=0x%08X %s bytes=%u); "
                       "queue pair declared dead\n",
                       (unsigned)r.Status, r.Status == ND_CANCELED ? "ND_CANCELED" : "short",
                       r.BytesTransferred);
                adminDead = true;
            }
        }
        SwitchToThread();
    }

    int totalCmds = 0;
    for (int i = 0; i < kMaxIoQueues; i++) totalCmds += io[i].commands;
    printf("target summary: ioQueuesGranted=%d ioQueuesEstablished=%d ioCommands=%d "
           "keepAlives=%d katoMs=%d disconnected=%d\n",
           st.ioQueuesGranted, ioAccepted, totalCmds, st.keepAlives, st.katoMs,
           (int)st.disconnected);
    // Every I/O queue must have carried traffic, or "two queues" is a claim about
    // a loop bound rather than about the protocol.
    if (st.ioQueuesGranted >= 2) {
        for (int i = 0; i < st.ioQueuesGranted && i < kMaxIoQueues; i++) {
            printf("  io queue qid=%u commands=%d\n", io[i].qid, io[i].commands);
        }
    }
    for (int i = 0; i < kMaxIoQueues; i++) io[i].q.destroy();
    admin.destroy();
    dev.close(nullptr, nullptr, 0);
    printf("target failures: %d\n", g_failures);
    return g_failures;
}

// ---------------------------------------------------------------------------
//  Initiator - the sequence a Linux host performs, in that order.
// ---------------------------------------------------------------------------
static bool submit(Queue& q, Device& d, uint8_t* cap, Completion* out, DWORD ms) {
    memcpy(d.region(kCapInOff), cap, 64);
    memset(d.region(kRespInOff), 0xFF, 16);
    if (!q.postReceive(d.region(kRespInOff), 16, (void*)CTX_RESP)) return false;
    if (!q.send(d.region(kCapInOff), 64, (void*)CTX_CAP)) return false;

    // Drain both completions without dropping either.  reap(expect) silently
    // DISCARDS a completion whose context does not match, so waiting for the
    // Send and then the Receive in that fixed order throws away the response
    // whenever the Receive completes first - and it routinely does.  The symptom
    // is an alternating failure, not an obvious one.
    bool gotSend = false, gotResp = false;
    ULONGLONG t0 = GetTickCount64();
    while (!gotSend || !gotResp) {
        ND2_RESULT r = {};
        if (q.poll(&r)) {
            uintptr_t ctx = (uintptr_t)r.RequestContext;
            if (ctx == CTX_CAP && ndOk(r.Status)) {
                gotSend = true;
            } else if (ctx == CTX_RESP && ndOk(r.Status) && r.BytesTransferred == 16) {
                gotResp = true;
            } else {
                printf("    submit: completion ctx=%p st=0x%08X bytes=%u\n",
                       r.RequestContext, (unsigned)r.Status, r.BytesTransferred);
                return false;
            }
            continue;
        }
        if (GetTickCount64() - t0 >= ms) return false;
        SwitchToThread();
    }
    const uint8_t* cqe = d.region(kRespInOff);
    out->cid = nvmeof_rd16(cqe + 12);
    out->status = nvmeof_rd16(cqe + 14);
    out->result = nvmeof_rd64(cqe + 0);
    return true;
}

static void fabricHeader(uint8_t* cap, uint8_t opcode, uint16_t cid, uint8_t fctype) {
    memset(cap, 0, 64);
    cap[0] = opcode;
    nvmeof_wr16(cap + 2, cid);
    cap[4] = fctype;
}

// Connect data blob, written once and pointed at by every Connect.
static uint8_t* connectData(Device& d) {
    uint8_t* cd = d.region(kConnectOff);
    memset(cd, 0, NVMEOF_CONNECT_DATA_SIZE);
    nvmeof_wr16(cd + 16, NVMEOF_CNTLID_DYNAMIC);
    memcpy(cd + 256, kSubNqn, strlen(kSubNqn));
    memcpy(cd + 512, kHostNqn, strlen(kHostNqn));
    return cd;
}

static void connectCmd(uint8_t* cap, Device& d, uint16_t cid, uint16_t qid, uint16_t sqsize) {
    fabricHeader(cap, NVMEOF_OPC_FABRICS, cid, NVMEOF_FCTYPE_CONNECT);
    nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)connectData(d),
                         NVMEOF_CONNECT_DATA_SIZE, d.rkey);
    nvmeof_wr16(cap + 42, qid);
    nvmeof_wr16(cap + 44, sqsize);
    nvmeof_wr32(cap + 48, NVMEOF_KATO_DEFAULT);
}

static int runInitiator(const char* serverIp, uint16_t port, const char* localIp) {
    printf("=== F6 INITIATOR %s -> %s:%u\n", localIp, serverIp, port);
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
    Queue admin;
    if (!admin.create(&dev, 0)) return 1;
    static Queue io[kMaxIoQueues];
    for (int i = 0; i < kMaxIoQueues; i++) if (!io[i].create(&dev, (uint16_t)(i + 1))) return 1;

    nvmeof_rdma_request_pd req;
    // Filled the way the Linux host fills it, including the asymmetric queue
    // sizes: hsqsize is 0-based (depth - 1).  Sending depth here makes a real
    // target refuse the admin queue with NVME_RDMA_CM_INVALID_HSQSIZE.
    nvmeof_rdma_fill_req(&req, 0, (uint16_t)(kSqSize + 1), 0);

    // ---- (0) a Connect with a bad recfmt must be refused ----
    {
        char b[64];
        Queue probe;
        if (!probe.create(&dev, 9)) return 1;
        sockaddr_in probeLocal = local;
        probeLocal.sin_port = htons((u_short)(port + 1));
        nvmeof_rdma_request_pd bad = req;
        bad.recfmt = 0xDEAD;
        HRESULT h = probe.conn->Bind((const sockaddr*)&probeLocal, sizeof(probeLocal));
        h = probe.conn->Connect(probe.qp, (const sockaddr*)&remote, sizeof(remote),
                                kReadLimit, kReadLimit, &bad, sizeof(bad), &probe.ov);
        if (h == ND_PENDING) h = probe.waitOverlapped(probe.conn, 8000);
        if (ndOk(h)) {
            h = probe.conn->CompleteConnect(&probe.ov);
            if (h == ND_PENDING) h = probe.waitOverlapped(probe.conn, 3000);
        }
        Report("a Connect with recfmt=0xDEAD is refused, not accepted",
               !ndOk(h), ndStr(h, b, sizeof(b)));
        probe.destroy();
    }

    // ---- (1) admin queue pair, with the private-data handshake ----
    {
        char b[64];
        HRESULT h = admin.conn->Bind((const sockaddr*)&local, sizeof(local));
        h = admin.conn->Connect(admin.qp, (const sockaddr*)&remote, sizeof(remote),
                                kReadLimit, kReadLimit, &req, sizeof(req), &admin.ov);
        if (h == ND_PENDING) h = admin.waitOverlapped(admin.conn, 20000);
        if (!ndOk(h)) { printf("admin Connect %s\n", ndStr(h, b, sizeof(b))); return 1; }

        uint8_t abuf[512] = {};
        nvmeof_rdma_accept_pd acc = {};
        ULONG alen = sizeof(abuf);
        HRESULT gh = admin.conn->GetPrivateData(abuf, &alen);
        memcpy(&acc, abuf, sizeof(acc));
        printf("  [conn] accept private data: hr=0x%08X len=%u recfmt=%u crqsize=%u\n",
               (unsigned)gh, alen, acc.recfmt, acc.crqsize);
        char d[128];
        sprintf_s(d, "len=%u recfmt=%u crqsize=%u", alen, acc.recfmt, acc.crqsize);
        Report("the controller's Accept private data is a valid record",
               ndOk(gh) && alen >= NVMEOF_RDMA_PRIVATE_DATA_SIZE && acc.recfmt == 0 &&
                   acc.crqsize > 0, d);

        h = admin.conn->CompleteConnect(&admin.ov);
        if (h == ND_PENDING) h = admin.waitOverlapped(admin.conn, kWaitMs);
        if (!ndOk(h)) { printf("admin CompleteConnect failed\n"); return 1; }
    }

    uint8_t cap[64];
    Completion c = {};
    uint16_t cid = 0;
    auto nextCid = [&]() { return ++cid; };

    // ---- (2) Connect qid 0: the admin queue ----
    connectCmd(cap, dev, nextCid(), 0, (uint16_t)kSqSize);
    uint16_t adminCntlid = 0;
    uint64_t capValue = 0;
    {
        bool ok = submit(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        adminCntlid = (uint16_t)c.result;
        char d[64];
        sprintf_s(d, "cntlid=%u", adminCntlid);
        Report("Connect qid=0 establishes the admin queue", ok && adminCntlid != 0, d);
    }

    // ---- (3) Set Features: Number of Queues (mandatory before I/O queues) ----
    int granted = 0;
    {
        fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, nextCid(), 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_NUM_QUEUES);
        nvmeof_wr32(cap + 44, ((uint32_t)(kMaxIoQueues - 1) << 16) |
                              (uint32_t)(kMaxIoQueues - 1));
        bool sent = submit(admin, dev, cap, &c, kWaitMs);
        granted = (int)(c.result & 0xffff) + 1;
        char d[96];
        sprintf_s(d, "asked for %d, controller granted %d", kMaxIoQueues, granted);
        Report("Set Features: Number of Queues is accepted", sent && statusOk(c.status), d);
    }
    {
        fabricHeader(cap, NVMEOF_OPC_GET_FEATURES, nextCid(), 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_NUM_QUEUES);
        bool sent = submit(admin, dev, cap, &c, kWaitMs);
        Report("Get Features: Number of Queues agrees with the Set",
               sent && statusOk(c.status) &&
                   ((int)(c.result & 0xffff) + 1) == granted, nullptr);
    }

    // ---- (4) enable the controller ----
    fabricHeader(cap, NVMEOF_OPC_FABRICS, nextCid(), NVMEOF_FCTYPE_PROPERTY_SET);
    nvmeof_wr32(cap + 44, NVMEOF_PROP_CC);
    nvmeof_wr64(cap + 48, 1);
    submit(admin, dev, cap, &c, kWaitMs);

    fabricHeader(cap, NVMEOF_OPC_FABRICS, nextCid(), NVMEOF_FCTYPE_PROPERTY_GET);
    nvmeof_wr32(cap + 44, NVMEOF_PROP_CSTS);
    submit(admin, dev, cap, &c, kWaitMs);

    // ---- (5) Set Features: Keep Alive Timer ----
    {
        fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, nextCid(), 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_KATO);
        nvmeof_wr32(cap + 44, NVMEOF_KATO_DEFAULT);
        bool sent = submit(admin, dev, cap, &c, kWaitMs);
        char d[96];
        sprintf_s(d, "controller uses %llu s granularity", (unsigned long long)c.result);
        Report("Set Features: Keep Alive Timer is accepted", sent && statusOk(c.status), d);
    }

    // ---- (6) Keep Alive, before enrolling in I/O queues ----
    {
        fabricHeader(cap, NVMEOF_OPC_KEEP_ALIVE, nextCid(), 0);
        bool sent = submit(admin, dev, cap, &c, kWaitMs);
        Report("Keep Alive is accepted once KATO is set", sent && statusOk(c.status), nullptr);
    }

    // ---- (7) the namespace scan, in the order a host performs it ----
    // Identify Controller, then the Active Namespace ID list, then per-id the
    // namespace structure and its identification descriptors.  A controller that
    // answers only CNS=0/1 gets as far as Identify Controller and then looks
    // empty, so this sequence is the difference between "connects" and "usable".
    uint32_t blockSize = kBlockSize;
    {
        memset(dev.region(kIdCtrlOff), 0xCC, NVMEOF_IDENTIFY_SIZE);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, nextCid(), 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24),
                             (uint64_t)(uintptr_t)dev.region(kIdCtrlOff),
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        cap[40] = NVMEOF_ID_CNS_CTRL;
        bool ok = submit(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        const uint8_t* id = dev.region(kIdCtrlOff);
        uint8_t sqes = id[NVMEOF_ID_CTRL_OFF_SQES];
        uint8_t cqes = id[NVMEOF_ID_CTRL_OFF_CQES];
        uint16_t icdoff = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_ICDOFF);
        uint16_t kas = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_KAS);
        uint16_t maxcmd = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_MAXCMD);
        char sn[21] = {};
        memcpy(sn, id + NVMEOF_ID_CTRL_OFF_SN, 20);
        char d[192];
        sprintf_s(d, "sqes=0x%02X cqes=0x%02X icdoff=%u kas=%u maxcmd=%u sn='%s'",
                  sqes, cqes, icdoff, kas, maxcmd, sn);
        Report("Identify Controller reports fabrics-correct fields",
               ok && (sqes >> 4) == 6 && (sqes & 0xf) == 6 &&
                   (cqes >> 4) == 4 && (cqes & 0xf) == 4 &&
                   icdoff == 0 && kas != 0 && maxcmd != 0, d);
    }

    uint32_t nsid = 0;
    {
        memset(dev.region(kIdNsListOff), 0xCC, NVMEOF_IDENTIFY_SIZE);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, nextCid(), 0);
        nvmeof_wr32(cap + 4, 0);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24),
                             (uint64_t)(uintptr_t)dev.region(kIdNsListOff),
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        cap[40] = NVMEOF_ID_CNS_NS_ACTIVE_LIST;
        bool ok = submit(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        uint32_t n = nvmeof_rd32(dev.region(kIdNsListOff));
        nsid = nvmeof_rd32(dev.region(kIdNsListOff) + 4);
        char d[96];
        sprintf_s(d, "count=%u firstNsid=%u", n, nsid);
        Report("Identify Active Namespace ID list (CNS 2)", ok && n >= 1 && nsid >= 1, d);
    }
    {
        memset(dev.region(kIdNsDescOff), 0xCC, NVMEOF_IDENTIFY_SIZE);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, nextCid(), 0);
        nvmeof_wr32(cap + 4, nsid);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24),
                             (uint64_t)(uintptr_t)dev.region(kIdNsDescOff),
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        cap[40] = NVMEOF_ID_CNS_NS_DESC_LIST;
        bool ok = submit(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        const uint8_t* d0 = dev.region(kIdNsDescOff);
        char d[128];
        sprintf_s(d, "nidt=0x%02X nidl=%u csi=0x%02X", d0[0], d0[1], d0[4]);
        Report("Identify Namespace Identification Descriptors (CNS 3)",
               ok && d0[0] == NVMEOF_NIDT_CSI && d0[1] == 1 && d0[4] == NVMEOF_CSI_NVM, d);
    }
    {
        memset(dev.region(kIdNsOff), 0xCC, NVMEOF_IDENTIFY_SIZE);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, nextCid(), 0);
        nvmeof_wr32(cap + 4, nsid);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24),
                             (uint64_t)(uintptr_t)dev.region(kIdNsOff),
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        cap[40] = NVMEOF_ID_CNS_NS;
        bool ok = submit(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        if (ok) blockSize = 1u << dev.region(kIdNsOff)[NVMEOF_ID_NS_OFF_LBAF + 2];
        char d[96];
        sprintf_s(d, "nsze=%llu block=%u",
                  (unsigned long long)nvmeof_rd64(dev.region(kIdNsOff)), blockSize);
        Report("Identify Namespace geometry for the id the list returned", ok, d);
    }

    // ---- (7b) Property Get must answer per register, not one value for all ----
    {
        struct { uint32_t off; const char* what; } regs[] = {
            { NVMEOF_PROP_CAP,  "CAP"  },
            { NVMEOF_PROP_VS,   "VS"   },
            { NVMEOF_PROP_CC,   "CC"   },
            { NVMEOF_PROP_CSTS, "CSTS" },
        };
        uint64_t seen[4] = {};
        int good = 0, distinct = 0;
        for (int i = 0; i < 4; i++) {
            fabricHeader(cap, NVMEOF_OPC_FABRICS, nextCid(), NVMEOF_FCTYPE_PROPERTY_GET);
            nvmeof_wr32(cap + 44, regs[i].off);
            // CAP is 64-bit, so reading it is an 8-byte access; the others are
            // 4-byte.  A host respects this and a target is expected to check it.
            cap[40] = (regs[i].off == NVMEOF_PROP_CAP) ? NVMEOF_PROP_SIZE_8
                                                       : NVMEOF_PROP_SIZE_4;
            bool sent = submit(admin, dev, cap, &c, kWaitMs);
            seen[i] = c.result;
            if (sent && statusOk(c.status)) good++;
        }
        capValue = seen[0];
        for (int i = 0; i < 4; i++) {
            for (int j = i + 1; j < 4; j++) if (seen[i] != seen[j]) distinct++;
        }
        char d[160];
        sprintf_s(d, "CAP=0x%llX VS=0x%llX CC=%llu CSTS=%llu",
                  (unsigned long long)seen[0], (unsigned long long)seen[1],
                  (unsigned long long)seen[2], (unsigned long long)seen[3]);
        // CAP, VS and CSTS are different registers; a target that returns one
        // value for every offset fails this and passes everything else.
        Report("Property Get answers each register separately",
               good == 4 && (seen[0] & 0xffff) == NVMEOF_CAP_MQES && seen[1] != 0 &&
                   seen[3] == 1 && distinct >= 3, d);
    }

    // ---- (7c) the exact checklist a Linux host applies to a fabrics controller
    //
    // nvme_check_ctrl_fabric_info() is the gate between "connected" and "usable",
    // and it is a short, literal list.  Asserting each condition directly is
    // worth more than asserting the fields individually, because it is the same
    // predicate the kernel will evaluate - and a failure names which clause.
    {
        const uint8_t* id = dev.region(kIdCtrlOff);
        uint16_t idCntlid = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_CNTLID);
        uint16_t kas = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_KAS);
        uint32_t ioccsz = nvmeof_rd32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ);
        uint32_t iorcsz = nvmeof_rd32(id + NVMEOF_ID_CTRL_OFF_IORCSZ);
        uint16_t maxcmd = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_MAXCMD);

        char d[192];
        sprintf_s(d, "cntlid=%u vs %u, kas=%u, ioccsz=%u, iorcsz=%u, maxcmd=%u",
                  idCntlid, adminCntlid, kas, ioccsz, iorcsz, maxcmd);
        // 1. Identify's cntlid must match the one the admin Connect assigned.
        // 2. KAS must be non-zero: "keep-alive support is mandatory for fabrics".
        // 3. ioccsz >= 4 and 4. iorcsz >= 1, or the host refuses the capsule sizes.
        // 5. maxcmd must be non-zero (the host warns and guesses otherwise).
        Report("the host's fabric-info checklist passes on every clause",
               idCntlid == adminCntlid && kas != 0 && ioccsz >= 4 && iorcsz >= 1 &&
                   maxcmd != 0, d);
    }
    {
        // ---- (7d) the fields the host reads to decide what to SEND ----
        //
        // Each of these is a promise about the wire, and each is checked against
        // the value Linux's nvmet reports for the same situation:
        //   * ioccsz = 4 (64 bytes): one SQE, no in-capsule data.  It must equal
        //     the receive buffer size.  The host only ever puts data in the
        //     capsule when SGLS.SAOS is advertised (use_inline_data is set from
        //     that one bit), but a promise that disagrees with the buffer is a
        //     promise waiting for a host that reads it differently.
        //   * icdoff = 0, which is what nvmet leaves it at with no in-capsule data.
        //   * vwc = 0: no volatile write cache, because the namespace is memory.
        const uint8_t* id = dev.region(kIdCtrlOff);
        uint32_t ioccsz = nvmeof_rd32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ);
        uint16_t icdoff = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_ICDOFF);
        uint8_t  vwc    = id[NVMEOF_ID_CTRL_OFF_VWC];
        uint32_t sgls   = nvmeof_rd32(id + NVMEOF_ID_CTRL_OFF_SGLS);
        char d[192];
        sprintf_s(d, "ioccsz=%u (%u B) icdoff=%u vwc=%u sgls=0x%08X saos=%d",
                  ioccsz, ioccsz * 16, icdoff, vwc, sgls, (sgls & NVMEOF_CTRL_SGLS_SAOS) ? 1 : 0);
        Report("capsule/data fields describe what the receive path can actually take",
               ioccsz == 4 && icdoff == 0 && vwc == 0 && (sgls & NVMEOF_CTRL_SGLS_SAOS) == 0, d);
    }
    {
        // ---- (7f) KATO is answered in the reference's units ----
        //
        // Set Features answers in seconds; Get Features answers in milliseconds.
        // nvmet does exactly this (nvmet_set_feat_kato returns ctrl->kato,
        // nvmet_get_feat_kato returns ctrl->kato * 1000), so a target that
        // answers seconds to both understates the timeout by 1000x for any host
        // that asks.
        fabricHeader(cap, NVMEOF_OPC_GET_FEATURES, nextCid(), 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_KATO);
        bool sent = submit(admin, dev, cap, &c, kWaitMs);
        char d[160];
        sprintf_s(d, "Get Features KATO -> %llu (Set asked for %u ms)",
                  (unsigned long long)c.result, (unsigned)NVMEOF_KATO_DEFAULT);
        Report("Get Features: KATO answers in milliseconds, the reference's unit",
               sent && statusOk(c.status) && c.result == NVMEOF_KATO_DEFAULT, d);
    }
    {
        // CAP.TO is the ready-wait the host allows itself.  Zero is legal and
        // this target survives it, but only by having no jitter at all.
        char d[128];
        sprintf_s(d, "CAP=0x%llX TO=%llu (%.1f s)",
                  (unsigned long long)capValue, (unsigned long long)((capValue >> 24) & 0xff),
                  ((capValue >> 24) & 0xff) / 2.0);
        Report("CAP advertises a non-zero ready timeout and MQES",
               ((capValue >> 24) & 0xff) != 0 && (capValue & 0xffff) == NVMEOF_CAP_MQES, d);
    }
    {
        // The fields that are zero ON PURPOSE, and the reason each one matters.
        //
        // This is not a completeness check - it is the opposite.  Every one of
        // these zeros suppresses a command this implementation does not have, and
        // the host decides what to send by reading them.  Setting one "for
        // completeness" starts a conversation the target cannot finish, so the
        // detail names the suppressed command: whoever changes a value has to
        // argue with the reason, not just with a zero.
        const uint8_t* id = dev.region(kIdCtrlOff);
        uint32_t oaes   = nvmeof_rd32(id + NVMEOF_ID_CTRL_OFF_OAES);
        uint8_t  lpa    = id[NVMEOF_ID_CTRL_OFF_LPA];
        uint8_t  apsta  = id[NVMEOF_ID_CTRL_OFF_APSTA];
        uint8_t  cmic   = id[NVMEOF_ID_CTRL_OFF_CMIC];
        uint32_t ctratt = nvmeof_rd32(id + NVMEOF_ID_CTRL_OFF_CTRATT);
        uint16_t oacs   = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_OACS);
        uint16_t oncs   = nvmeof_rd16(id + NVMEOF_ID_CTRL_OFF_ONCS);
        char d[224];
        sprintf_s(d, "oaes=%u(no AEN cmd) lpa=%u(no effects log) apsta=%u(no APST) "
                     "ctratt=%u(no host-behaviour) cmic=%u(no ANA) oacs=%u oncs=%u",
                  oaes, lpa, apsta, ctratt, cmic, oacs, oncs);
        Report("capability fields stay zero so the host does not send what we cannot answer",
               (oaes & NVMEOF_OAES_AEN_SUPPORTED) == 0 &&
                   (lpa & NVMEOF_CTRL_LPA_CMD_EFFECTS_LOG) == 0 &&
                   apsta == 0 && ctratt == 0 && cmic == 0 && oacs == 0 && oncs == 0, d);
    }

    // ---- (9) one queue pair per I/O queue, each with its own Connect ----
    int ioUp = 0;
    for (int i = 0; i < granted && i < kMaxIoQueues; i++) {
        uint16_t qid = (uint16_t)(i + 1);
        char b[64];
        nvmeof_rdma_request_pd ireq;
        nvmeof_rdma_fill_req(&ireq, qid, (uint16_t)(kSqSize + 1), adminCntlid);
        HRESULT h = io[i].conn->Bind((const sockaddr*)&local, sizeof(local));
        h = io[i].conn->Connect(io[i].qp, (const sockaddr*)&remote, sizeof(remote),
                                kReadLimit, kReadLimit, &ireq, sizeof(ireq), &io[i].ov);
        if (h == ND_PENDING) h = io[i].waitOverlapped(io[i].conn, 20000);
        if (ndOk(h)) {
            h = io[i].conn->CompleteConnect(&io[i].ov);
            if (h == ND_PENDING) h = io[i].waitOverlapped(io[i].conn, kWaitMs);
        }
        if (!ndOk(h)) {
            char d[96];
            sprintf_s(d, "qid=%u queue pair %s", qid, ndStr(h, b, sizeof(b)));
            Report("I/O queue pair connected", 0, d);
            continue;
        }
        connectCmd(cap, dev, nextCid(), qid, (uint16_t)kSqSize);
        bool ok = submit(io[i], dev, cap, &c, kWaitMs) && statusOk(c.status);
        char d[64];
        sprintf_s(d, "qid=%u", qid);
        Report("fabrics Connect on the I/O queue's own queue pair", ok, d);
        if (ok) ioUp++;
    }
    {
        char d[96];
        sprintf_s(d, "%d of %d I/O queues established", ioUp, granted);
        Report("every granted I/O queue was established without Create SQ/CQ",
               granted >= 2 && ioUp == granted, d);
    }

    // ---- (10) I/O on each queue ----
    for (int i = 0; i < ioUp; i++) {
        const uint32_t blocks = 8;
        uint32_t slba = (uint32_t)(i * blocks);
        uint8_t* buf = dev.region(kXferOff);
        for (uint32_t k = 0; k < blocks * kBlockSize; k++) {
            buf[k] = (uint8_t)(k * 13u + 7u + i * 29u);
        }
        fabricHeader(cap, NVMEOF_OPC_WRITE, nextCid(), 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, slba);
        nvmeof_wr32(cap + 48, blocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)buf,
                             blocks * kBlockSize, dev.rkey);
        bool w = submit(io[i], dev, cap, &c, kWaitMs) && statusOk(c.status);

        memset(buf, 0, blocks * kBlockSize);
        fabricHeader(cap, NVMEOF_OPC_READ, nextCid(), 0);
        nvmeof_wr32(cap + 4, 1);
        nvmeof_wr64(cap + 40, slba);
        nvmeof_wr32(cap + 48, blocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)buf,
                             blocks * kBlockSize, dev.rkey);
        bool r = submit(io[i], dev, cap, &c, kWaitMs) && statusOk(c.status);

        int bad = 0;
        for (uint32_t k = 0; k < blocks * kBlockSize; k++) {
            if (buf[k] != (uint8_t)(k * 13u + 7u + i * 29u)) bad++;
        }
        char d[128];
        sprintf_s(d, "qid=%d write=%d read=%d bad=%d", i + 1, (int)w, (int)r, bad);
        Report("WRITE then READ round trips on this I/O queue", w && r && bad == 0, d);
    }

    // ---- (10b) Flush is answered even though no write cache is advertised ----
    //
    // Flush shares opcode 0x00 with the admin queue's Delete SQ and is only
    // distinguishable by the queue it arrives on - the same queue-dispatch rule
    // as WRITE vs Create SQ.  A host may send it for reasons of its own (fsync,
    // unmount) whatever VWC says, and answering Invalid Opcode would turn that
    // into an I/O error.  Posted here, after the I/O queues are live: sending it
    // earlier fails the transport and proves nothing.
    {
        fabricHeader(cap, NVMEOF_OPC_FLUSH, nextCid(), 0);
        nvmeof_wr32(cap + 4, 1);
        bool sent = submit(io[0], dev, cap, &c, kWaitMs);
        char d[128];
        sprintf_s(d, "status=0x%04X", c.status);
        Report("Flush on a live I/O queue is answered, not rejected as an admin opcode",
               sent && statusOk(c.status), d);
    }

    // ---- (11) Create/Delete I/O SQ and CQ must be refused ----
    // This is the assertion that keeps the queue model honest.  If the target
    // ever starts accepting these again it will pass every test in this file
    // right up until a Linux host refuses to talk to it.
    {
        struct { uint8_t op; const char* what; } bad4[] = {
            { NVMEOF_OPC_CREATE_SQ, "Create I/O SQ" },
            { NVMEOF_OPC_CREATE_CQ, "Create I/O CQ" },
            { NVMEOF_OPC_DELETE_SQ, "Delete I/O SQ" },
            { NVMEOF_OPC_DELETE_CQ, "Delete I/O CQ" },
        };
        int refused = 0;
        for (int i = 0; i < 4; i++) {
            fabricHeader(cap, bad4[i].op, nextCid(), 0);
            nvmeof_wr32(cap + 40, (1u << 16) | (uint32_t)kSqSize);
            nvmeof_wr32(cap + 44, 1u);
            bool sent = submit(admin, dev, cap, &c, kWaitMs);
            bool isInvalidOpcode = sent &&
                nvmeof_status_sct(c.status) == NVMEOF_SCT_GENERIC &&
                nvmeof_status_sc(c.status) == NVMEOF_SC_INVALID_OPCODE;
            if (isInvalidOpcode) refused++;
            char d[96];
            sprintf_s(d, "sct=%u sc=0x%02X", nvmeof_status_sct(c.status),
                      nvmeof_status_sc(c.status));
            Report(bad4[i].what, isInvalidOpcode, d);
        }
        char d[96];
        sprintf_s(d, "%d of 4 refused", refused);
        Report("all four PCI-only queue commands are refused", refused == 4, d);
    }

    // ---- (12) leaving: the controller is disabled, then the queue pairs go ----
    //
    // There is no fabrics Disconnect command.  This step used to send fctype 0x08
    // - a value no NVMe header defines - and the target here accepted it, so both
    // ends agreed on a command the world answers with Invalid Opcode (DESIGN 8.44).
    // A host really leaves the way the Linux host does: Property Set CC with
    // EN = 0, and then the RDMA connections disappear.
    {
        // The probe for the fabricated value comes first: a target that has just
        // been told CC.EN=0 may legitimately stop answering, and then this
        // assertion would be measuring a target that is not there.  (That is
        // exactly how this assertion failed the first time, in F5.)
        fabricHeader(cap, NVMEOF_OPC_FABRICS, nextCid(), 0x08);
        bool sent = submit(admin, dev, cap, &c, kWaitMs);
        char d[96];
        sprintf_s(d, "sct=%u sc=0x%02X", nvmeof_status_sct(c.status),
                  nvmeof_status_sc(c.status));
        Report("fctype 0x08 does not exist and is refused as an invalid opcode",
               sent && !statusOk(c.status) &&
                   nvmeof_status_sc(c.status) == NVMEOF_SC_INVALID_OPCODE, d);
    }
    {
        fabricHeader(cap, NVMEOF_OPC_FABRICS, nextCid(), NVMEOF_FCTYPE_PROPERTY_SET);
        nvmeof_wr32(cap + 44, NVMEOF_PROP_CC);
        nvmeof_wr64(cap + 48, 0);                    // EN = 0
        bool sent = submit(admin, dev, cap, &c, kWaitMs);
        char d[96];
        sprintf_s(d, "sct=%u sc=0x%02X", nvmeof_status_sct(c.status),
                  nvmeof_status_sc(c.status));
        Report("Property Set CC.EN=0 disables the controller (a host's real exit)",
               sent && statusOk(c.status), d);
    }
    {
        ULONGLONG t0 = GetTickCount64();
        fabricHeader(cap, NVMEOF_OPC_KEEP_ALIVE, nextCid(), 0);
        bool sent = submit(admin, dev, cap, &c, 2000);
        ULONGLONG ms = GetTickCount64() - t0;
        char d[128];
        sprintf_s(d, "sent=%d after %llu ms", (int)sent, (unsigned long long)ms);
        Report("after the controller is disabled the association is gone",
               !sent || !statusOk(c.status), d);
        Report("the failed Keep Alive failed promptly, it did not hang", ms < 6000, d);
    }

    for (int i = 0; i < kMaxIoQueues; i++) io[i].destroy();
    admin.destroy();
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
