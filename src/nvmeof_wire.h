// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
// nvmeof_wire.h - NVMe over Fabrics wire structures, for a from-scratch
// Windows implementation on top of NetworkDirect (NDSPI).
//
// SOURCES (both fetched and cross-checked, not recalled):
//   * Linux  include/linux/nvme.h  (torvalds/linux, master) - command, completion,
//     fabrics command, connect data, SGL descriptor, opcodes, status codes.
//   * SPDK   spdk_nvme_sgl_descriptor (spdk.io doc for nvme_spec.h) - resolves the
//     one thing Linux's header alone leaves ambiguous: the SGL descriptor is
//     16 bytes in *every* variant, including the keyed one.
//
// WHY THE static_asserts BELOW MATTER
// -----------------------------------
// Nothing here is laid out from arithmetic done by hand.  Every offset that the
// protocol cares about is asserted against its published value, so a wrong
// padding byte is a compile error instead of a wire-format bug that only shows
// up as an unhelpful "Invalid Field in Command" from a real target.  The anchors
// (sqes @512, nn @516, subnqn @768, ioccsz @1792, sizeof 4096) are from the NVMe
// base spec's Identify Controller figure.
//
// WHAT IS DELIBERATELY *NOT* HARDCODED
// ------------------------------------
// Capsule sizes are NOT constants in this header.  ioccsz / iorcsz / icdoff are
// read from the target's Identify Controller data at connect time, because that
// is what the protocol says to do:
//   ioccsz  - I/O Command Capsule Size,   in 16-byte units  (offset 1792)
//   iorcsz  - I/O Response Capsule Size,  in 16-byte units  (offset 1796)
//   icdoff  - In-Capsule Data Offset,     in 16-byte units  (offset 1800)
// Baking in "128 / 16 / 32" would work against the one target it was measured
// against and silently break against any other, including our own if we ever
// change it.

#ifndef NVMEOF_WIRE_H
#define NVMEOF_WIRE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>   // memset, for the private-data helper below

// MSVC needs the un-underscored spelling in C (and it must be compiled with
// /std:c11 or later); every other C compiler wants _Static_assert.  Getting
// this wrong is silent in the worst way - the asserts simply stop existing and
// the header becomes documentation - so both spellings are pinned here and the
// self-test is built in both C and C++ to prove they actually fire.
#if defined(__cplusplus)
  #define NVMEOF_STATIC_ASSERT(c, m) static_assert(c, m)
#elif defined(_MSC_VER)
  #define NVMEOF_STATIC_ASSERT(c, m) static_assert(c, m)
#else
  #define NVMEOF_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

// ===========================================================================
//  Little-endian field accessors.
//
//  The wire is little-endian and most of these structures live in unaligned
//  byte buffers (the in-capsule SGL is at offset 24 of a 64-byte SQE, the
//  Identify payload starts at an arbitrary offset in a received message), so
//  every field goes through these rather than through a cast.  A cast would be
//  both alignment-undefined and endian-wrong, and it would work on x86 by
//  accident, which is the worst kind of working.
//
//  These are first in the file because everything below uses them.
// ===========================================================================
static inline uint16_t nvmeof_rd16(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}
static inline uint32_t nvmeof_rd24(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint32_t)(b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16));
}
static inline uint32_t nvmeof_rd32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline uint64_t nvmeof_rd64(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint64_t)nvmeof_rd32(b) | ((uint64_t)nvmeof_rd32(b + 4) << 32);
}
static inline void nvmeof_wr16(void* p, uint16_t v) {
    uint8_t* b = (uint8_t*)p; b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
}
static inline void nvmeof_wr24(void* p, uint32_t v) {
    uint8_t* b = (uint8_t*)p; b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16);
}
static inline void nvmeof_wr32(void* p, uint32_t v) {
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static inline void nvmeof_wr64(void* p, uint64_t v) {
    nvmeof_wr32(p, (uint32_t)v); nvmeof_wr32((uint8_t*)p + 4, (uint32_t)(v >> 32));
}

#pragma pack(push, 1)

// ===========================================================================
//  Sizes
// ===========================================================================
#define NVMEOF_SQE_SIZE           64u   // every NVMe command
#define NVMEOF_CQE_SIZE           16u   // every NVMe completion
#define NVMEOF_IDENTIFY_SIZE    4096u   // Identify payload
#define NVMEOF_CONNECT_DATA_SIZE 1024u  // fabrics Connect data
#define NVMEOF_NQN_FIELD_LEN     256u   // NQN field inside wire structures
#define NVMEOF_NQN_MAX_LEN       223u   // max length of a legal NQN itself
#define NVMEOF_SGL_DESC_SIZE      16u   // ALL SGL descriptor variants

// ===========================================================================
//  Opcodes
// ===========================================================================
enum {
    NVMEOF_OPC_FABRICS          = 0x7f,  // admin opcode meaning "look at fctype"

    // I/O command set
    NVMEOF_OPC_FLUSH            = 0x00,
    NVMEOF_OPC_WRITE            = 0x01,
    NVMEOF_OPC_READ             = 0x02,
    NVMEOF_OPC_WRITE_ZEROES     = 0x08,  // nvme_cmd_write_zeroes
    NVMEOF_OPC_DSM              = 0x09,  // nvme_cmd_dsm (Dataset Management)

    // Write Zeroes / DSM field bits (nvme_write_zeroes_cdw12 / nvme_dsm_cdw11).
    NVMEOF_WZ_DEAC              = 1u << 25,   // "deallocate" instead of writing zeros
    NVMEOF_WZ_FUA               = 1u << 30,
    NVMEOF_DSM_AD               = 1u << 2,    // attributes: deallocate
    NVMEOF_DSM_RANGE_SIZE       = 16u,        // one nvme_dsm_range: 4 + 4 + 8 bytes
    NVMEOF_DSM_MAX_RANGES       = 256u,

    // Admin command set
    NVMEOF_OPC_DELETE_SQ        = 0x00,
    NVMEOF_OPC_CREATE_SQ        = 0x01,
    NVMEOF_OPC_GET_LOG_PAGE     = 0x02,
    NVMEOF_OPC_DELETE_CQ        = 0x04,
    NVMEOF_OPC_CREATE_CQ        = 0x05,
    NVMEOF_OPC_IDENTIFY         = 0x06,
    NVMEOF_OPC_ABORT            = 0x08,
    NVMEOF_OPC_SET_FEATURES     = 0x09,
    NVMEOF_OPC_GET_FEATURES     = 0x0a,
    NVMEOF_OPC_ASYNC_EVENT      = 0x0c,
    NVMEOF_OPC_KEEP_ALIVE       = 0x18,
};

// fctype values, now taken from ref/linux_nvme.h (`enum nvmf_capsule_command`)
// and checked by run_xref.ps1 like every other wire constant.  They are not
// sequential and not in the order one would guess.
//
// This comment used to say "connect == 0x01, property_get == 0x04 and disconnect
// == 0x08 ... taken from the header rather than invented".  Two things were wrong
// with that sentence: it named a disconnect value that no header defines, and it
// claimed a provenance it did not have - the values came from a secondary source
// (FreeBSD's nvmf_proto.h), not from the reference this project keeps and checks
// against.  A comment asserting correctness is not a check; a test is.
//
// Keep Alive is deliberately absent here: it is NOT a fabrics command.  It is
// admin opcode 0x18 in the NVM command set (see NVMEOF_OPC_KEEP_ALIVE above), and
// inventing an fctype for it would have produced a command no Linux target
// recognises.
enum {
    NVMEOF_FCTYPE_PROPERTY_SET  = 0x00,
    NVMEOF_FCTYPE_CONNECT       = 0x01,
    NVMEOF_FCTYPE_PROPERTY_GET  = 0x04,
    NVMEOF_FCTYPE_AUTH_SEND     = 0x05,
    NVMEOF_FCTYPE_AUTH_RECEIVE  = 0x06,
};

// There is no "Disconnect" fabrics command.  This project had one - fctype 0x08,
// with a target branch that treated it as "the host is leaving" and a host step
// that sent it and asserted it was accepted - until the reference header was read:
//
//     enum nvmf_capsule_command {
//         nvme_fabrics_type_property_set  = 0x00,
//         nvme_fabrics_type_connect       = 0x01,
//         nvme_fabrics_type_property_get  = 0x04,
//         nvme_fabrics_type_auth_send     = 0x05,
//         nvme_fabrics_type_auth_receive  = 0x06,
//     };
//
// Five values, and no disconnect anywhere in that header.  A host leaves by
// disabling the controller (a Property Set of CC with EN = 0) and then tearing the
// RDMA connections down - which is what the Linux host does, and what this project
// does now.  Against a real target the fabricated command would simply have been
// answered Invalid Opcode, and our own target's willingness to accept it was the
// only reason the two ends agreed (DESIGN 8.44).

// ===========================================================================
//  SGL descriptors - 16 bytes, all variants
// ===========================================================================
// Byte 15 is (type << 4) | subtype.  SPDK's spdk_nvme_sgl_descriptor declares
// these as 4-bit bitfields in the order `subtype; type;`, which on a
// little-endian target puts subtype in bits 3:0 and type in bits 7:4 - matching
// Linux's "Descriptor subtype - lower 4 bits / Descriptor type - upper 4 bits".
enum {
    // subtype (byte 15, low nibble)
    NVMEOF_SGL_SUBTYPE_ADDRESS      = 0x0,  // absolute address (what RDMA uses)
    NVMEOF_SGL_SUBTYPE_OFFSET       = 0x1,  // offset of in-capsule data
    NVMEOF_SGL_SUBTYPE_TRANSPORT_A  = 0xa,  // transport defined
    NVMEOF_SGL_SUBTYPE_INVALIDATE   = 0xf,  // RDMA remote invalidation request

    // type (byte 15, high nibble)
    NVMEOF_SGL_TYPE_DATA_BLOCK      = 0x0,
    NVMEOF_SGL_TYPE_BIT_BUCKET      = 0x1,  // "offset" subtype only variant
    NVMEOF_SGL_TYPE_SEGMENT         = 0x2,
    NVMEOF_SGL_TYPE_LAST_SEGMENT    = 0x3,
    NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK= 0x4,  // the one NVMe-oF/RDMA leans on
    NVMEOF_SGL_TYPE_TRANSPORT_DATA  = 0x5,
};

typedef struct {
    uint64_t address;       // 0..7
    uint32_t length;        // 8..11
    uint8_t  reserved[3];   // 12..14
    uint8_t  type_subtype;  // 15
} nvmeof_sgl_unkeyed;

typedef struct {
    uint64_t address;       // 0..7
    // 8..10 = length (24-bit LE), 11..14 = key (32-bit LE).  Kept as a byte
    // array so the byte order does not depend on bitfield allocation rules.
    uint8_t  length[3];     // 8..10  (24-bit!)
    uint8_t  key[4];        // 11..14 (rkey, 32-bit)
    uint8_t  type_subtype;  // 15
} nvmeof_sgl_keyed;

typedef union {
    uint64_t           address;
    nvmeof_sgl_unkeyed unkeyed;
    nvmeof_sgl_keyed   keyed;
    uint8_t            raw[NVMEOF_SGL_DESC_SIZE];
} nvmeof_sgl;

NVMEOF_STATIC_ASSERT(sizeof(nvmeof_sgl_unkeyed) == 16, "SGL descriptor must be 16 bytes");
NVMEOF_STATIC_ASSERT(sizeof(nvmeof_sgl_keyed)   == 16, "keyed SGL descriptor is ALSO 16 bytes");
NVMEOF_STATIC_ASSERT(sizeof(nvmeof_sgl)         == 16, "SGL union must be 16 bytes");

static inline uint8_t nvmeof_sgl_make_type_subtype(uint8_t type, uint8_t subtype) {
    return (uint8_t)((type << 4) | (subtype & 0x0f));
}
static inline uint8_t nvmeof_sgl_type_of(uint8_t ts)    { return (uint8_t)(ts >> 4); }
static inline uint8_t nvmeof_sgl_subtype_of(uint8_t ts) { return (uint8_t)(ts & 0x0f); }

// A keyed data-block descriptor with subtype 0xf is a NORMAL data transfer that
// additionally asks the target to invalidate the key it was just given, once the
// transfer is done.  It is not a different kind of transfer, which is exactly why
// it is easy to get wrong: the type nibble is still 0x4, so code that switches on
// the type alone treats it as an ordinary descriptor and **silently drops the
// invalidation**.  The host would then believe its STag is dead while it is still
// live - so a target that cannot honour the request must refuse the command
// rather than complete it.
#define NVMEOF_SGL_TS_KEYED_NORMAL      0x40   /* type 0x4, subtype 0x0 */
#define NVMEOF_SGL_TS_KEYED_INVALIDATE  0x4f   /* type 0x4, subtype 0xf */
static inline int nvmeof_sgl_is_keyed(uint8_t ts) {
    return nvmeof_sgl_type_of(ts) == NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK;
}
static inline int nvmeof_sgl_wants_invalidate(uint8_t ts) {
    return nvmeof_sgl_is_keyed(ts) &&
           nvmeof_sgl_subtype_of(ts) == NVMEOF_SGL_SUBTYPE_INVALIDATE;
}

// ===========================================================================
//  SQE byte 1 (flags) - and why 0x40 is not optional
// ===========================================================================
//
// nvmet validates this byte for EVERY command, INCLUDING fabrics commands, before
// it parses anything at all (drivers/nvme/target/core.c, nvmet_req_init):
//
//     if (unlikely(flags & (NVME_CMD_FUSE_FIRST | NVME_CMD_FUSE_SECOND))) {
//             req->error_loc = offsetof(struct nvme_common_command, flags);
//             status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
//             goto fail;
//     }
//     if (unlikely((flags & NVME_CMD_SGL_ALL) != NVME_CMD_SGL_METABUF)) {
//             req->error_loc = offsetof(struct nvme_common_command, flags);
//             status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
//             goto fail;
//     }
//
// So PSDT must be 0b01 (NVME_CMD_SGL_METABUF) and the FUSE bits must be clear.
// The Linux host does this for every RDMA command (drivers/nvme/host/rdma.c,
// nvme_rdma_map_data: `c->common.flags |= NVME_CMD_SGL_METABUF;`), which is why
// nobody notices: leaving the byte at 0 costs NOTHING against a peer that does
// not check, so both of our own endpoints agreed with each other for months while
// every real target answered Invalid Field (SCT=0 SC=0x02) to all of it - the
// Property Get that then read back as CAP=0, the Property Set and the Connect.
#define NVMEOF_CMD_FLAGS_FUSE_FIRST  0x01u
#define NVMEOF_CMD_FLAGS_FUSE_SECOND 0x02u
#define NVMEOF_CMD_FLAGS_METABUF     0x40u   /* PSDT = 0b01 */
#define NVMEOF_CMD_FLAGS_METASEG     0x80u   /* PSDT = 0b10 */
#define NVMEOF_CMD_FLAGS_SGL_ALL     0xc0u   /* both PSDT bits */

static inline int nvmeof_sqe_flags_ok(uint8_t flags) {
    if (flags & (NVMEOF_CMD_FLAGS_FUSE_FIRST | NVMEOF_CMD_FLAGS_FUSE_SECOND)) return 0;
    if ((flags & NVMEOF_CMD_FLAGS_SGL_ALL) != NVMEOF_CMD_FLAGS_METABUF)        return 0;
    return 1;
}

// ===========================================================================
//  Command and completion
// ===========================================================================
typedef struct {
    uint8_t  opcode;        // 0
    uint8_t  flags;         // 1
    uint16_t command_id;    // 2
    uint32_t nsid;          // 4
    uint32_t cdw2;          // 8
    uint32_t cdw3;          // 12
    uint64_t metadata;      // 16
    nvmeof_sgl dptr;        // 24..39   (PRP1/PRP2 in the base spec)
    uint32_t cdw10;         // 40
    uint32_t cdw11;         // 44
    uint32_t cdw12;         // 48
    uint32_t cdw13;         // 52
    uint32_t cdw14;         // 56
    uint32_t cdw15;         // 60
} nvmeof_sqe;

NVMEOF_STATIC_ASSERT(sizeof(nvmeof_sqe) == NVMEOF_SQE_SIZE, "SQE must be 64 bytes");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_sqe, opcode)     == 0,  "SQE opcode @0");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_sqe, command_id) == 2,  "SQE CID @2");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_sqe, nsid)       == 4,  "SQE NSID @4");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_sqe, metadata)   == 16, "SQE metadata @16");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_sqe, dptr)       == 24, "SQE dptr/SGL @24");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_sqe, cdw10)      == 40, "SQE cdw10 @40");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_sqe, cdw15)      == 60, "SQE cdw15 @60");

