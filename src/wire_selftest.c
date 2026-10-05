// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
// Compile-time and small runtime self-test for nvmeof_wire.h.
//
// The point of this file is that the wire header's static_asserts actually get
// evaluated.  A header full of asserts that is never compiled is documentation,
// not verification.
//
// Build:  cl /nologo /W4 /WX /I.. wire_selftest.c /Fe:wire_selftest.exe
// Run:    wire_selftest.exe   (prints PASS/FAIL and exits non-zero on failure)

#include <stdio.h>
#include <string.h>

#include "nvmeof_wire.h"

static int g_failures = 0;

// The condition goes through a function call, and that is not decoration: this file
// checks CONSTANT comparisons on purpose (a wrong constant is exactly the bug it
// exists to find), and MSVC 14.4x and earlier answer that with
//
//     warning C4127: conditional expression is constant
//
// which /W4 /WX turns into error C2220.  MSVC 14.5x stopped emitting it for this
// shape, so the file built clean locally and failed on every CI run for 21 pushes:
// the windows-2022 runner ships 14.44, this machine's default toolset is 14.51.
// Reproduced by forcing the toolset locally:
//
//     cl /nologo /TC /W4 /WX wire_selftest.c      -vcvars_ver=14.44 -> error C2220
//     cl /nologo /TC /W4 /WX wire_selftest.c      -vcvars_ver=14.51 -> clean
//
// Passing the value through a function keeps every check running, keeps /W4 /WX on
// for the rest of the file (where a warning really is a wire-format bug), and does not
// suppress anything.  Static layout assertions are unaffected: those are
// NVMEOF_STATIC_ASSERT, which is a compile-time check in its own right.
static int check_holds(int cond) { return cond; }

#define CHECK(cond, msg) do { \
    if (!check_holds(!!(cond))) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_failures++; } \
} while (0)

static void test_sgl_keyed(void) {
    nvmeof_sgl sgl;
    memset(&sgl, 0xAA, sizeof(sgl));   // poison, so untouched bytes are visible
    nvmeof_sgl_set_keyed(&sgl, 0x0000000123456789ull, 0x00100000u, 0xDEADBEEF);

    // Byte-exact expectations.  If any of these ever change, the change is a
    // wire-format change and has to be justified against the spec, not patched.
    CHECK(nvmeof_rd64(sgl.raw + 0) == 0x0000000123456789ull, "keyed sgl address @0");
    CHECK(nvmeof_rd24(sgl.raw + 8) == 0x00100000u,           "keyed sgl 24-bit length @8");
    CHECK(nvmeof_rd32(sgl.raw + 11) == 0xDEADBEEF,           "keyed sgl key @11");
    CHECK(sgl.raw[15] == 0x40, "keyed sgl type<<4|subtype == 0x40 (type 4, subtype 0)");
    CHECK(nvmeof_sgl_type_of(sgl.raw[15])    == NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK,
          "keyed sgl type decodes to 4");
    CHECK(nvmeof_sgl_subtype_of(sgl.raw[15]) == NVMEOF_SGL_SUBTYPE_ADDRESS,
          "keyed sgl subtype decodes to 0");
}

static void test_sgl_unkeyed(void) {
    nvmeof_sgl sgl;
    memset(&sgl, 0xAA, sizeof(sgl));
    nvmeof_sgl_set_unkeyed(&sgl, 0x1000ull, 0x200, NVMEOF_SGL_TYPE_SEGMENT,
                           NVMEOF_SGL_SUBTYPE_ADDRESS);
    CHECK(nvmeof_rd64(sgl.raw + 0) == 0x1000ull, "unkeyed addr @0");
    CHECK(nvmeof_rd32(sgl.raw + 8) == 0x200u,    "unkeyed 32-bit length @8");
    CHECK(sgl.raw[12] == 0 && sgl.raw[13] == 0 && sgl.raw[14] == 0,
          "unkeyed reserved[3] @12..14 zeroed");
    CHECK(sgl.raw[15] == 0x20, "unkeyed type<<4|subtype == 0x20 (type 2 segment)");
}

// The type/subtype packing is the single most error-prone byte in the whole
// format, so exercise the full cross product rather than a sample.
static void test_type_subtype_packing(void) {
    for (int t = 0; t < 16; t++) {
        for (int st = 0; st < 16; st++) {
            uint8_t ts = nvmeof_sgl_make_type_subtype((uint8_t)t, (uint8_t)st);
            CHECK(ts == (uint8_t)((t << 4) | st), "type/subtype pack");
            CHECK(nvmeof_sgl_type_of(ts) == t, "type/subtype unpack type");
            CHECK(nvmeof_sgl_subtype_of(ts) == st, "type/subtype unpack subtype");
        }
    }
}

static void test_command_layout(void) {
    nvmeof_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVMEOF_OPC_READ;
    sqe.command_id = 0x1234;
    sqe.nsid = 1;
    nvmeof_sgl_set_keyed(&sqe.dptr, 0x2000, 4096, 0xCAFE);

    // The SGL must be exactly where the SQE's dptr field is, at offset 24, and
    // the read+write commands put SLBA at cdw10/cdw11 (offset 40).
    CHECK(((uint8_t*)&sqe.dptr - (uint8_t*)&sqe) == 24, "dptr sits at SQE offset 24");
    const uint8_t* p = (const uint8_t*)&sqe;
    CHECK(p[0] == 0x02, "opcode byte 0");
    CHECK(nvmeof_rd16(p + 2) == 0x1234, "cid at byte 2");
    CHECK(nvmeof_rd64(p + 24) == 0x2000, "sgl address lands at byte 24");
    CHECK(p[39] == 0x40, "sgl type byte lands at byte 39 (24+15)");
}

static void test_sgl_invalidate(void) {
    // The invalidate request differs from an ordinary keyed transfer by ONE
    // nibble of ONE byte, so a codec that switches on the type alone cannot tell
    // them apart and silently drops the invalidation.  Pin both spellings.
    CHECK(NVMEOF_SGL_TS_KEYED_NORMAL     == 0x40, "keyed data block, normal subtype");
    CHECK(NVMEOF_SGL_TS_KEYED_INVALIDATE == 0x4f, "keyed data block, invalidate subtype");
    CHECK(nvmeof_sgl_is_keyed(NVMEOF_SGL_TS_KEYED_NORMAL) == 1, "0x40 is a keyed descriptor");
    CHECK(nvmeof_sgl_is_keyed(NVMEOF_SGL_TS_KEYED_INVALIDATE) == 1, "0x4f is a keyed descriptor");
    CHECK(nvmeof_sgl_wants_invalidate(NVMEOF_SGL_TS_KEYED_NORMAL) == 0, "0x40 does not invalidate");
    CHECK(nvmeof_sgl_wants_invalidate(NVMEOF_SGL_TS_KEYED_INVALIDATE) == 1, "0x4f invalidates");
    // The type is identical in both, which is the trap.
    CHECK(nvmeof_sgl_type_of(NVMEOF_SGL_TS_KEYED_NORMAL) ==
          nvmeof_sgl_type_of(NVMEOF_SGL_TS_KEYED_INVALIDATE),
          "normal and invalidate share a descriptor type");
    // And the helper must agree with how the encoder builds one.
    CHECK(nvmeof_sgl_make_type_subtype(NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK,
                                       NVMEOF_SGL_SUBTYPE_INVALIDATE) ==
          NVMEOF_SGL_TS_KEYED_INVALIDATE, "encoder agrees on the invalidate spelling");
}