// Read/Write: NLB is 0-based in cdw12 bits 15:0, SLBA is a 64-bit field at 40.
typedef struct {
    uint64_t slba;          // 40..47
    uint16_t nlb;           // 48..49  (0-based: 0 means one block)
    uint16_t control;       // 50..51
    uint32_t dsmgmt;        // 52
    uint32_t reftag;        // 56
    uint16_t apptag;        // 60
    uint16_t appmask;       // 62
} nvmeof_rw_cdw;

#define NVMEOF_RW_CONTROL_FUA   (1u << 14)
#define NVMEOF_RW_CONTROL_LR    (1u << 15)

typedef struct {
    // 0..3 = command specific result (u64 or two u32, per command)
    uint64_t result;        // 0..7
    uint16_t sq_head;       // 8..9
    uint16_t sq_id;         // 10..11
    uint16_t command_id;    // 12..13
    uint16_t status;        // 14..15  (bit 0 = phase tag)
} nvmeof_cqe;

NVMEOF_STATIC_ASSERT(sizeof(nvmeof_cqe) == NVMEOF_CQE_SIZE, "CQE must be 16 bytes");

// ---------------------------------------------------------------------------
//  Status field layout - derived, not guessed, because it is the one word that
//  appears on every single completion and getting it wrong rejects every
//  response a real target sends.
//
//  Two published sources appear to disagree:
//
//    SPDK  spdk_nvme_status bitfields, declared p:1 sc:8 sct:3 crd:2 m:1 dnr:1
//          -> on a little-endian target that is p@0, sc@8:1, sct@11:9,
//             crd@13:12, m@14, dnr@15, and SPDK exposes it as a union with
//             status_raw, so these ARE the raw field positions.
//
//    Linux include/linux/nvme.h masks: SC 0x00ff, SCT 0x0700, CRD 0x1800,
//          MORE 0x2000, DNR 0x4000.
//
//  Applying a single right shift by 1 to SPDK's layout reproduces all five of
//  Linux's masks exactly (sc->0x00ff, sct->0x0700, crd->0x1800, m->0x2000,
//  dnr->0x4000), so Linux's constants are defined over `status >> 1` and the
//  two sources agree.  That they must agree is not a guess either: SPDK and
//  Linux nvmet interoperate in production, so a raw-layout disagreement is
//  impossible.
//
//  This is recorded here because both spellings appear in the wild and the
//  difference is one bit - which is exactly the kind of difference that
//  produces a driver that works against one target and not another.
// ---------------------------------------------------------------------------
#define NVMEOF_STATUS_P_MASK     0x0001u  // bit 0
#define NVMEOF_STATUS_SC_SHIFT   1
#define NVMEOF_STATUS_SC_MASK    0x01feu  // bits 8:1
#define NVMEOF_STATUS_SCT_SHIFT  9
#define NVMEOF_STATUS_SCT_MASK   0x0e00u  // bits 11:9
#define NVMEOF_STATUS_CRD_SHIFT  12
#define NVMEOF_STATUS_CRD_MASK   0x3000u  // bits 13:12
#define NVMEOF_STATUS_MORE       0x4000u  // bit 14
#define NVMEOF_STATUS_DNR        0x8000u  // bit 15

// Status Code Type (SCT) values.  Note these are *not* shifted: they are the
// 3-bit type, and callers combine them with the status code via
// NVMEOF_STATUS_MAKE below.
enum {
    NVMEOF_SCT_GENERIC          = 0x0,
    NVMEOF_SCT_COMMAND_SPECIFIC = 0x1,
    NVMEOF_SCT_MEDIA            = 0x2,
    NVMEOF_SCT_PATH             = 0x3,
    NVMEOF_SCT_VENDOR           = 0x7,
};

#define NVMEOF_STATUS_MAKE(sct, sc) \
    ((uint16_t)((((uint32_t)(sct) & 0x7u) << NVMEOF_STATUS_SCT_SHIFT) | \
                (((uint32_t)(sc) & 0xffu) << NVMEOF_STATUS_SC_SHIFT)))