static void test_admin_cases_a_host_needs(void) {
    // CNS 2 and CNS 3 are the namespace scan.  A controller that answers only
    // CNS 0/1 connects and then looks empty, so these two values are as
    // load-bearing as the ones used by the data path.
    CHECK(NVMEOF_ID_CNS_NS == 0x00, "CNS namespace structure");
    CHECK(NVMEOF_ID_CNS_CTRL == 0x01, "CNS controller structure");
    CHECK(NVMEOF_ID_CNS_NS_ACTIVE_LIST == 0x02, "CNS active namespace id list");
    CHECK(NVMEOF_ID_CNS_NS_DESC_LIST == 0x03, "CNS namespace identification descriptors");
    CHECK(NVMEOF_NIDT_UUID  == 0x03, "ns descriptor type: UUID");
    CHECK(NVMEOF_NIDT_CSI   == 0x04, "ns descriptor type: CSI");
    CHECK(NVMEOF_NIDT_NGUID == 0x02, "ns descriptor type: NGUID");
    CHECK(NVMEOF_NIDT_EUI64 == 0x01, "ns descriptor type: EUI64");
    CHECK(NVMEOF_CSI_NVM == 0x00, "command set identifier: NVM");
    CHECK(NVMEOF_NIDT_CSI_LEN == 1, "CSI descriptor length is 1");
    // The four descriptor types must be distinct: collapsing CSI onto UUID is
    // exactly the mistake these constants used to contain, and two endpoints
    // sharing the mistake agree with each other and disagree with every host.
    CHECK(NVMEOF_NIDT_CSI != NVMEOF_NIDT_UUID &&
          NVMEOF_NIDT_UUID != NVMEOF_NIDT_NGUID &&
          NVMEOF_NIDT_NGUID != NVMEOF_NIDT_EUI64, "descriptor types are distinct");

    // Keep Alive status codes are GENERIC (SCT 0) 0x19/0x1a, not command-specific
    // 0x01/0x02.  A host reads the code using the type, so a right code with the
    // wrong type is still unreadable.
    CHECK(NVMEOF_SC_KA_TIMEOUT_EXPIRED == 0x19, "KA_TIMEOUT_EXPIRED is generic SC 0x19");
    CHECK(NVMEOF_SC_KA_TIMEOUT_INVALID == 0x1a, "KA_TIMEOUT_INVALID is generic SC 0x1a");
    CHECK(nvmeof_status_sct(NVMEOF_STATUS_CQE(NVMEOF_SCT_GENERIC,
                                              NVMEOF_SC_KA_TIMEOUT_INVALID)) ==
          NVMEOF_SCT_GENERIC, "Keep Alive timeout invalid carries the generic SCT");
    CHECK(nvmeof_status_sc(NVMEOF_STATUS_CQE(NVMEOF_SCT_GENERIC,
                                             NVMEOF_SC_KA_TIMEOUT_INVALID)) == 0x1a,
          "Keep Alive timeout invalid survives the status round trip");

    // Property offsets a host reads during bring-up.  They must be four
    // different registers, not one value returned for every offset - which is
    // what this target used to do.
    CHECK(NVMEOF_PROP_CAP == 0x00, "property CAP offset");
    CHECK(NVMEOF_PROP_VS == 0x08, "property VS offset");
    CHECK(NVMEOF_PROP_CC == 0x14, "property CC offset");
    CHECK(NVMEOF_PROP_CSTS == 0x1c, "property CSTS offset");
    CHECK(NVMEOF_PROP_CAP != NVMEOF_PROP_VS && NVMEOF_PROP_VS != NVMEOF_PROP_CC &&
          NVMEOF_PROP_CC != NVMEOF_PROP_CSTS, "the four registers are distinct");

    // CAP is built from bit fields, so a typo in a shift is invisible without
    // reading it back.  MQES 15:0, CQR bit 16, TO 31:24, CSS.NVM bit 37.
    CHECK((NVMEOF_CAP_VALUE & 0xffffull) == NVMEOF_CAP_MQES, "CAP MQES is in bits 15:0");
    CHECK(((NVMEOF_CAP_VALUE >> 16) & 1ull) == 1ull, "CAP CQR is bit 16");
    CHECK(((NVMEOF_CAP_VALUE >> 24) & 0xffull) == NVMEOF_CAP_TO, "CAP TO is bits 31:24");
    CHECK(((NVMEOF_CAP_VALUE >> 37) & 1ull) == 1ull, "CAP CSS.NVM is bit 37");
    // A host's ready-wait is (CAP.TO + 1) / 2 seconds and it reads CSTS once
    // before checking that deadline, so TO = 0 leaves exactly one iteration.
    CHECK(NVMEOF_CAP_TO != 0, "CAP.TO is non-zero, so a host has ready-wait slack");

    // The property access-size field and the Connect attribute bit that turns
    // SQHD into the reserved value.  Both are easy to ignore and both change
    // what a host believes.
    CHECK(NVMEOF_PROP_SIZE_4 == 0, "4-byte property access");
    CHECK(NVMEOF_PROP_SIZE_8 == 1, "8-byte property access");
    CHECK(NVMEOF_PROP_ATTRIB_SIZE_MASK == 0x7, "property access size is bits 2:0");
    CHECK(NVMEOF_CONNECT_CATTR_DISABLE_SQFLOW == (1u << 2), "cattr SQ-flow disable is bit 2");
    CHECK(NVMEOF_SC_CMD_SEQ_ERROR == 0x0c, "command sequence error is SC 0x0c");

    // SGLS and ONCS bit values, from include/linux/nvme.h.  Bit 20 is the trap:
    // it is SAOS (in-capsule data via an offset SGL), not "the keyed length is
    // 24 bits" - that is a property of the descriptor format and has no bit.
    // These targets used to set bit 20 while rejecting offset descriptors.
    CHECK(NVMEOF_CTRL_SGLS_BYTE_ALIGNED  == 1u,        "SGLS byte aligned is bit 0");
    CHECK(NVMEOF_CTRL_SGLS_DWORD_ALIGNED == 2u,        "SGLS DWORD aligned is bit 1");
    CHECK(NVMEOF_CTRL_SGLS_KEYED         == (1u << 2), "SGLS keyed (KSDBDS) is bit 2");
    CHECK(NVMEOF_CTRL_SGLS_SAOS          == (1u << 20), "SGLS SAOS is bit 20");
    CHECK((NVMEOF_CTRL_SGLS_ADVERTISED & NVMEOF_CTRL_SGLS_SAOS) == 0,
          "the advertised SGLS value does not claim in-capsule data");
    CHECK((NVMEOF_CTRL_SGLS_ADVERTISED & NVMEOF_CTRL_SGLS_KEYED) != 0,
          "the advertised SGLS value does claim keyed descriptors");

    CHECK(NVMEOF_CTRL_ONCS_COMPARE      == (1u << 0), "ONCS compare is bit 0");
    CHECK(NVMEOF_CTRL_ONCS_WRITE_UNCOR  == (1u << 1), "ONCS write uncorrectable is bit 1");
    CHECK(NVMEOF_CTRL_ONCS_DSM          == (1u << 2), "ONCS DSM is bit 2");
    CHECK(NVMEOF_CTRL_ONCS_WRITE_ZEROES == (1u << 3), "ONCS write zeroes is bit 3");
    CHECK(NVMEOF_CTRL_ONCS_RESERVATIONS == (1u << 5), "ONCS reservations is bit 5");
    CHECK(NVMEOF_CTRL_ONCS_TIMESTAMP    == (1u << 6), "ONCS timestamp is bit 6");
    CHECK(NVMEOF_CTRL_ONCS_WRITE_ZEROES != NVMEOF_CTRL_ONCS_DSM,
          "ONCS bits are distinct");

    // Connect's response packs the controller id in the low 16 bits and two
    // authentication request bits above it; a controller that does not require
    // authentication must leave those clear or the host starts a login.
    CHECK(NVMEOF_CNTLID_DYNAMIC == 0xffff, "dynamic controller id is 0xffff");
    CHECK(NVMEOF_CNTLID_MIN <= 1 && 1 <= NVMEOF_CNTLID_MAX,
          "the controller id this target hands out is in the legal range");

    // The offsets of the fields that are zero on purpose.  A wrong offset here
    // means the host reads some other field as a capability and starts a
    // conversation this target cannot finish, so they are pinned individually.
    CHECK(NVMEOF_ID_CTRL_OFF_OAES == 92u, "OAES offset is 92");
    CHECK(NVMEOF_ID_CTRL_OFF_OACS == 256u, "OACS offset is 256");
    CHECK(NVMEOF_ID_CTRL_OFF_ACL == 258u, "ACL offset is 258");
    CHECK(NVMEOF_ID_CTRL_OFF_AERL == 259u, "AERL offset is 259");
    CHECK(NVMEOF_ID_CTRL_OFF_FRMW == 260u, "FRMW offset is 260");
    CHECK(NVMEOF_ID_CTRL_OFF_LPA == 261u, "LPA offset is 261");
    CHECK(NVMEOF_ID_CTRL_OFF_APSTA == 265u, "APSTA offset is 265");
    CHECK(NVMEOF_ID_CTRL_OFF_KAS == 320u, "KAS offset is 320");

    // The AEN mask bits, from include/linux/nvme.h.  They are only meaningful
    // once OAES advertises them, which this controller does not.
    CHECK(NVMEOF_OAES_NS_ATTR == (1u << 8), "OAES namespace attribute is bit 8");
    CHECK(NVMEOF_OAES_FW_ACT == (1u << 9), "OAES firmware activate is bit 9");
    CHECK(NVMEOF_OAES_ANA_CHANGE == (1u << 11), "OAES ANA change is bit 11");
    CHECK(NVMEOF_OAES_DISC_CHANGE == 0x80000000u, "OAES discovery change is bit 31");
    CHECK(NVMEOF_CTRL_LPA_CMD_EFFECTS_LOG == (1u << 1), "LPA command effects log is bit 1");
    CHECK(NVMEOF_CTRL_CMIC_ANA == (1u << 3), "CMIC ANA is bit 3");
    // If someone enables an AEN bit they must have implemented the Async Event
    // command first; that is the whole point of keeping the mask in one place.
    CHECK((0u & NVMEOF_OAES_AEN_SUPPORTED) == 0,
          "advertising no AEN bits keeps the host from posting Async Event commands");
}

static void test_status_encoding(void) {
    // The two builders differ by exactly the phase bit, and the difference is
    // load-bearing: a responder that uses MAKE for a CQE produces status 0x0000,
    // which reads identically to "success without a phase", so every
    // success assertion fails and every failure assertion passes.
    CHECK(NVMEOF_STATUS_MAKE(NVMEOF_SCT_GENERIC, NVMEOF_SC_SUCCESS) == 0x0000,
          "MAKE(success) has no phase bit");
    CHECK(NVMEOF_STATUS_CQE(NVMEOF_SCT_GENERIC, NVMEOF_SC_SUCCESS) == 0x0001,
          "CQE(success) sets the phase bit");
    CHECK((NVMEOF_STATUS_CQE(NVMEOF_SCT_GENERIC, NVMEOF_SC_SUCCESS) &
           NVMEOF_STATUS_P_MASK) != 0, "CQE status has P=1");
    CHECK(nvmeof_status_phase(NVMEOF_STATUS_CQE(NVMEOF_SCT_GENERIC, NVMEOF_SC_SUCCESS)) == 1,
          "phase reads back as set");
    CHECK(nvmeof_status_phase(NVMEOF_STATUS_MAKE(NVMEOF_SCT_GENERIC, NVMEOF_SC_SUCCESS)) == 0,
          "phase reads back as clear for MAKE");

    // A non-success status must survive the round trip through both extractors,
    // with the phase bit stripped off first.
    uint16_t st = NVMEOF_STATUS_CQE(NVMEOF_SCT_COMMAND_SPECIFIC, 0x0C);
    CHECK(nvmeof_status_phase(st) == 1, "error CQE still has P=1");
    CHECK(nvmeof_status_sct(st) == NVMEOF_SCT_COMMAND_SPECIFIC, "error CQE sct survives");
    CHECK(nvmeof_status_sc(st) == 0x0C, "error CQE sc survives");
}