// Building a CQE's status is NOT the same as building the bare (sct, sc) pair:
// every completion queue entry carries the phase bit set, and it lives in the
// same 16-bit field.  Forgetting it is silent - nothing errors, the status just
// reads as 0x0000 - and the resulting failure mode is thoroughly misleading:
// every assertion that expects success fails while every assertion that expects
// a failure passes, because "no phase bit" and "not success" are the same
// predicate.  That looks exactly like a broken transport and is really one
// missing bit in the responder.  Use this when filling a CQE in; use
// NVMEOF_STATUS_MAKE only when you want the pair without a phase, e.g. to
// compare against an extracted (sct, sc).
#define NVMEOF_STATUS_CQE(sct, sc) \
    ((uint16_t)(NVMEOF_STATUS_MAKE(sct, sc) | NVMEOF_STATUS_P_MASK))

static inline uint8_t  nvmeof_status_sc(uint16_t status)  { return (uint8_t)((status & NVMEOF_STATUS_SC_MASK)  >> NVMEOF_STATUS_SC_SHIFT); }
static inline uint8_t  nvmeof_status_sct(uint16_t status) { return (uint8_t)((status & NVMEOF_STATUS_SCT_MASK) >> NVMEOF_STATUS_SCT_SHIFT); }
static inline int      nvmeof_status_phase(uint16_t status) { return (status & NVMEOF_STATUS_P_MASK) != 0; }

// Cross-check against Linux's spelling: after `status >> 1` the 16-bit value
// must match Linux's SC/SCT/MORE/DNR masks.  This is the reconciliation above,
// asserted so that a future edit to either side breaks here.
NVMEOF_STATIC_ASSERT(((NVMEOF_STATUS_SC_MASK  >> 1)) == 0x00ffu, "SC must equal linux NVME_SC_MASK after >>1");
NVMEOF_STATIC_ASSERT(((NVMEOF_STATUS_SCT_MASK >> 1)) == 0x0700u, "SCT must equal linux NVME_SCT_MASK after >>1");
NVMEOF_STATIC_ASSERT(((NVMEOF_STATUS_MORE     >> 1)) == 0x2000u, "MORE must equal linux NVME_STATUS_MORE after >>1");
NVMEOF_STATIC_ASSERT(((NVMEOF_STATUS_DNR      >> 1)) == 0x4000u, "DNR must equal linux NVME_STATUS_DNR after >>1");
NVMEOF_STATIC_ASSERT((NVMEOF_STATUS_CRD_MASK >> 1) == 0x1800u, "CRD must equal linux NVME_STATUS_CRD after >>1");

// The status codes this implementation produces or has to recognise.
enum {
    NVMEOF_SC_SUCCESS              = 0x00,
    NVMEOF_SC_INVALID_OPCODE       = 0x01,
    NVMEOF_SC_INVALID_FIELD        = 0x02,
    NVMEOF_SC_CMDID_CONFLICT       = 0x03,
    NVMEOF_SC_DATA_XFER_ERROR      = 0x04,
    NVMEOF_SC_INTERNAL             = 0x06,
    NVMEOF_SC_INVALID_NS           = 0x0b,
    // The SGL describes a length the command does not agree with (or the data could
    // not be copied out of it).  nvmet answers this for a transfer-length mismatch -
    // nvmet_check_transfer_len / nvmet_copy_from_sgl - and it is what a host sees
    // when a target tries to transfer the wrong number of bytes.
    NVMEOF_SC_SGL_INVALID_DATA     = 0x0f,
    NVMEOF_SC_SGL_INVALID_TYPE     = 0x11,
    NVMEOF_SC_INVALID_IO_CMD_SET   = 0x2c,
    NVMEOF_SC_LBA_RANGE            = 0x80,
    NVMEOF_SC_NS_NOT_READY         = 0x82,
    // command specific
    NVMEOF_SC_CQ_INVALID           = 0x00,
    NVMEOF_SC_QID_INVALID          = 0x01,
    NVMEOF_SC_QUEUE_SIZE           = 0x02,
    NVMEOF_SC_INVALID_QUEUE        = 0x0c,
    // fabrics, command specific
    NVMEOF_SC_CONNECT_FORMAT       = 0x80,
    NVMEOF_SC_CONNECT_CTRL_BUSY    = 0x81,
    NVMEOF_SC_CONNECT_INVALID_PARAM= 0x82,
    NVMEOF_SC_CONNECT_INVALID_HOST = 0x84,
};

// Command-specific status codes used by the async-event machinery.  ASYNC_LIMIT is
// what a controller answers once it is already holding the maximum number of
// Async Event commands; the reference allows 4 outstanding (NVMET_ASYNC_EVENTS)
// and reports NVME_SC_ASYNC_LIMIT for the next one.
#define NVMEOF_AER_MAX_PENDING         4     // NVMET_ASYNC_EVENTS

// ASYNC_LIMIT is the one status this project uses whose number does not fit in
// the 8-bit SC field: the spec's 0x105 goes on the wire as SCT=command-specific
// with SC=0x05.  The pair is written out rather than derived, because the low
// byte alone is ambiguous - 0x80 is LBA_RANGE as a generic status and
// CONNECT_FORMAT as a command-specific one - so there is no arithmetic rule to
// derive it from.  (A first attempt at exactly such a rule is why these lines
// exist: it classified CONNECT_INVALID_PARAM 0x82 as generic and the assert below
// caught it.)
#define NVMEOF_SC_ASYNC_LIMIT          0x105
#define NVMEOF_SC_ASYNC_LIMIT_SCT      NVMEOF_SCT_COMMAND_SPECIFIC
#define NVMEOF_SC_ASYNC_LIMIT_SC       0x05
NVMEOF_STATIC_ASSERT((((uint32_t)NVMEOF_SC_ASYNC_LIMIT_SCT << 8) |
                      (uint32_t)NVMEOF_SC_ASYNC_LIMIT_SC) == NVMEOF_SC_ASYNC_LIMIT,
                     "the split SCT/SC pair must reproduce the spec's 0x105");

// NVME_SC_AUTH_REQUIRED = 0x191 (linux/nvme.h:2202), i.e. SCT=command-specific
// with SC=0x91 - the same 8-bit SC as NVME_SC_INVALID_NS' neighbour range, which
// is why it is spelled as a pair like ASYNC_LIMIT rather than as an SC alone.
//
// A target with a host key refuses every NON-fabrics admin command and every I/O
// command on an unauthenticated admin queue with this | DNR = 0x4191; fabrics
// commands (Connect, Property Get/Set, Auth Send/Receive) are explicitly exempt
// because nvmet's fabrics dispatcher runs before nvmet_check_ctrl_status
// (target/admin-cmd.c:1632-1642).  Getting that exemption wrong is a deadlock,
// not a failure: the host's own Auth Send would be refused with "authenticate
// first".
#define NVMEOF_SC_AUTH_REQUIRED        0x191
#define NVMEOF_SC_AUTH_REQUIRED_SCT    NVMEOF_SCT_COMMAND_SPECIFIC
#define NVMEOF_SC_AUTH_REQUIRED_SC     0x91
NVMEOF_STATIC_ASSERT((((uint32_t)NVMEOF_SC_AUTH_REQUIRED_SCT << 8) |
                      (uint32_t)NVMEOF_SC_AUTH_REQUIRED_SC) == NVMEOF_SC_AUTH_REQUIRED,
                     "the split SCT/SC pair must reproduce the spec's 0x191");
NVMEOF_STATIC_ASSERT(NVMEOF_SC_AUTH_REQUIRED_SC != NVMEOF_SC_ASYNC_LIMIT_SC,
                     "two different command-specific codes must not share an SC");
NVMEOF_STATIC_ASSERT(NVMEOF_SC_AUTH_REQUIRED_SC != NVMEOF_SC_CONNECT_FORMAT,
                     "0x91 (auth required) and 0x80 (connect format) are different "
                     "codes that both live in the command-specific SCT");

// ===========================================================================
//  Fabrics commands - the SQE is reinterpreted, the 64 bytes are not the same
// ===========================================================================
//  common:      opcode@0 resv@1 cid@2 fctype@4 resv[35]@5 TaskTag[24]@40
//  connect:     ... dptr@24 recfmt@40 qid@42 sqsize@44 cattr@46 resv@47 kato@48
//  property:    ... (TaskTag area reused) attrib@40 resv[3]@41 offset@44 value@48
typedef struct {
    uint8_t  opcode;        // 0  = NVMEOF_OPC_FABRICS
    uint8_t  reserved1;     // 1
    uint16_t command_id;    // 2
    uint8_t  fctype;        // 4
    uint8_t  reserved2[35]; // 5..39
    uint8_t  task_tag[24];  // 40..63  (SGL for the *response* on some commands)
} nvmeof_fabrics_common;

typedef struct {
    uint8_t  opcode;        // 0
    uint8_t  reserved1;     // 1
    uint16_t command_id;    // 2
    uint8_t  fctype;        // 4
    uint8_t  reserved2[19]; // 5..23
    nvmeof_sgl dptr;        // 24..39  SGL pointing at the 1024-byte Connect data
    uint16_t recfmt;        // 40
    uint16_t qid;           // 42
    uint16_t sqsize;        // 44
    uint8_t  cattr;         // 46
    uint8_t  reserved3;     // 47
    uint32_t kato;          // 48
    uint8_t  reserved4[12]; // 52..63
} nvmeof_fabrics_connect;

typedef struct {
    uint8_t  opcode;        // 0
    uint8_t  reserved1;     // 1
    uint16_t command_id;    // 2
    uint8_t  fctype;        // 4
    uint8_t  reserved2[35]; // 5..39
    uint8_t  attrib;        // 40
    uint8_t  reserved3[3];  // 41..43
    uint32_t offset;        // 44
    uint64_t value;         // 48..55   (set only)
    uint8_t  reserved4[8];  // 56..63
} nvmeof_fabrics_property_set;

typedef struct {
    uint8_t  opcode;        // 0
    uint8_t  reserved1;     // 1
    uint16_t command_id;    // 2
    uint8_t  fctype;        // 4
    uint8_t  reserved2[35]; // 5..39
    uint8_t  attrib;        // 40
    uint8_t  reserved3[3];  // 41..43
    uint32_t offset;        // 44
    uint8_t  reserved4[16]; // 48..63
} nvmeof_fabrics_property_get;

// ---------------------------------------------------------------------------
//  Auth Send / Auth Receive (fctype 0x05 / 0x06) - DH-HMAC-CHAP
// ---------------------------------------------------------------------------
// The one fabrics command whose data pointer and whose length field are separate
// ideas: the dptr is an ordinary keyed SGL (bytes 24..39) but the length is NOT
// in the SGL's length field as far as the command is concerned - it is `tl` (Auth
// Send) or `al` (Auth Receive) at bytes 44..47, and nvmet checks the two agree
// (nvmet_check_transfer_len).  Reading the transfer length out of the SGL and
// ignoring al/tl would still work against a well-behaved host and fail against
// the reference the moment the two disagree, so both are checked here.
//
//   auth_send:    ... dptr@24 resv3@40 spsp0@41 spsp1@42 secp@43 tl@44 resv4[16]@48
//   auth_receive: ... dptr@24 resv3@40 spsp0@41 spsp1@42 secp@43 al@44 resv4[16]@48
//
// secp is the Security Protocol: 0xE9 is DH-HMAC-CHAP, and nvmet answers Invalid
// Field | DNR (0x4002) to anything else, to spsp0/spsp1 != 1, and to a zero tl/al
// (fabrics-cmd-auth.c:258-282, 538-562).  All three are checked before the payload
// is even read.
#define NVMEOF_AUTH_OFF_SPSP0   41
#define NVMEOF_AUTH_OFF_SPSP1   42
#define NVMEOF_AUTH_OFF_SECP    43
#define NVMEOF_AUTH_OFF_AL_TL   44
#define NVMEOF_AUTH_SECP_DHCHAP 0xE9u
#define NVMEOF_AUTH_SPSP0       0x01u
#define NVMEOF_AUTH_SPSP1       0x01u

typedef struct {
    uint8_t  opcode;        // 0
    uint8_t  reserved1;     // 1
    uint16_t command_id;    // 2
    uint8_t  fctype;        // 4   = 0x05 or 0x06
    uint8_t  reserved2[19]; // 5..23
    nvmeof_sgl dptr;        // 24..39  SGL for the payload
    uint8_t  reserved3;     // 40
    uint8_t  spsp0;         // 41  = 0x01
    uint8_t  spsp1;         // 42  = 0x01
    uint8_t  secp;          // 43  = 0xE9
    union {
        uint32_t tl;        // 44..47  Auth Send: payload length, bytes
        uint32_t al;        // 44..47  Auth Receive: allocation length, bytes
    } al_tl;
    uint8_t  reserved4[16]; // 48..63
} nvmeof_fabrics_auth;

NVMEOF_STATIC_ASSERT(sizeof(nvmeof_fabrics_common)       == 64, "fabrics cmd 64B");
NVMEOF_STATIC_ASSERT(sizeof(nvmeof_fabrics_connect)      == 64, "connect cmd 64B");
NVMEOF_STATIC_ASSERT(sizeof(nvmeof_fabrics_property_set) == 64, "prop set 64B");
NVMEOF_STATIC_ASSERT(sizeof(nvmeof_fabrics_property_get) == 64, "prop get 64B");
NVMEOF_STATIC_ASSERT(sizeof(nvmeof_fabrics_auth)         == 64, "auth cmd 64B");
// The three offsets that a hand-written byte map gets wrong are asserted against
// the struct rather than restated: this is the same trap as §8.44's "constant
// somebody remembered", and `offsetof` cannot be misremembered.
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_fabrics_auth, spsp0)   == NVMEOF_AUTH_OFF_SPSP0, "spsp0 offset");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_fabrics_auth, spsp1)   == NVMEOF_AUTH_OFF_SPSP1, "spsp1 offset");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_fabrics_auth, secp)    == NVMEOF_AUTH_OFF_SECP,  "secp offset");
NVMEOF_STATIC_ASSERT(offsetof(nvmeof_fabrics_auth, al_tl)   == NVMEOF_AUTH_OFF_AL_TL, "al/tl offset");

// NVME_CONNECT_AUTHREQ_ATR: bit 17 of the Connect COMPLETION RESULT (not its
// status).  Bits 15:0 of that dword are CNTLID, so the two share a field and
// "result = cntlid" is silently the "no authentication" answer.  Set for the
// admin queue only - nvmet clears it for qid != 0 (fabrics-cmd.c:251).
#define NVMEOF_CONNECT_AUTHREQ_ATR (1u << 17)
NVMEOF_STATIC_ASSERT((NVMEOF_CONNECT_AUTHREQ_ATR & 0xffffu) == 0,
                     "ATR must live above the CNTLID half of the Connect result");

// There is no disconnect capsule layout.  One used to live here - a 64-byte
// struct with recfmt at 40, mirroring the fctype 0x08 that does not exist - and it
// is removed with it (DESIGN 8.44).  A host leaves by writing CC.EN=0 and dropping
// the RDMA connections; there is no capsule for it.

// ===========================================================================
//  RDMA transport private data - carried in the RDMA-CM private data of the
//  connection, NOT in a capsule.
// ===========================================================================
// This is the piece that makes a Linux peer accept us.  nvmet's RDMA transport
// validates recfmt and the queue sizes out of the *connection* private data on
// every Connect, and returns the controller's receive-queue size the same way on
// Accept; neither side looks in the capsule for it.  Both structures are 32 bytes
// in every NVMe-oF version seen here, which is why the transport is allowed to
// reject any other length (NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH).
#define NVMEOF_RDMA_PRIVATE_DATA_SIZE 32

enum {
    NVMEOF_RDMA_QPTYPE_RC     = 0x1,   // reliable connected
    NVMEOF_RDMA_QPTYPE_RD     = 0x2,
    NVMEOF_RDMA_PRTYPE_NONE   = 0x1,
    NVMEOF_RDMA_PRTYPE_IB     = 0x2,
    NVMEOF_RDMA_PRTYPE_ROCE   = 0x3,   // RoCE v1
    NVMEOF_RDMA_PRTYPE_ROCE2  = 0x4,   // RoCE v2 - what this card runs
    NVMEOF_RDMA_PRTYPE_IWARP  = 0x5,
    NVMEOF_RDMA_CMS_RDMA_CM   = 0x1,
};

typedef struct {
    uint16_t recfmt;        // 0   = 0
    uint16_t qid;           // 2
    uint16_t hrqsize;       // 4   host receive queue size
    uint16_t hsqsize;       // 6   host send queue size
    uint16_t cntlid;        // 8   0xFFFF asks the controller to pick one
    uint8_t  reserved[22];  // 10..31
} nvmeof_rdma_request_pd;

typedef struct {
    uint16_t recfmt;        // 0   = 0
    uint16_t crqsize;       // 2   controller receive queue size
    uint8_t  reserved[28];  // 4..31
} nvmeof_rdma_accept_pd;

typedef struct {
    uint16_t recfmt;        // 0
    uint16_t sts;           // 2   NVMEOF_RDMA_ERROR_*
} nvmeof_rdma_reject_pd;

NVMEOF_STATIC_ASSERT(sizeof(nvmeof_rdma_request_pd) == NVMEOF_RDMA_PRIVATE_DATA_SIZE,
                     "request private data is 32B");
NVMEOF_STATIC_ASSERT(sizeof(nvmeof_rdma_accept_pd)  == NVMEOF_RDMA_PRIVATE_DATA_SIZE,
                     "accept private data is 32B");

enum {
    NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH = 0x1,
    NVMEOF_RDMA_ERROR_INVALID_RECFMT              = 0x2,
    NVMEOF_RDMA_ERROR_INVALID_QID                 = 0x3,
    NVMEOF_RDMA_ERROR_INVALID_HSQSIZE             = 0x4,
    NVMEOF_RDMA_ERROR_INVALID_HRQSIZE             = 0x5,
    NVMEOF_RDMA_ERROR_NO_RESOURCES                = 0x6,
    NVMEOF_RDMA_ERROR_INVALID_IRD                 = 0x7,
    NVMEOF_RDMA_ERROR_INVALID_ORD                 = 0x8,
    NVMEOF_RDMA_ERROR_INVALID_CNTLID              = 0x9,
};

// NVME_AQ_DEPTH: the admin queue is always 32 entries in NVMe, and a fabrics
// target enforces it on the Connect private data (see the check below).
#define NVMEOF_AQ_DEPTH 32

// Defaults a Linux RDMA target uses, from include/linux/nvme-rdma.h: the port
// nvmet listens on unless configfs says otherwise, and its queue-size limits.
#define NVMEOF_RDMA_IP_PORT             4420
#define NVMEOF_RDMA_DEFAULT_QUEUE_SIZE  128
#define NVMEOF_RDMA_MAX_QUEUE_SIZE      256
#define NVMEOF_RDMA_CM_FMT_1_0          0x0

// Fill a Connect request's private data the way the Linux host fills it.
//
// The two queue sizes are NOT symmetric and this is the field a real target
// checks: nvmet computes "my receive queue size = hsqsize + 1" and rejects the
// admin queue when that exceeds NVME_AQ_DEPTH.  So a host that sends
// hsqsize = depth (instead of depth - 1) is refused outright by Linux with
// NVME_RDMA_CM_INVALID_HSQSIZE - which is exactly what this project's host did
// until the reference was read, because our own target never looked.
static inline void nvmeof_rdma_fill_req(nvmeof_rdma_request_pd* pd, uint16_t qid,
                                        uint16_t depth, uint16_t cntlid) {
    memset(pd, 0, sizeof(*pd));
    pd->recfmt  = 0;                              // NVME_RDMA_CM_FMT_1_0
    pd->qid     = qid;
    pd->hrqsize = depth;                          // our receive queue: `depth` responses
    pd->hsqsize = (uint16_t)(depth - 1);          // 0-based, as the Linux host sends it
    pd->cntlid  = cntlid;                         // 0 for admin, the assigned id for I/O
}

// Validate a Connect request's private data the way a Linux target does, in the
// same order and with the same status codes.  Returns 0 when acceptable, else a
// NVMEOF_RDMA_ERROR_* value to send back in the Reject.  On success *qidOut is
// the queue the peer says it is connecting.
static inline uint16_t nvmeof_rdma_check_req(const nvmeof_rdma_request_pd* pd,
                                             uint32_t len, uint16_t* qidOut) {
    if (!pd || len == 0) return NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH;
    if (nvmeof_rd16((const uint8_t*)pd + 0) != 0) return NVMEOF_RDMA_ERROR_INVALID_RECFMT;
    uint16_t qid     = nvmeof_rd16((const uint8_t*)pd + 2);
    uint16_t hsqsize = nvmeof_rd16((const uint8_t*)pd + 6);
    if (qid == 0 && (uint32_t)hsqsize + 1 > NVMEOF_AQ_DEPTH)
        return NVMEOF_RDMA_ERROR_INVALID_HSQSIZE;
    if (qidOut) *qidOut = qid;
    return 0;
}