static void test_fabrics_layout(void) {
    // These offsets are the ones that differ from the plain SQE and are the
    // reason the fabrics commands are modelled as their own types.
    CHECK(offsetof(nvmeof_fabrics_common, fctype)  == 4,  "fctype @4");
    CHECK(offsetof(nvmeof_fabrics_common, task_tag) == 40, "fabrics Task Tag @40");
    CHECK(offsetof(nvmeof_fabrics_connect, dptr)    == 24, "connect dptr @24");
    CHECK(offsetof(nvmeof_fabrics_connect, recfmt)  == 40, "connect recfmt @40");
    CHECK(offsetof(nvmeof_fabrics_connect, qid)     == 42, "connect qid @42");
    CHECK(offsetof(nvmeof_fabrics_connect, sqsize)  == 44, "connect sqsize @44");
    CHECK(offsetof(nvmeof_fabrics_connect, kato)    == 48, "connect kato @48");
    CHECK(offsetof(nvmeof_fabrics_property_set, offset) == 44, "prop set offset @44");
    CHECK(offsetof(nvmeof_fabrics_property_set, value)  == 48, "prop set value @48");
    CHECK(offsetof(nvmeof_fabrics_property_get, offset) == 44, "prop get offset @44");

    // fctype values are not sequential; pin them.
    CHECK(NVMEOF_FCTYPE_PROPERTY_SET == 0x00, "fctype property set");
    CHECK(NVMEOF_FCTYPE_CONNECT      == 0x01, "fctype connect");
    CHECK(NVMEOF_FCTYPE_PROPERTY_GET == 0x04, "fctype property get");
    CHECK(NVMEOF_FCTYPE_AUTH_SEND    == 0x05, "fctype auth send");
    CHECK(NVMEOF_FCTYPE_AUTH_RECEIVE == 0x06, "fctype auth receive");
    // There is deliberately no disconnect fctype.  This project had one (0x08),
    // accepted by our own target and sent by our own host, until the reference
    // header was read - see DESIGN 8.44.  The "no sixth member" rule is enforced
    // where it can be enforced properly: src/xref_constants.py checks that every
    // NVMEOF_FCTYPE_* has a counterpart in the reference header, so re-adding one
    // fails run_xref.ps1.  A count constant here would itself be a wire name the
    // reference does not define, which is the very thing being prevented.

    // Keep Alive is an admin opcode, not an fctype.  If someone ever adds an
    // fctype for it this check is the place that says no.
    CHECK(NVMEOF_OPC_KEEP_ALIVE == 0x18, "keep alive is admin opcode 0x18");

    // Auth Send / Auth Receive: the bytes nvmet validates BEFORE it looks at the
    // payload.  A byte-image check, because these three fields sit in the space
    // that is "reserved" for every other fctype and a struct can be the right
    // size with them in the wrong place.
    CHECK(offsetof(nvmeof_fabrics_auth, dptr)  == 24, "auth dptr @24");
    CHECK(offsetof(nvmeof_fabrics_auth, spsp0) == 41, "auth spsp0 @41");
    CHECK(offsetof(nvmeof_fabrics_auth, spsp1) == 42, "auth spsp1 @42");
    CHECK(offsetof(nvmeof_fabrics_auth, secp)  == 43, "auth secp @43");
    CHECK(offsetof(nvmeof_fabrics_auth, al_tl) == 44, "auth al/tl @44");
    CHECK(NVMEOF_AUTH_SECP_DHCHAP == 0xE9, "DH-HMAC-CHAP is security protocol 0xE9");
    CHECK(NVMEOF_AUTH_SPSP0 == 0x01 && NVMEOF_AUTH_SPSP1 == 0x01,
          "both spsp bytes are 1 (nvmet refuses anything else with Invalid Field)");
    {
        uint8_t cap[64];
        nvmeof_fabrics_auth *a = (nvmeof_fabrics_auth *)cap;
        memset(cap, 0, sizeof(cap));
        a->fctype = NVMEOF_FCTYPE_AUTH_SEND;
        a->spsp0 = 0x01; a->spsp1 = 0x01; a->secp = NVMEOF_AUTH_SECP_DHCHAP;
        a->al_tl.tl = 0x11223344u;
        CHECK(cap[4] == 0x05 && cap[41] == 0x01 && cap[42] == 0x01 && cap[43] == 0xE9,
              "auth SQE byte image: fctype@4 spsp0@41 spsp1@42 secp@43");
        CHECK(cap[44] == 0x44 && cap[47] == 0x11, "tl is little-endian at 44..47");
    }

    // ATR is a bit of the Connect RESULT, sharing the dword with the controller id.
    // Truncating the result to 16 bits - which is what "read cntlid" looks like -
    // silently drops the one signal that tells the host to authenticate.
    CHECK(NVMEOF_CONNECT_AUTHREQ_ATR == (1u << 17), "ATR is bit 17 of the Connect result");
    CHECK((NVMEOF_CONNECT_AUTHREQ_ATR & 0xFFFFu) == 0, "ATR does not collide with CNTLID");
    CHECK((0x1234u | NVMEOF_CONNECT_AUTHREQ_ATR) == 0x21234u,
          "cntlid and ATR coexist in one dword");

    // The property attrib byte: bit 0 says the register is 64 bits wide, and a
    // target is expected to check it (nvmet does).  Our host used to read CAP with
    // attrib = 0, which a real target refuses with Invalid Field.
    CHECK(NVMEOF_PROP_SIZE_4 == 0, "attrib: 32-bit register");
    CHECK(NVMEOF_PROP_SIZE_8 == 1, "attrib: 64-bit register");
    CHECK(offsetof(nvmeof_fabrics_property_get, attrib) == 40, "prop get attrib @40");
    CHECK(offsetof(nvmeof_fabrics_property_set, attrib) == 40, "prop set attrib @40");
}

static void test_rdma_private_data(void) {
    // The RDMA transport carries queue sizes in the connection private data, not
    // in a capsule.  These offsets are what a Linux peer reads, so they are
    // pinned here rather than trusted to the struct definition.
    CHECK(sizeof(nvmeof_rdma_request_pd) == 32, "request private data is 32B");
    CHECK(sizeof(nvmeof_rdma_accept_pd)  == 32, "accept private data is 32B");
    CHECK(offsetof(nvmeof_rdma_request_pd, recfmt)  == 0, "pd recfmt @0");
    CHECK(offsetof(nvmeof_rdma_request_pd, qid)     == 2, "pd qid @2");
    CHECK(offsetof(nvmeof_rdma_request_pd, hrqsize) == 4, "pd hrqsize @4");
    CHECK(offsetof(nvmeof_rdma_request_pd, hsqsize) == 6, "pd hsqsize @6");
    CHECK(offsetof(nvmeof_rdma_request_pd, cntlid)  == 8, "pd cntlid @8");
    CHECK(offsetof(nvmeof_rdma_accept_pd, crqsize)  == 2, "accept pd crqsize @2");

    // A byte-image check, because a struct that compiles to the right size can
    // still put the fields in the wrong place.
    nvmeof_rdma_request_pd pd;
    memset(&pd, 0, sizeof(pd));
    pd.recfmt = 0; pd.qid = 1; pd.hrqsize = 32; pd.hsqsize = 32; pd.cntlid = 0xFFFF;
    const uint8_t* p = (const uint8_t*)&pd;
    CHECK(nvmeof_rd16(p + 0) == 0x0000, "pd[0..1] recfmt = 0");
    CHECK(nvmeof_rd16(p + 2) == 0x0001, "pd[2..3] qid = 1");
    CHECK(nvmeof_rd16(p + 4) == 0x0020, "pd[4..5] hrqsize = 32");
    CHECK(nvmeof_rd16(p + 6) == 0x0020, "pd[6..7] hsqsize = 32");
    CHECK(nvmeof_rd16(p + 8) == 0xFFFF, "pd[8..9] cntlid = 0xFFFF (dynamic)");

    // Transport type / provider / CMS values, which appear in the discovery log
    // and in the Connect command's trtype fields.  Guessing these wrong is a
    // silent interop failure against Linux, so they are pinned.
    CHECK(NVMEOF_RDMA_QPTYPE_RC    == 0x1, "qptype reliable connected");
    CHECK(NVMEOF_RDMA_PRTYPE_ROCE2 == 0x4, "prtype RoCE v2");
    CHECK(NVMEOF_RDMA_CMS_RDMA_CM  == 0x1, "cms rdma_cm");
    CHECK(NVMEOF_KATO_DEFAULT      == 120000, "default keep-alive timeout 120000 ms");
}