// SGL descriptors a Linux host actually sends, from the reference drivers:
//
//   * every Read/Write goes through map_sg_fr, which marks the keyed descriptor
//     0x4f - keyed data descriptor WITH the invalidate subtype (host rdma.c:
//     "sg->type = (NVME_KEY_SGL_FMT_DATA_DESC << 4) | NVME_SGL_FMT_INVALIDATE").
//     The target is asked to invalidate the host's key once the transfer is done;
//     Linux's own target answers with IB_WR_SEND_WITH_INV, and its host does not
//     depend on that - it invalidates locally when the completion carries no
//     invalidation.  See nvmeof_sgl_wants_invalidate() for the subtype test.
//   * a command with no data uses a keyed descriptor with addr/len/key = 0.

// The default keep-alive timeout, in milliseconds, when the host does not ask
// for one (NVMF_KATO_DEFAULT).
#define NVMEOF_KATO_DEFAULT 120000

// Feature Identifiers a fabrics host actually sets and gets.  A Linux host sets
// Number of Queues before it will connect any I/O queue at all, so a target that
// does not implement Set Features cannot be brought up by a real host no matter
// how correct its data path is.
//
// VOLATILE_WC and ASYNC_EVENT are here because a real `nvme get-feature` sweep of a
// target found exactly three fids it did not know: 0x06 (twice, from two spellings of
// the same query), 0x04 and 0x0b.  nvmet answers 0x06 with the constant 1
// (admin-cmd.c: `nvmet_set_result(req, 1)`) and 0x0b with the async-event mask it is
// holding, and refuses everything else with Invalid Field | DNR.
//
// 0x04 is TEMPERATURE THRESHOLD, not APST - this constant was named APST for a few
// minutes because that is what 0x04 "usually is" in casual descriptions of the
// feature list.  The reference header is the authority (NVME_FEAT_TEMP_THRESH = 0x04,
// NVME_FEAT_AUTO_PST = 0x0c), and nvmet does not implement it either: its switch has
// `#if 0 case NVME_FEAT_TEMP_THRESH:` and falls through to Invalid Field | DNR.  The
// xref pair below is what caught the wrong name.
enum {
    NVMEOF_FID_ARBITRATION = 0x01,
    NVMEOF_FID_POWER_MGMT  = 0x02,
    NVMEOF_FID_TEMP_THRESH = 0x04,
    NVMEOF_FID_VWC         = 0x06,
    NVMEOF_FID_NUM_QUEUES  = 0x07,
    NVMEOF_FID_ASYNC_EVENT = 0x0b,
    NVMEOF_FID_KATO        = 0x0f,
};

// Controller Configuration / Status bits.  The two that a fabrics host reads back
// and acts on, and which this target used to answer with only half the story:
//
//   * CSTS.SHST (bits 3:2, "shutdown complete") is what a host waits for after it
//     requests a shutdown notification (CC.SHN, bits 15:14).  nvmet sets it on the
//     0 -> nonzero transition of SHN and clears it on the way back
//     (nvmet_update_cc).  Without it the host gives up and logs
//     "Device not ready; aborting shutdown, CSTS=0x1" on every disconnect.
//   * CSTS.CFS (bit 1) is the fatal-error flag, and it is how a target that failed
//     DH-HMAC-CHAP tells a host that the controller is gone rather than merely busy
//     (nvmet_ctrl_fatal_error).
#define NVMEOF_CC_EN              0x00000001u
#define NVMEOF_CC_SHN_SHIFT       14
#define NVMEOF_CC_SHN_MASK        0x0000C000u
#define NVMEOF_CSTS_RDY           0x00000001u
#define NVMEOF_CSTS_CFS           0x00000002u
#define NVMEOF_CSTS_SHST_CMPLT    0x00000008u

// Identify Controller Data Structure values, used by the Connect command and by
// the Identify command's CNS field.  The CNS values themselves are defined with
// the Identify layout further down; what is recorded here is *why two of them
// matter*: a host enumerates namespaces by reading the Active Namespace ID list
// (CNS 2) and then, for each id it finds, the Namespace Identification
// Descriptors (CNS 3).  A controller that answers only CNS=0/1 brings a Linux
// host up as far as Identify Controller and then fails at the namespace scan -
// which looks like a data-path fault and is really a missing admin case.

// Namespace Identification Descriptor types (CNS 3).
//
// These were wrong, and wrong in the worst way: the values came from memory
// rather than from a header, and because this target and this project's own test
// both used the same wrong constant, every assertion passed.  A Linux host
// parsing the response saw nidt=0x03 (UUID) with nidl=1 and would have read a
// one-byte UUID.  The authoritative values are from include/linux/nvme.h:
//
//   NVME_NIDT_EUI64 = 0x01, NVME_NIDT_NGUID = 0x02,
//   NVME_NIDT_UUID  = 0x03, NVME_NIDT_CSI   = 0x04
//
// The lesson is the project's own stated top risk: two endpoints written by the
// same author agree with each other and disagree with the world.
enum {
    NVMEOF_NIDT_EUI64 = 0x01,
    NVMEOF_NIDT_NGUID = 0x02,
    NVMEOF_NIDT_UUID  = 0x03,
    NVMEOF_NIDT_CSI   = 0x04,
};
#define NVMEOF_NIDT_CSI_LEN 1
#define NVMEOF_NIDT_UUID_LEN 16
#define NVMEOF_NIDT_NGUID_LEN 16
#define NVMEOF_NIDT_EUI64_LEN 8
enum { NVMEOF_CSI_NVM = 0x00 };

// ===========================================================================
//  Fields that are ZERO ON PURPOSE
// ===========================================================================
// This controller leaves a whole group of Identify Controller capability fields
// at zero, and that is not laziness - each zero suppresses a command or a code
// path this implementation does not have.  A Linux host reads them and decides
// what to send; filling one in "for completeness" activates a conversation the
// target cannot finish.
//
//   OAES = 0    -> the host computes supported_aens = oaes & NVME_AEN_SUPPORTED,
//                  gets 0, and returns from nvme_enable_aen() without ever
//                  issuing Set Features: Async Event or posting an Async Event
//                  command.  This is the only reason AEN is not required.
//   LPA  = 0    -> no Command Effects log, which would be a Get Log Page.
//   APSTA = 0   -> no APST, which would be Set Features: Auto Power State
//                  Transition with a 256-byte payload.
//   ONCS = 0    -> no timestamp Set Features, and the host will not send DSM,
//                  Write Zeroes or reservations.
//   CTRATT = 0  -> no host behaviour Set Features (CRDT/ELBAS gated).
//   CMIC = 0    -> no ANA log read, and a second controller on the same
//                  subsystem is refused - which is correct, we serve one.
//   OACS = 0    -> no namespace management, no security, no directives.
//
// The order of business when implementing one of those features is to implement
// the command it activates FIRST and set the bit second.
// These are #defines rather than enum members on purpose: DISC_CHANGE is bit 31,
// and an enumerator whose value does not fit in an `int` is converted to a
// negative int (MSVC C4308 under /W4 /WX).  The value is still correct in the
// end, but only after a sign change that a reader should not have to reason
// about - and a mask that went negative somewhere would compare wrongly.
#define NVMEOF_OAES_NS_ATTR     0x00000100u
#define NVMEOF_OAES_FW_ACT      0x00000200u
#define NVMEOF_OAES_ANA_CHANGE  0x00000800u
#define NVMEOF_OAES_DISC_CHANGE 0x80000000u
#define NVMEOF_OAES_AEN_SUPPORTED \
    (NVMEOF_OAES_NS_ATTR | NVMEOF_OAES_FW_ACT | \
     NVMEOF_OAES_ANA_CHANGE | NVMEOF_OAES_DISC_CHANGE)

#define NVMEOF_CTRL_LPA_CMD_EFFECTS_LOG (1u << 1)
#define NVMEOF_CTRL_CMIC_MULTI_PORT     (1u << 0)
#define NVMEOF_CTRL_CMIC_MULTI_CTRL     (1u << 1)
#define NVMEOF_CTRL_CMIC_ANA            (1u << 3)
#define NVMEOF_CTRL_VWC_PRESENT         (1u << 0)

// Controller registers reachable through Property Get.  A host reads CSTS to
// wait for ready and VS to learn the version; answering every offset with the
// same value (which this target used to do) is wrong for all of them.
#define NVMEOF_PROP_CAP     0x00   // 8 bytes
#define NVMEOF_PROP_VS      0x08   // 4 bytes
#define NVMEOF_PROP_NSSR    0x20   // 4 bytes
#define NVMEOF_PROP_CRTO    0x68   // 4 bytes; only readable when CAP.CRMS says so

// A minimal but honest CAP: MQES (max queue entries, 0-based) in bits 15:0,
// CQR (contiguous queues required) in bit 16, TO (ready timeout, 500 ms units)
// in bits 31:24, and CSS.NVM (bit 37).
//
// TO is not decoration.  A Linux host computes its ready-wait deadline as
// (CAP.TO + 1) / 2 seconds and reads CSTS once before checking that deadline, so
// TO = 0 leaves it exactly one iteration of slack: a controller that is not
// already reporting RDY when it is first asked is rejected outright.  This
// target sets RDY synchronously with CC.EN, so it survives TO = 0 - but only by
// having no scheduling jitter at all, which is not a property to depend on.
#define NVMEOF_CAP_MQES 31u
#define NVMEOF_CAP_TO   30u

// CAP.CRMS (Controller Ready Modes Supported), bits 44:43 of CAP:
//   bit 43 = CRWMS (ready with a configurable timeout), bit 44 = CRIMS
//   (ready independently).  Both are what a real host looks at before it decides
//   whether reading CRTO is even a legal question - nvmet sets CRWMS alone, and
//   nvmet's property get does not implement CRTO at all, so a host that reads CRTO
//   unconditionally gets Invalid Field from a perfectly healthy Linux target.
//   include/linux/nvme.h: NVME_CAP_CRMS_CRWMS / NVME_CAP_CRMS_CRIMS.
#define NVMEOF_CAP_CRMS_CRWMS (1ull << 43)
#define NVMEOF_CAP_CRMS_CRIMS (1ull << 44)
#define NVMEOF_CAP_VALUE ((uint64_t)NVMEOF_CAP_MQES | (1ull << 16) | \
                          ((uint64_t)NVMEOF_CAP_TO << 24) | (1ull << 37))