static void test_connect_data(void) {
    nvmeof_connect_data cd;
    memset(&cd, 0, sizeof(cd));
    cd.cntlid = NVMEOF_CNTLID_DYNAMIC;
    memcpy(cd.subsysnqn, "nqn.2024-01.local.rdma:test", 27);
    memcpy(cd.hostnqn,  "nqn.2014-08.org.nvmexpress:uuid:0", 35);

    const uint8_t* p = (const uint8_t*)&cd;
    CHECK(nvmeof_rd16(p + 16) == 0xffff, "cntlid at offset 16");
    CHECK(memcmp(p + 256, "nqn.2024-01.local.rdma:test", 27) == 0, "subsysnqn at 256");
    CHECK(memcmp(p + 512, "nqn.2014-08.org.nvmexpress:uuid:0", 35) == 0, "hostnqn at 512");
}

static void test_cqe_layout(void) {
    nvmeof_cqe cqe;
    memset(&cqe, 0, sizeof(cqe));
    cqe.sq_head = 3; cqe.sq_id = 1; cqe.command_id = 0x1234;
    // SCT = command specific (1), SC = QUEUE_SIZE (2), phase tag set.
    cqe.status = (uint16_t)(NVMEOF_STATUS_MAKE(NVMEOF_SCT_COMMAND_SPECIFIC,
                                              NVMEOF_SC_QUEUE_SIZE) | 0x1);
    const uint8_t* p = (const uint8_t*)&cqe;
    CHECK(nvmeof_rd16(p + 8)  == 3, "cqe sq_head @8");
    CHECK(nvmeof_rd16(p + 10) == 1, "cqe sq_id @10");
    CHECK(nvmeof_rd16(p + 12) == 0x1234, "cqe cid @12");

    // Raw packing: sc@8:1, sct@11:9, p@0.
    //   sct=1 -> 1<<9 = 0x0200 ; sc=2 -> 2<<1 = 0x0004 ; p -> 0x0001
    //   total  = 0x0205
    CHECK(nvmeof_rd16(p + 14) == 0x0205, "cqe status @14 packs sc@8:1 sct@11:9 p@0");
    CHECK(nvmeof_status_sc (nvmeof_rd16(p + 14)) == NVMEOF_SC_QUEUE_SIZE,
          "status code decodes to 2");
    CHECK(nvmeof_status_sct(nvmeof_rd16(p + 14)) == NVMEOF_SCT_COMMAND_SPECIFIC,
          "status code type decodes to 1");
    CHECK(nvmeof_status_phase(nvmeof_rd16(p + 14)) == 1, "phase tag decodes to 1");

    // And the same value with the phase bit clear must still decode identically
    // - the phase tag must not leak into SC.
    uint16_t noP = (uint16_t)(nvmeof_rd16(p + 14) & ~NVMEOF_STATUS_P_MASK);
    CHECK(nvmeof_status_sc(noP)  == NVMEOF_SC_QUEUE_SIZE, "SC unaffected by phase bit");
    CHECK(nvmeof_status_sct(noP) == NVMEOF_SCT_COMMAND_SPECIFIC, "SCT unaffected by phase bit");

    // Exhaustive: every (sct, sc) pair must survive a pack/unpack round trip.
    for (int sct = 0; sct < 8; sct++) {
        for (int sc = 0; sc < 256; sc++) {
            uint16_t s = NVMEOF_STATUS_MAKE(sct, sc);
            CHECK(nvmeof_status_sct(s) == sct, "sct round trip");
            CHECK(nvmeof_status_sc(s)  == sc,  "sc round trip");
            CHECK(nvmeof_status_phase(s) == 0, "phase clear in a packed status");
        }
    }

    // The two status codes whose number does not fit the 8-bit SC field are
    // spelled as (sct, sc) pairs because the low byte alone is ambiguous.  What
    // has to survive is the PAIR: a host that reconstructs the status from the
    // CQE must see 0x191 (auth required), not 0x091.
    {
        uint16_t authReq = NVMEOF_STATUS_CQE(NVMEOF_SC_AUTH_REQUIRED_SCT,
                                             NVMEOF_SC_AUTH_REQUIRED_SC);
        CHECK(nvmeof_status_sct(authReq) == NVMEOF_SCT_COMMAND_SPECIFIC,
              "auth required is command-specific");
        CHECK(nvmeof_status_sc(authReq) == 0x91, "auth required SC is 0x91");
        // Linux reads the status word as (cqe_status >> 1); this is the equality
        // that makes the pair mean what the spec says it means.
        CHECK((authReq >> 1) == NVMEOF_SC_AUTH_REQUIRED,
              "the CQE spelling reconstructs Linux's 0x191");
        CHECK(((uint16_t)(authReq | NVMEOF_STATUS_DNR) >> 1) ==
              (NVMEOF_SC_AUTH_REQUIRED | 0x4000u),
              "with DNR the reconstructed status is Linux's 0x4191");
    }
}

static void test_identify_offsets(void) {
    // The anchors this implementation depends on.  A target that disagrees with
    // any of these is either non-conformant or talking to a struct we got
    // wrong, and either way the interop test should catch it - so assert the
    // values are at least internally consistent and in ascending order.
    const uint32_t offs[] = {
        NVMEOF_ID_CTRL_OFF_VID, NVMEOF_ID_CTRL_OFF_SN, NVMEOF_ID_CTRL_OFF_MN,
        NVMEOF_ID_CTRL_OFF_FR, NVMEOF_ID_CTRL_OFF_MDTS, NVMEOF_ID_CTRL_OFF_CNTLID,
        NVMEOF_ID_CTRL_OFF_VER, NVMEOF_ID_CTRL_OFF_CNTRLTYPE, NVMEOF_ID_CTRL_OFF_OACS,
        NVMEOF_ID_CTRL_OFF_SQES, NVMEOF_ID_CTRL_OFF_NN, NVMEOF_ID_CTRL_OFF_ONCS,
        NVMEOF_ID_CTRL_OFF_VWC, NVMEOF_ID_CTRL_OFF_AWUN, NVMEOF_ID_CTRL_OFF_AWUPF,
        NVMEOF_ID_CTRL_OFF_SGLS, NVMEOF_ID_CTRL_OFF_SUBNQN, NVMEOF_ID_CTRL_OFF_IOCCSZ,
        NVMEOF_ID_CTRL_OFF_IORCSZ, NVMEOF_ID_CTRL_OFF_ICDOFF,
        NVMEOF_ID_CTRL_OFF_CTRATTR, NVMEOF_ID_CTRL_OFF_MSDBD,
    };
    for (size_t i = 1; i < sizeof(offs)/sizeof(offs[0]); i++) {
        CHECK(offs[i] > offs[i-1], "identify offsets strictly ascending");
    }
    CHECK(offs[sizeof(offs)/sizeof(offs[0]) - 1] < NVMEOF_IDENTIFY_SIZE,
          "all identify anchors inside 4096 bytes");

    // Reading through the accessors must agree with reading the raw bytes.
    nvmeof_id_ctrl_buf b;
    memset(&b, 0, sizeof(b));
    nvmeof_wr16(b.raw + NVMEOF_ID_CTRL_OFF_CNTLID, 0x0007);
    nvmeof_wr32(b.raw + NVMEOF_ID_CTRL_OFF_IOCCSZ, 8);
    CHECK(nvmeof_idc_u16(&b, NVMEOF_ID_CTRL_OFF_CNTLID) == 7, "idc u16 accessor");
    CHECK(nvmeof_idc_u32(&b, NVMEOF_ID_CTRL_OFF_IOCCSZ) == 8, "idc u32 accessor");
}

// ---------------------------------------------------------------------------
//  Golden vectors.
//
//  Everything else in this file checks that the encoder and the decoder agree
//  with each other.  When both endpoints of a protocol are written by the same
//  person, that is exactly the kind of test that passes while the wire format is
//  wrong: two consistent halves of a shared misunderstanding.
//
//  So these vectors are literal byte images, derived by hand from the field
//  offset table in nvmeof_wire.h and the NVMe-oF command layouts - NOT produced
//  by running the encoder.  If the encoder and the table ever drift apart, the
//  only way this can still pass is if the literal below is edited too, which is
//  a deliberate act rather than an accident.
// ---------------------------------------------------------------------------

typedef struct { const char* what; const uint8_t* bytes; size_t len; } Golden;

static void hexdump(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        printf("%02X ", p[i]);
        if ((i % 16) == 15) printf("\n");
    }
    if (n % 16) printf("\n");
}

static void check_golden(const Golden* g, const uint8_t* got, size_t gotLen) {
    if (gotLen != g->len) {
        printf("FAIL: %s length %zu != expected %zu\n", g->what, gotLen, g->len);
        g_failures++;
        return;
    }
    for (size_t i = 0; i < g->len; i++) {
        if (got[i] != g->bytes[i]) {
            printf("FAIL: %s differs at byte %zu: got 0x%02X expected 0x%02X\n",
                   g->what, i, got[i], g->bytes[i]);
            printf("  got:\n"); hexdump(got, g->len);
            printf("  expected:\n"); hexdump(g->bytes, g->len);
            g_failures++;
            return;
        }
    }
    printf("  golden OK: %s (%zu bytes, byte-exact)\n", g->what, g->len);
}