// Property Get/Set 'attrib' byte: bits 2:0 are the access size (0 = 4 bytes,
// 1 = 8 bytes).  Linux's target splits its register table on this bit - an
// 8-byte read may only name CAP, a 4-byte read may only name VS/CC/CSTS/CRTO -
// so a target that ignores the field answers questions nobody asked.
#define NVMEOF_PROP_ATTRIB_SIZE_MASK 0x7u
#define NVMEOF_PROP_SIZE_4           0u
#define NVMEOF_PROP_SIZE_8           1u

// Connect command's cattr bit 2: the host is telling the controller that it is
// not tracking SQ head, and the controller should report SQHD as 0xffff.
// Linux's nvmet honours it (nvmet_install_queue); ignoring it means reporting a
// head pointer the host explicitly said it would not maintain.
#define NVMEOF_CONNECT_CATTR_DISABLE_SQFLOW (1u << 2)

// Keep Alive's status codes.
//
// These are GENERIC status codes in the 0x19..0x1a range, not command-specific
// ones at 0x01/0x02.  The original values here were invented from memory and
// then asserted by our own test, so the wrong answer passed.  From
// include/linux/nvme.h:
//
//   NVME_SC_KA_TIMEOUT_EXPIRED = 0x19,
//   NVME_SC_KA_TIMEOUT_INVALID = 0x1A,
//
// both with SCT = NVME_SCT_GENERIC (0).  A host distinguishes them from
// command-specific codes by SCT, so getting the type wrong makes the status
// unreadable even when the code is right.
enum {
    NVMEOF_SC_KA_TIMEOUT_EXPIRED = 0x19,
    NVMEOF_SC_KA_TIMEOUT_INVALID = 0x1a,
};

// Generic command-specific status codes used by the Connect path.  Linux's
// nvmet answers a second Connect for a queue id that already exists with
// CMD_SEQ_ERROR, not with a connect-specific code - the difference matters to a
// host deciding whether to retry.
#define NVMEOF_SC_CMD_SEQ_ERROR 0x0c

// property_set/get attrib values
enum {
    NVMEOF_ATTRIB_HSQ    = 0x0,  // host SQ entry size
    NVMEOF_ATTRIB_HCQ    = 0x1,  // host CQ entry size
    NVMEOF_ATTRIB_SQHD   = 0x2,  // SQ head doorbell
    NVMEOF_ATTRIB_CQHD   = 0x3,  // CQ head doorbell
    // 0x4..0x7 are the "keep alive timer for a specific controller" variants
    NVMEOF_ATTRIB_CQT    = 0x4,
    NVMEOF_ATTRIB_CQD    = 0x5,
    NVMEOF_ATTRIB_MNAN   = 0x6,
    NVMEOF_ATTRIB_MNAND  = 0x7,
};
// Controller property offsets that fabrics property_set/get address.  These are
// the *register* offsets from the base spec, with the uppermost byte replaced by
// the attrib value, so only the low bits of the offset are meaningful.
#define NVMEOF_PROP_CC      0x14
#define NVMEOF_PROP_CSTS    0x1c

// cattr bits
#define NVMEOF_CONNECT_CATTR_SQFLOW   (1u << 0)
#define NVMEOF_CONNECT_CATTR_SQDISABLE (1u << 2)

#define NVMEOF_CNTLID_DYNAMIC  0xffffu   // request a dynamic controller id
// "Note that cntlid of value 0 is considered illegal in the fabrics world"
// (include/linux/nvme.h).  The controller id this target hands out must be in
// this range, and it must be the same number the Connect response returned and
// the Identify Controller payload reports - the host compares all three.
#define NVMEOF_CNTLID_MIN      1u
#define NVMEOF_CNTLID_MAX      0xffefu
#define NVMEOF_QID_ADMIN       0u
// Connect data - exactly 1024 bytes.
typedef struct {
    uint8_t  host_id[16];                    // 0..15
    uint16_t cntlid;                         // 16..17
    uint8_t  reserved4[238];                 // 18..255
    char     subsysnqn[NVMEOF_NQN_FIELD_LEN];// 256..511
    char     hostnqn[NVMEOF_NQN_FIELD_LEN];  // 512..767
    uint8_t  reserved5[256];                 // 768..1023
} nvmeof_connect_data;

NVMEOF_STATIC_ASSERT(sizeof(nvmeof_connect_data) == NVMEOF_CONNECT_DATA_SIZE,
                     "Connect data must be 1024 bytes");

// ===========================================================================
//  Identify Controller
// ===========================================================================
// A note on how this is modelled, because the obvious approach is a trap.
//
// The natural thing is `struct nvme_id_ctrl` copied from the kernel header.
// That struct is 4096 bytes long and most of its middle is reserved runs whose
// lengths were redefined between spec revisions (the kernel's own field is even
// *named* rsvd352 while sitting at a different offset in current revisions).
// Hand-copying it means one wrong reserved length silently shifts every field
// after it, and the failure mode is a controller that connects fine and then
// misbehaves in ways that look like anything but an off-by-four.
//
// So the structure is accessed by named offset into a 4096-byte byte buffer, and
// the fields are read with the little-endian helpers at the bottom of this file.
// Only the anchors this implementation actually uses are named, and each one is
// asserted to be where the NVMe base specification says it is.
//
// The offsets below are from the NVMe base specification's Identify Controller
// data structure figure.  They cannot be compile-time verified against the spec
// from here - what verifies them is the interop test against an independent
// target (Linux nvmet), which is the acceptance gate for this work.

#define NVMEOF_ID_CTRL_OFF_VID        0u     // u16
#define NVMEOF_ID_CTRL_OFF_SSVID      2u     // u16
#define NVMEOF_ID_CTRL_OFF_SN         4u     // char[20]
#define NVMEOF_ID_CTRL_OFF_MN         24u    // char[40]
#define NVMEOF_ID_CTRL_OFF_FR         64u    // char[8]
#define NVMEOF_ID_CTRL_OFF_RAB        72u    // u8   recommended arbitration burst
#define NVMEOF_ID_CTRL_OFF_IEEE       73u    // u8[3] OUI
#define NVMEOF_ID_CTRL_OFF_CMIC       76u    // u8   multi-path / multi-controller / ANA
#define NVMEOF_ID_CTRL_OFF_MDTS       77u    // u8  (0 = no limit)
#define NVMEOF_ID_CTRL_OFF_CNTLID     78u    // u16
#define NVMEOF_ID_CTRL_OFF_VER        80u    // u32
#define NVMEOF_ID_CTRL_OFF_CTRATT     96u    // u32  controller attributes
#define NVMEOF_ID_CTRL_OFF_CNTRLTYPE  111u   // u8
#define NVMEOF_ID_CTRL_OFF_OACS       256u   // u16
#define NVMEOF_ID_CTRL_OFF_ACL        258u   // u8
#define NVMEOF_ID_CTRL_OFF_AERL       259u   // u8
#define NVMEOF_ID_CTRL_OFF_FRMW       260u   // u8
#define NVMEOF_ID_CTRL_OFF_LPA        261u   // u8
#define NVMEOF_ID_CTRL_OFF_APSTA      265u   // u8
#define NVMEOF_ID_CTRL_OFF_OAES       92u    // u32  optional async events supported
#define NVMEOF_ID_CTRL_OFF_KAS        320u   // u16  keep alive support, seconds
#define NVMEOF_ID_CTRL_OFF_SQES       512u   // u8
#define NVMEOF_ID_CTRL_OFF_CQES       513u   // u8
#define NVMEOF_ID_CTRL_OFF_MAXCMD     514u   // u16  max outstanding commands
#define NVMEOF_ID_CTRL_OFF_NN         516u   // u32  number of namespaces
#define NVMEOF_ID_CTRL_OFF_ONCS       520u   // u16
#define NVMEOF_ID_CTRL_OFF_VWC        525u   // u8
#define NVMEOF_ID_CTRL_OFF_AWUN       526u   // u16
#define NVMEOF_ID_CTRL_OFF_AWUPF      528u   // u16
#define NVMEOF_ID_CTRL_OFF_SGLS       536u   // u32
#define NVMEOF_ID_CTRL_OFF_SUBNQN     768u   // char[256]
#define NVMEOF_ID_CTRL_OFF_IOCCSZ     1792u  // u32  I/O cmd capsule size /16
#define NVMEOF_ID_CTRL_OFF_IORCSZ     1796u  // u32  I/O rsp capsule size /16
#define NVMEOF_ID_CTRL_OFF_ICDOFF     1800u  // u16  in-capsule data offset /16
#define NVMEOF_ID_CTRL_OFF_CTRATTR    1802u  // u8   fabrics ctrl attributes
#define NVMEOF_ID_CTRL_OFF_MSDBD      1803u  // u8   max SGL data block descriptors

// These two must agree; the compiler enforces the relationship, and the values
// are the published ones.  If a future revision moves them, this is where it
// breaks loudly rather than at 3am against a real array.
NVMEOF_STATIC_ASSERT(NVMEOF_ID_CTRL_OFF_ICDOFF + 2u == NVMEOF_ID_CTRL_OFF_CTRATTR,
                     "icdoff/ctrattr adjacency");
NVMEOF_STATIC_ASSERT(NVMEOF_ID_CTRL_OFF_CTRATTR + 1u == NVMEOF_ID_CTRL_OFF_MSDBD,
                     "ctrattr/msdbd adjacency");
// The three that sit between other fields, so a wrong value cannot be spotted
// by eye: RAB is one byte ending where MDTS begins, MAXCMD fills the gap
// between CQES and NN, and KAS is a 16-bit field at 320.
NVMEOF_STATIC_ASSERT(NVMEOF_ID_CTRL_OFF_RAB + 1u < NVMEOF_ID_CTRL_OFF_MDTS,
                     "RAB precedes MDTS");
NVMEOF_STATIC_ASSERT(NVMEOF_ID_CTRL_OFF_CQES + 1u == NVMEOF_ID_CTRL_OFF_MAXCMD,
                     "MAXCMD follows CQES");
NVMEOF_STATIC_ASSERT(NVMEOF_ID_CTRL_OFF_MAXCMD + 2u == NVMEOF_ID_CTRL_OFF_NN,
                     "MAXCMD precedes NN");
NVMEOF_STATIC_ASSERT(NVMEOF_ID_CTRL_OFF_KAS + 2u <= NVMEOF_ID_CTRL_OFF_SQES,
                     "KAS is inside the controller area");