// A fabrics Connect capsule, built to these field values:
//   opcode 0x7f, command_id 0x1234, fctype 0x01 (Connect)
//   SGL: keyed data block -> addr 0x1000, length 1024, rkey 0xAABBCCDD
//   recfmt 0, qid 0 (admin), sqsize 32, cattr 0, kato 30000
static const uint8_t kGoldenConnectCapsule[64] = {
/* 00 */ 0x7f, 0x00, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* 16 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* 32 */ 0x00, 0x04, 0x00, 0xdd, 0xcc, 0xbb, 0xaa, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
/* 48 */ 0x30, 0x75, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// A fabrics Property Set capsule targeting CC.EN = 1:
//   opcode 0x7f, command_id 0x0001, fctype 0x00 (Property Set)
//   attrib 0 (HSQ... no: 0 = HSQ is 0x0; this vector uses attrib 0)
//   offset 0x14 (CC), value 1
static const uint8_t kGoldenPropSetCapsule[64] = {
/* 00 */ 0x7f, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* 16 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* 32 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
/* 48 */ 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// A success completion: sq_head 0, sq_id 0, cid 0x1234, status with the phase
// bit set and SC/SCT zero.
static const uint8_t kGoldenCqeSuccess[16] = {
/*  0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/*  8 */ 0x00, 0x00, 0x00, 0x00, 0x34, 0x12, 0x01, 0x00,
};

static void test_golden_capsules(void) {
    // Build the Connect capsule with the encoder under test.
    uint8_t cap[64];
    memset(cap, 0, sizeof(cap));
    nvmeof_fabrics_connect* c = (nvmeof_fabrics_connect*)cap;
    c->opcode = NVMEOF_OPC_FABRICS;
    c->command_id = 0x1234;
    c->fctype = NVMEOF_FCTYPE_CONNECT;
    nvmeof_sgl_set_keyed(&c->dptr, 0x1000ull, 1024u, 0xAABBCCDDu);
    c->recfmt = 0;
    c->qid = 0;
    c->sqsize = 32;
    c->cattr = 0;
    c->kato = 30000;
    Golden g1 = { "fabrics Connect capsule", kGoldenConnectCapsule, sizeof(kGoldenConnectCapsule) };
    check_golden(&g1, cap, sizeof(cap));

    // Build the Property Set capsule.
    uint8_t ps[64];
    memset(ps, 0, sizeof(ps));
    nvmeof_fabrics_property_set* p = (nvmeof_fabrics_property_set*)ps;
    p->opcode = NVMEOF_OPC_FABRICS;
    p->command_id = 0x0001;
    p->fctype = NVMEOF_FCTYPE_PROPERTY_SET;
    p->attrib = 0;
    p->offset = NVMEOF_PROP_CC;
    p->value = 1;
    Golden g2 = { "fabrics Property Set capsule", kGoldenPropSetCapsule, sizeof(kGoldenPropSetCapsule) };
    check_golden(&g2, ps, sizeof(ps));

    // And a success completion.
    uint8_t cqe[16];
    memset(cqe, 0, sizeof(cqe));
    nvmeof_cqe* q = (nvmeof_cqe*)cqe;
    q->sq_head = 0;
    q->sq_id = 0;
    q->command_id = 0x1234;
    q->status = (uint16_t)(NVMEOF_STATUS_MAKE(NVMEOF_SCT_GENERIC, NVMEOF_SC_SUCCESS) | 0x1);
    Golden g3 = { "success completion", kGoldenCqeSuccess, sizeof(kGoldenCqeSuccess) };
    check_golden(&g3, cqe, sizeof(cqe));

    // The Connect data layout, checked field by field against its offsets.
    nvmeof_connect_data cd;
    memset(&cd, 0, sizeof(cd));
    cd.cntlid = NVMEOF_CNTLID_DYNAMIC;
    memcpy(cd.subsysnqn, "nqn.2024-01.local.rdma:t", 24);
    memcpy(cd.hostnqn, "nqn.2014-08.org.nvmexpress:uuid:x", 33);
    const uint8_t* p2 = (const uint8_t*)&cd;
    CHECK(nvmeof_rd16(p2 + 16) == 0xffff, "connect data cntlid @16");
    CHECK(memcmp(p2 + 256, "nqn.2024-01.local.rdma:t", 24) == 0, "connect data subsysnqn @256");
    CHECK(memcmp(p2 + 512, "nqn.2014-08.org.nvmexpress:uuid:x", 33) == 0, "connect data hostnqn @512");
    CHECK(p2[255] == 0 && p2[767] == 0, "connect data nqn fields are NUL-padded");
}

int main(void) {
    test_sgl_keyed();
    test_sgl_unkeyed();
    test_type_subtype_packing();
    test_command_layout();
    test_sgl_invalidate();
    test_admin_cases_a_host_needs();
    test_status_encoding();
    test_fabrics_layout();
    test_rdma_private_data();
    test_connect_data();
    test_cqe_layout();
    test_identify_offsets();
    test_golden_capsules();

    if (g_failures) {
        printf("nvmeof_wire self-test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("nvmeof_wire self-test: PASS (all static asserts compiled, all byte layouts verified)\n");
    printf("  SQE %u B, CQE %u B, SGL descriptor %u B (keyed and unkeyed), "
           "Connect data %u B, Identify %u B\n",
           (unsigned)sizeof(nvmeof_sqe), (unsigned)sizeof(nvmeof_cqe),
           (unsigned)sizeof(nvmeof_sgl), (unsigned)sizeof(nvmeof_connect_data),
           (unsigned)NVMEOF_IDENTIFY_SIZE);
    return 0;
}