// sgls bits.  Values from include/linux/nvme.h:
//
//   NVME_CTRL_SGLS_BYTE_ALIGNED = 1,      NVME_CTRL_SGLS_DWORD_ALIGNED = 2,
//   NVME_CTRL_SGLS_KSDBDS       = 1 << 2, NVME_CTRL_SGLS_MSDS          = 1 << 19,
//   NVME_CTRL_SGLS_SAOS         = 1 << 20,
//
// Bit 20 is NOT "the 24-bit length is supported".  The 24-bit length is a
// property of the keyed descriptor's *format* and has no capability bit at all;
// bit 20 is SAOS - "SGLs are supported with an Offset", i.e. in-capsule data
// addressed by an offset descriptor.  This target advertised bit 20 in all four
// of its targets while rejecting offset descriptors as an invalid SGL type, so
// it claimed a capability it refuses to honour.  A host that believes the claim
// can put data in the capsule and get an error back for a request it was told
// was legal.
//
// What these targets actually do is keyed data-block descriptors, byte aligned,
// which is exactly what Linux's nvmet advertises for the RDMA transport before
// in-capsule data is considered.
#define NVMEOF_CTRL_SGLS_BYTE_ALIGNED  (1u << 0)
#define NVMEOF_CTRL_SGLS_DWORD_ALIGNED (1u << 1)
#define NVMEOF_CTRL_SGLS_KEYED         (1u << 2)   // KSDBDS
#define NVMEOF_CTRL_SGLS_SAOS          (1u << 20)  // in-capsule data via offset SGL
#define NVMEOF_CTRL_SGLS_ADVERTISED \
    (NVMEOF_CTRL_SGLS_BYTE_ALIGNED | NVMEOF_CTRL_SGLS_KEYED)

// fabrics ctrattr bits
#define NVMEOF_CTRL_CTRATTR_128BIT_HOSTID (1u << 0)

// ctrltype values
enum {
    NVMEOF_CTRLTYPE_IO    = 1,
    NVMEOF_CTRLTYPE_DISC  = 2,
    NVMEOF_CTRLTYPE_ADMIN = 3,
};

// Identify command CNS (Controller or Namespace Structure) values, carried in
// cdw10 byte 0 of an Identify command.
enum {
    NVMEOF_ID_CNS_NS              = 0x00,
    NVMEOF_ID_CNS_CTRL            = 0x01,
    NVMEOF_ID_CNS_NS_ACTIVE_LIST  = 0x02,
    NVMEOF_ID_CNS_NS_DESC_LIST    = 0x03,
    NVMEOF_ID_CNS_CS_NS           = 0x05,
    NVMEOF_ID_CNS_CS_CTRL         = 0x06,
    NVMEOF_ID_CNS_NS_PRESENT_LIST = 0x10,
    NVMEOF_ID_CNS_NS_PRESENT      = 0x11,
};

// oncs bits.  Values from include/linux/nvme.h.  There is deliberately no
// "verify" here: bit 7 is not an ONCS bit, and a made-up capability bit is worse
// than a missing one because a host will believe it.
#define NVMEOF_CTRL_ONCS_COMPARE        (1u << 0)
#define NVMEOF_CTRL_ONCS_WRITE_UNCOR    (1u << 1)
#define NVMEOF_CTRL_ONCS_DSM            (1u << 2)
#define NVMEOF_CTRL_ONCS_WRITE_ZEROES   (1u << 3)
#define NVMEOF_CTRL_ONCS_RESERVATIONS   (1u << 5)
#define NVMEOF_CTRL_ONCS_TIMESTAMP      (1u << 6)

// nsfeat bits (Identify Namespace byte 24).  Value from include/linux/nvme.h:
// NVME_NS_FEAT_THIN = 1 << 0.  Thin provisioning means "a deallocated block may
// read as something defined" - without it a host has no reason to believe a
// deallocate did anything, and without ONCS.DSM it never sends one at all.
#define NVMEOF_NS_FEAT_THIN             (1u << 0)

// WHAT WE ADVERTISE, IN ONE PLACE, TOGETHER WITH THE REASON.
//
// ONCS is a promise: a bit set here says "send me this and I will answer it".  A
// host that believes a promise this target cannot keep loses data or wedges, so
// the rule is "advertise only what the dispatch answers".  A comment cannot make
// that rule survive editing; this can:
//
//   * the value written into Identify Controller is COMPUTED from this table
//     (nvmeofOncsFromCaps below), so the field and the table cannot drift;
//   * the table is unit-tested with no hardware and no target (iscsi_selftest.cpp,
//     "nvmeof capability table"), which pins the exact set - a new bit has to be
//     added here deliberately and the test has to be updated with it.
//
// Compare (0x05), Write Uncorrectable (0x04) and Reservations are deliberately
// ABSENT: there is no case for any of them in the I/O dispatch, so advertising
// them would be a lie the host acts on.  They were absent before this table
// existed too - the difference is that now their absence is checked.
struct NvmeOfIoCaps { uint16_t oncsBit; uint8_t opcode; const char* name; };

// "struct NvmeOfIoCaps", not "NvmeOfIoCaps": this header is compiled as BOTH C and C++
// (wire_selftest.c is built both ways on purpose), and in C a struct tag does not become
// a type name.  Writing it the C++ way here compiles in every C++ file and fails only in
// the C build - which is exactly what happened, and exactly why that build exists.
static const struct NvmeOfIoCaps kNvmeOfIoCaps[] = {
    { NVMEOF_CTRL_ONCS_DSM,          NVMEOF_OPC_DSM,          "Dataset Management" },
    { NVMEOF_CTRL_ONCS_WRITE_ZEROES, NVMEOF_OPC_WRITE_ZEROES, "Write Zeroes" },
};

// Identify Controller's ONCS: the OR of the table above.
static inline uint16_t nvmeofOncsFromCaps(void) {
    uint16_t v = 0;
    for (unsigned i = 0; i < sizeof(kNvmeOfIoCaps) / sizeof(kNvmeOfIoCaps[0]); i++)
        v |= kNvmeOfIoCaps[i].oncsBit;
    return v;
}

// Convenience readers over a 4096-byte Identify Controller buffer.
typedef struct { uint8_t raw[NVMEOF_IDENTIFY_SIZE]; } nvmeof_id_ctrl_buf;

static inline uint16_t nvmeof_idc_u16(const nvmeof_id_ctrl_buf* b, uint32_t off) {
    return nvmeof_rd16(b->raw + off);
}
static inline uint32_t nvmeof_idc_u32(const nvmeof_id_ctrl_buf* b, uint32_t off) {
    return nvmeof_rd32(b->raw + off);
}
static inline const char* nvmeof_idc_str(const nvmeof_id_ctrl_buf* b, uint32_t off) {
    return (const char*)(b->raw + off);
}


// ===========================================================================
//  Identify Namespace - the fields we need
// ===========================================================================
#define NVMEOF_ID_NS_OFF_NSZE     0u    // u64, size in logical blocks
#define NVMEOF_ID_NS_OFF_NCAP     8u    // u64
#define NVMEOF_ID_NS_OFF_NUSE     16u   // u64
// The next five offsets are DERIVED FROM THE REFERENCE STRUCT, not remembered.
// ref/linux_nvme.h:438 (struct nvme_id_ns) lists the fields in order, and the
// offsets already in use here are the anchors that prove the walk is right:
// nlbaf 25, flbas 26, nmic 30, nsattr 99 and lbaf 128 all agree with the values
// this file has been using.  Counting the same field list gives:
//   nsfeat 24 (u8)   dlfeat 33 (u8)   npwg 64 (u16)   npwa 66 (u16)
//   npdg 68 (u16)    npda 70 (u16)    nows 72 (u16)
// These exist because the target now advertises deallocate; before that it told
// hosts it had no thin provisioning at all and a mounted namespace had no discard.
#define NVMEOF_ID_NS_OFF_NSFEAT   24u   // u8,  bit 0 = thin provisioning
#define NVMEOF_ID_NS_OFF_DLFEAT   33u   // u8,  what a deallocated block reads as
#define NVMEOF_ID_NS_OFF_NPWG     64u   // u16, preferred write granularity, 0-based blocks
#define NVMEOF_ID_NS_OFF_NPWA     66u   // u16, preferred write alignment
#define NVMEOF_ID_NS_OFF_NPDG     68u   // u16, preferred deallocate granularity
#define NVMEOF_ID_NS_OFF_NPDA     70u   // u16, preferred deallocate alignment
#define NVMEOF_ID_NS_OFF_NOWS     72u   // u16, optimal write size
#define NVMEOF_ID_NS_OFF_NLBAF    25u   // u8, number of LBA formats - 1
#define NVMEOF_ID_NS_OFF_FLBAS    26u   // u8, formatted LBA size index
#define NVMEOF_ID_NS_OFF_NMIC     30u   // u8, multi-path capabilities
#define NVMEOF_ID_NS_OFF_NSATTR   99u   // u8, namespace attributes
#define NVMEOF_ID_NS_OFF_LBAF     128u  // nvmeof_lbaf[nlbaf+1]

// FLBAS does not hold the LBA format index in one piece: bits 3:0 are the low
// four bits and bits 6:5 are the top two, which is why Linux's
// nvme_lbaf_index() reassembles them as
//   (flbas & 0xf) | ((flbas & 0x60) >> 1)
// A reader that treats FLBAS as a plain byte gets index 0 right by accident and
// every other index wrong.
#define NVMEOF_ID_NS_FLBAS_LBA_MASK  0x0fu
#define NVMEOF_ID_NS_FLBAS_LBA_UMASK 0x60u
#define NVMEOF_ID_NS_FLBAS_LBA_SHIFT 1u

enum {
    NVMEOF_NS_NMIC_SHARED = 0x01,
};
#define NVMEOF_NS_ATTR_RO 0x01u

typedef struct {
    uint16_t ms;   // metadata size
    uint8_t  ds;   // log2 of the LBA data size
    uint8_t  rp;   // relative performance
} nvmeof_lbaf;

NVMEOF_STATIC_ASSERT(sizeof(nvmeof_lbaf) == 4, "LBA format entry is 4 bytes");

// ===========================================================================
//  Discovery (NVMe-oF 1.0 §5.3; every number here is quoted from ref/linux_nvme.h)
// ===========================================================================
//
// A host that wants to FIND targets connects to a well-known subsystem name and
// asks for the discovery log page.  This is the whole of it:
//
//   #define NVME_DISC_SUBSYS_NAME  "nqn.2014-08.org.nvmexpress.discovery"   (line 28)
//   #define NVME_LOG_DISC          0x70                                      (line 1423)
//   enum nvme_subsys_type { NVME_NQN_DISC = 1, NVME_NQN_NVME = 2, ... }      (line 35)
//   struct nvmf_disc_rsp_page_hdr   { u64 genctr; u64 numrec; u16 recfmt; u8 resv[1006]; }
//   struct nvmf_disc_rsp_page_entry { ...1024 bytes, see the offsets below... }
//
// Two details that a guess would get wrong: the entry subtype for a real NVMe
// subsystem is **2** (1 means "referral to another discovery controller"), and
// `cntlid` in an entry is **0xffff** ("dynamic") because the controller id does not
// exist until a host actually connects.
#define NVMEOF_DISC_SUBSYS_NAME "nqn.2014-08.org.nvmexpress.discovery"
#define NVMEOF_LOG_DISC         0x70

// Log page identifiers.  Only the ones whose ANSWER MATTERS are here: which LIDs a
// target may answer with a zero-filled success is not a free choice, it is measured
// against nvmet (linux/f5_nvmet_ref.sh + f5_nvmet_ref2.sh).  See the table in
// f5_interop.cpp's Get Log Page branch for the rows and the reason.
//   linux/include/linux/nvme.h:  NVME_LOG_ERROR 0x01 / NVME_LOG_SMART 0x02 /
//                                NVME_LOG_FW_SLOT 0x03 / NVME_LOG_CHANGED_NS 0x04 /
//                                NVME_LOG_CMD_EFFECTS 0x05 / NVME_LOG_ANA 0x0c
#define NVMEOF_LOG_ERROR        0x01
#define NVMEOF_LOG_SMART        0x02
#define NVMEOF_LOG_FW_SLOT      0x03
#define NVMEOF_LOG_CMD_EFFECTS  0x05
#define NVMEOF_LOG_ANA          0x0c

#define NVMEOF_NQN_DISC         1     // discovery log entry subtype: referral
#define NVMEOF_NQN_NVME         2     // discovery log entry subtype: NVMe subsystem
#define NVMEOF_NQN_CURR         3     // ...or "this is the current discovery subsystem"

#define NVMEOF_DISC_HDR_SIZE    1024u
#define NVMEOF_DISC_ENTRY_SIZE  1024u
#define NVMEOF_DISC_ENTRY_OFF_TRTYPE   0u
#define NVMEOF_DISC_ENTRY_OFF_ADRFAM   1u
#define NVMEOF_DISC_ENTRY_OFF_SUBTYPE  2u
#define NVMEOF_DISC_ENTRY_OFF_TREQ     3u
#define NVMEOF_DISC_ENTRY_OFF_PORTID   4u
#define NVMEOF_DISC_ENTRY_OFF_CNTLID   6u
#define NVMEOF_DISC_ENTRY_OFF_ASQSZ    8u
#define NVMEOF_DISC_ENTRY_OFF_TRSVCID  32u
#define NVMEOF_DISC_ENTRY_OFF_SUBNQN   256u
#define NVMEOF_DISC_ENTRY_OFF_TRADDR   512u
#define NVMEOF_DISC_ENTRY_OFF_TSAS     768u

#define NVMEOF_TRTYPE_RDMA      1     // transport type: RDMA (TCP is 3)
#define NVMEOF_ADRFAM_IPV4      1     // address family: IPv4 (IPv6 is 2)
// cntlid 0xffff ("dynamic") is NVMEOF_CNTLID_DYNAMIC, defined with the Connect
// command above - a discovery entry uses the same value, because the controller id
// does not exist until a host connects.

NVMEOF_STATIC_ASSERT(NVMEOF_DISC_ENTRY_OFF_TSAS + 256u == NVMEOF_DISC_ENTRY_SIZE,
                     "a discovery entry is header + 256 bytes of TSAS");

// recfmt 0 is the only format NVMe-oF 1.0 defines, and the host checks it.
static inline void nvmeof_disc_hdr_set(uint8_t* p, uint64_t genctr, uint64_t numrec) {
    memset(p, 0, NVMEOF_DISC_HDR_SIZE);
    nvmeof_wr64(p, genctr);
    nvmeof_wr64(p + 8, numrec);
    nvmeof_wr16(p + 16, 0);
}

// One entry.  `trsvcid` is the port number as TEXT ("4420"), which is why it has its
// own 32-byte field rather than the binary port id.
static inline void nvmeof_disc_entry_set(uint8_t* e, uint8_t subtype, uint16_t portid,
                                         uint16_t cntlid, const char* subnqn,
                                         const char* traddr, const char* trsvcid) {
    memset(e, 0, NVMEOF_DISC_ENTRY_SIZE);
    e[NVMEOF_DISC_ENTRY_OFF_TRTYPE]  = NVMEOF_TRTYPE_RDMA;
    e[NVMEOF_DISC_ENTRY_OFF_ADRFAM]  = NVMEOF_ADRFAM_IPV4;
    e[NVMEOF_DISC_ENTRY_OFF_SUBTYPE] = subtype;
    e[NVMEOF_DISC_ENTRY_OFF_TREQ]    = 0;      // no authentication required
    nvmeof_wr16(e + NVMEOF_DISC_ENTRY_OFF_PORTID, portid);
    nvmeof_wr16(e + NVMEOF_DISC_ENTRY_OFF_CNTLID, cntlid);
    nvmeof_wr16(e + NVMEOF_DISC_ENTRY_OFF_ASQSZ, NVMEOF_AQ_DEPTH);
    if (trsvcid) memcpy(e + NVMEOF_DISC_ENTRY_OFF_TRSVCID, trsvcid, strlen(trsvcid));
    if (subnqn)  memcpy(e + NVMEOF_DISC_ENTRY_OFF_SUBNQN, subnqn, strlen(subnqn));
    if (traddr)  memcpy(e + NVMEOF_DISC_ENTRY_OFF_TRADDR, traddr, strlen(traddr));
}

// ===========================================================================
//  A generic little-endian field reader - moved to the top of the file.
// ===========================================================================

// Build a keyed SGL descriptor pointing at [addr, addr+len) with the given rkey.
static inline void nvmeof_sgl_set_keyed(nvmeof_sgl* sgl, uint64_t addr, uint32_t len, uint32_t rkey) {
    nvmeof_wr64(sgl->raw, addr);
    nvmeof_wr24(sgl->raw + 8, len);        // 24-bit length
    nvmeof_wr32(sgl->raw + 11, rkey);      // 32-bit key
    sgl->raw[15] = nvmeof_sgl_make_type_subtype(NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK,
                                                NVMEOF_SGL_SUBTYPE_ADDRESS);
}

// "This command has no data" is STILL a descriptor, not an empty field.
//
// Linux's host marks a command with no data with a keyed SGL data block of length
// zero (drivers/nvme/host/rdma.c, nvme_rdma_set_sg_null):
//
//     sg->addr = 0; put_unaligned_le24(0, sg->length);
//     put_unaligned_le32(0, sg->key);
//     sg->type = NVME_KEY_SGL_FMT_DATA_DESC << 4;      /* 0x40 */
//
// and the target's RDMA transport switches on that byte for EVERY command:
//
//     case NVME_SGL_FMT_DATA_DESC:                     /* 0x0 */
//             switch (sgl->type & 0xf) {
//             case NVME_SGL_FMT_OFFSET: return ...inline...
//             default: pr_err("invalid SGL subtype: %#x\n", sgl->type);
//                      return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
//
// So a zeroed dptr (type 0x00, subtype 0x00) is Invalid Field - for Property Get,
// Property Set, Keep Alive, Set Features, Async Event, Identify without data.  Our
// own target ignores the descriptor when a command carries no data, so this went
// unnoticed until a real target answered six "invalid SGL subtype: 0x0" in a row and
// refused CC.EN=1, after which every later command was answered "while CC.EN == 0".
static inline void nvmeof_sgl_set_null(nvmeof_sgl* sgl) {
    for (int i = 0; i < NVMEOF_SGL_DESC_SIZE; i++) sgl->raw[i] = 0;
    sgl->raw[15] = nvmeof_sgl_make_type_subtype(NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK,
                                                NVMEOF_SGL_SUBTYPE_ADDRESS);
}

// Is this descriptor acceptable to a target's RDMA transport?  Mirrors
// nvmet_rdma_map_sgl (drivers/nvme/target/rdma.c), which accepts exactly two
// shapes for a non-write command: a keyed data block (subtype address, optionally
// with invalidate) and - for in-capsule data on a write - an unkeyed data block
// with subtype offset.  Returns 0 or the SCT/SC pair a real target answers with.
static inline int nvmeof_sgl_check_rdma(const uint8_t* dptr, int isWrite,
                                        uint8_t* sct, uint8_t* sc) {
    const uint8_t ts = dptr[15];
    const uint8_t type = nvmeof_sgl_type_of(ts);
    const uint8_t sub  = nvmeof_sgl_subtype_of(ts);
    if (type == NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK) {
        if (sub == NVMEOF_SGL_SUBTYPE_ADDRESS || sub == NVMEOF_SGL_SUBTYPE_INVALIDATE) return 0;
        *sct = NVMEOF_SCT_GENERIC; *sc = NVMEOF_SC_INVALID_FIELD; return 1;   /* invalid subtype */
    }
    if (type == NVMEOF_SGL_TYPE_DATA_BLOCK && sub == NVMEOF_SGL_SUBTYPE_OFFSET && isWrite) return 0;
    if (type == NVMEOF_SGL_TYPE_DATA_BLOCK) { *sct = NVMEOF_SCT_GENERIC; *sc = NVMEOF_SC_INVALID_FIELD; return 1; }
    *sct = NVMEOF_SCT_GENERIC; *sc = NVMEOF_SC_SGL_INVALID_TYPE; return 1;    /* unknown type */
}

// Build an unkeyed SGL descriptor (used for in-capsule data references).
static inline void nvmeof_sgl_set_unkeyed(nvmeof_sgl* sgl, uint64_t addr, uint32_t len,
                                          uint8_t type, uint8_t subtype) {
    nvmeof_wr64(sgl->raw, addr);
    nvmeof_wr32(sgl->raw + 8, len);
    sgl->raw[12] = sgl->raw[13] = sgl->raw[14] = 0;
    sgl->raw[15] = nvmeof_sgl_make_type_subtype(type, subtype);
}

#pragma pack(pop)

#endif // NVMEOF_WIRE_H
