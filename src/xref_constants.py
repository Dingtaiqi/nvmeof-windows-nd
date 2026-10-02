#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Cross-check our NVMe wire constants against the Linux UAPI header.

WHY THIS EXISTS
---------------
Four separate bugs in this project were the same mistake: a constant was written
from memory instead of read from a header, and because our target and our own
test shared the wrong value, every assertion passed.  The values found that way
were NVMEOF_NIDT_CSI (0x03, actually 0x04), the Keep Alive status codes
(0x01/0x02, actually generic 0x1a/0x19), NVMEOF_CTRL_SGLS_24BIT_LEN (a fabricated
name for bit 20, which is SAOS), and NVMEOF_CTRL_ONCS_VERIFY (not an ONCS bit).

Reading the header carefully once is what found them.  Reading it carefully is
also what a person stops doing.  This script does the reading instead: it pulls
named constants out of the reference header and out of ours and compares the
integer values, so the check can be re-run whenever either side changes rather
than depending on someone's attention.

It is deliberately dumb - a text scanner, not a C parser - because a dumb scanner
that is always run beats a clever one that is not.

PROVENANCE
----------
ref/linux_nvme.h is include/linux/nvme.h from the mainline Linux tree, fetched
over the network and stored verbatim so this check is reproducible and its source
is inspectable.  It is a reference copy, not code we build.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent      # nvmeof/
REF = ROOT / "ref" / "linux_nvme.h"
REF_RDMA = ROOT / "ref" / "linux_nvme_rdma.h"
OURS = ROOT / "src" / "nvmeof_wire.h"
# The DH-HMAC-CHAP message ids live in their own header, and they are exactly the
# kind of value §8.44 is about: a wrong auth_id is answered with Failure1, which
# looks identical to a wrong HMAC.  So they get checked against the reference too,
# from a second "ours" file with its own pair list.
OURS_AUTH = ROOT / "src" / "nvmeof_dhchap.h"

# (reference name, our name, why it matters)
PAIRS = [
    # --- fabrics command types: not sequential, and every host reads them ---
    ("nvme_fabrics_type_property_set", "NVMEOF_FCTYPE_PROPERTY_SET", "fabrics property set"),
    ("nvme_fabrics_type_connect",      "NVMEOF_FCTYPE_CONNECT",      "fabrics connect"),
    ("nvme_fabrics_type_property_get", "NVMEOF_FCTYPE_PROPERTY_GET", "fabrics property get"),
    ("nvme_fabrics_type_auth_send",    "NVMEOF_FCTYPE_AUTH_SEND",    "fabrics auth send"),
    ("nvme_fabrics_type_auth_receive", "NVMEOF_FCTYPE_AUTH_RECEIVE", "fabrics auth receive"),

    # --- Connect / queue model ---
    ("NVME_CONNECT_DISABLE_SQFLOW", "NVMEOF_CONNECT_CATTR_DISABLE_SQFLOW", "cattr SQ-flow disable"),
    ("NVME_CNTLID_DYNAMIC",         "NVMEOF_CNTLID_DYNAMIC",   "dynamic controller id"),
    ("NVME_CNTLID_MIN",             "NVMEOF_CNTLID_MIN",       "legal controller id floor"),
    ("NVME_CNTLID_MAX",             "NVMEOF_CNTLID_MAX",       "legal controller id ceiling"),

    # --- DH-HMAC-CHAP: the three values nvmet validates before reading a payload,
    #     and the bit that tells a host to authenticate at all.
    ("NVME_CONNECT_AUTHREQ_ATR",            "NVMEOF_CONNECT_AUTHREQ_ATR", "Connect result bit 17"),
    ("NVME_AUTH_DHCHAP_PROTOCOL_IDENTIFIER","NVMEOF_AUTH_SECP_DHCHAP",    "security protocol 0xE9"),
    ("NVME_SC_AUTH_REQUIRED",               "NVMEOF_SC_AUTH_REQUIRED",    "SC auth required 0x191"),

    # --- Identify CNS: CNS 2 and 3 are the namespace scan ---
    ("NVME_ID_CNS_NS",              "NVMEOF_ID_CNS_NS",              "identify namespace"),
    ("NVME_ID_CNS_CTRL",            "NVMEOF_ID_CNS_CTRL",            "identify controller"),
    ("NVME_ID_CNS_NS_ACTIVE_LIST",  "NVMEOF_ID_CNS_NS_ACTIVE_LIST",  "active namespace id list"),
    ("NVME_ID_CNS_NS_DESC_LIST",    "NVMEOF_ID_CNS_NS_DESC_LIST",    "namespace descriptors"),

    # --- Namespace identification descriptor types: CSI is 0x04, not 0x03 ---
    ("NVME_NIDT_EUI64", "NVMEOF_NIDT_EUI64", "ns descriptor EUI64"),
    ("NVME_NIDT_NGUID", "NVMEOF_NIDT_NGUID", "ns descriptor NGUID"),
    ("NVME_NIDT_UUID",  "NVMEOF_NIDT_UUID",  "ns descriptor UUID"),
    ("NVME_NIDT_CSI",   "NVMEOF_NIDT_CSI",   "ns descriptor CSI"),
    ("NVME_NIDT_CSI_LEN", "NVMEOF_NIDT_CSI_LEN", "CSI descriptor length"),
    ("NVME_CSI_NVM",    "NVMEOF_CSI_NVM",    "command set identifier NVM"),

    # --- Capability bits: bit 20 is SAOS, not a length property ---
    ("NVME_CTRL_SGLS_BYTE_ALIGNED",  "NVMEOF_CTRL_SGLS_BYTE_ALIGNED",  "SGLS byte aligned"),
    ("NVME_CTRL_SGLS_DWORD_ALIGNED", "NVMEOF_CTRL_SGLS_DWORD_ALIGNED", "SGLS DWORD aligned"),
    ("NVME_CTRL_SGLS_KSDBDS",        "NVMEOF_CTRL_SGLS_KEYED",         "SGLS keyed descriptors"),
    ("NVME_CTRL_SGLS_SAOS",          "NVMEOF_CTRL_SGLS_SAOS",          "SGLS in-capsule data"),
    ("NVME_CTRL_LPA_CMD_EFFECTS_LOG", "NVMEOF_CTRL_LPA_CMD_EFFECTS_LOG", "LPA effects log"),
    ("NVME_CTRL_CMIC_ANA",           "NVMEOF_CTRL_CMIC_ANA",           "CMIC ANA"),
    ("NVME_CTRL_VWC_PRESENT",        "NVMEOF_CTRL_VWC_PRESENT",        "volatile write cache"),
    ("NVME_CTRL_ONCS_DSM",           "NVMEOF_CTRL_ONCS_DSM",           "ONCS DSM"),
    ("NVME_CTRL_ONCS_WRITE_ZEROES",  "NVMEOF_CTRL_ONCS_WRITE_ZEROES",  "ONCS write zeroes"),
    # The namespace-side half of the same promise: without this bit a host has no
    # reason to believe a deallocate defined anything, so discard stays disabled even
    # with ONCS.DSM set.  It was missing from the header AND from this table until the
    # target started advertising deallocate at all (DESIGN 8.74).
    ("NVME_NS_FEAT_THIN",            "NVMEOF_NS_FEAT_THIN",            "nsfeat thin provisioning"),

    # --- Optional async events: this is what gates whether a host posts an AEN ---
    ("NVME_AEN_CFG_NS_ATTR",     "NVMEOF_OAES_NS_ATTR",     "OAES namespace attribute"),
    ("NVME_AEN_CFG_FW_ACT",      "NVMEOF_OAES_FW_ACT",      "OAES firmware activate"),
    ("NVME_AEN_CFG_ANA_CHANGE",  "NVMEOF_OAES_ANA_CHANGE",  "OAES ANA change"),
    ("NVME_AEN_CFG_DISC_CHANGE", "NVMEOF_OAES_DISC_CHANGE", "OAES discovery change"),

    # --- Features a host sets ---
    ("NVME_FEAT_NUM_QUEUES",  "NVMEOF_FID_NUM_QUEUES",  "feature: number of queues"),
    ("NVME_FEAT_ASYNC_EVENT", "NVMEOF_FID_ASYNC_EVENT", "feature: async event config"),
    ("NVME_FEAT_KATO",        "NVMEOF_FID_KATO",        "feature: keep alive timer"),
    ("NVME_FEAT_VOLATILE_WC", "NVMEOF_FID_VWC",         "feature: volatile write cache"),
    ("NVME_FEAT_TEMP_THRESH", "NVMEOF_FID_TEMP_THRESH", "feature: temperature threshold (0x04)"),

    # --- CC / CSTS bits a fabrics host writes and polls ---
    ("NVME_CC_SHN_MASK",      "NVMEOF_CC_SHN_MASK",     "CC shutdown-notification field"),
    ("NVME_CSTS_RDY",         "NVMEOF_CSTS_RDY",        "CSTS ready"),
    ("NVME_CSTS_CFS",         "NVMEOF_CSTS_CFS",        "CSTS controller fatal status"),
    ("NVME_CSTS_SHST_CMPLT",  "NVMEOF_CSTS_SHST_CMPLT", "CSTS shutdown complete"),

    # --- Status codes: SCT and SC both matter ---
    ("NVME_SC_INVALID_OPCODE",     "NVMEOF_SC_INVALID_OPCODE",     "SC invalid opcode"),
    ("NVME_SC_INVALID_FIELD",      "NVMEOF_SC_INVALID_FIELD",      "SC invalid field"),
    ("NVME_SC_CMDID_CONFLICT",     "NVMEOF_SC_CMDID_CONFLICT",     "SC command id conflict"),
    ("NVME_SC_DATA_XFER_ERROR",    "NVMEOF_SC_DATA_XFER_ERROR",    "SC data transfer error"),
    ("NVME_SC_INVALID_NS",         "NVMEOF_SC_INVALID_NS",         "SC invalid namespace"),
    ("NVME_SC_SGL_INVALID_TYPE",   "NVMEOF_SC_SGL_INVALID_TYPE",   "SC SGL invalid type"),
    ("NVME_SC_LBA_RANGE",          "NVMEOF_SC_LBA_RANGE",          "SC LBA out of range"),
    ("NVME_SC_CMD_SEQ_ERROR",      "NVMEOF_SC_CMD_SEQ_ERROR",      "SC command sequence error"),
    ("NVME_SC_KA_TIMEOUT_EXPIRED", "NVMEOF_SC_KA_TIMEOUT_EXPIRED", "SC keep alive expired"),
    ("NVME_SC_KA_TIMEOUT_INVALID", "NVMEOF_SC_KA_TIMEOUT_INVALID", "SC keep alive invalid"),

    # --- SGL descriptor type/subtype nibbles ---
    ("NVME_SGL_FMT_ADDRESS",      "NVMEOF_SGL_SUBTYPE_ADDRESS",    "SGL subtype address"),
    ("NVME_SGL_FMT_INVALIDATE",   "NVMEOF_SGL_SUBTYPE_INVALIDATE", "SGL subtype invalidate"),
    ("NVME_SGL_FMT_DATA_DESC",    "NVMEOF_SGL_TYPE_DATA_BLOCK",    "SGL type data block"),
    ("NVME_KEY_SGL_FMT_DATA_DESC", "NVMEOF_SGL_TYPE_KEYED_DATA_BLOCK", "SGL type keyed"),
    ("NVME_SGL_FMT_SEG_DESC",     "NVMEOF_SGL_TYPE_SEGMENT",       "SGL type segment"),
    ("NVME_SGL_FMT_LAST_SEG_DESC", "NVMEOF_SGL_TYPE_LAST_SEGMENT", "SGL type last segment"),
    ("NVME_TRANSPORT_SGL_DATA_DESC", "NVMEOF_SGL_TYPE_TRANSPORT_DATA", "SGL type transport"),

    # --- Namespace identify field values ---
    ("NVME_NS_NMIC_SHARED",  "NVMEOF_NS_NMIC_SHARED",  "namespace shared"),
    ("NVME_NS_ATTR_RO",      "NVMEOF_NS_ATTR_RO",      "namespace read-only"),
    ("NVME_NS_FLBAS_LBA_MASK", "NVMEOF_ID_NS_FLBAS_LBA_MASK", "FLBAS index mask"),

    # --- Register offsets a host reads through Property Get ---
    ("NVME_REG_CAP",  "NVMEOF_PROP_CAP",  "property CAP offset"),
    ("NVME_REG_VS",   "NVMEOF_PROP_VS",   "property VS offset"),
    ("NVME_REG_CC",   "NVMEOF_PROP_CC",   "property CC offset"),
    ("NVME_REG_CSTS", "NVMEOF_PROP_CSTS", "property CSTS offset"),

    # --- Admin queue depth, enforced on the Connect private data ---
    ("NVME_AQ_DEPTH", "NVMEOF_AQ_DEPTH", "admin queue entries a fabrics target enforces"),
]

# The DH-HMAC-CHAP wire values.  Every one of these is a byte on the wire, and a
# wrong one is indistinguishable from "the secret does not match" at the far end:
# the reference answers with Failure1 and no explanation.  Checked against
# nvmeof_dhchap.h, which spells them kFoo rather than NVMEOF_FOO because they are
# not part of the transport wire header.
PAIRS_AUTH = [
    ("NVME_AUTH_DHCHAP_PROTOCOL_IDENTIFIER", "kSecpDhchap",    "security protocol id"),
    ("NVME_AUTH_COMMON_MESSAGES",            "kAuthTypeCommon","auth_type: common messages"),
    ("NVME_AUTH_DHCHAP_MESSAGES",            "kAuthTypeDhchap","auth_type: DH-HMAC-CHAP"),
    ("NVME_AUTH_DHCHAP_MESSAGE_NEGOTIATE",   "kMsgNegotiate",  "auth_id negotiate"),
    ("NVME_AUTH_DHCHAP_MESSAGE_CHALLENGE",   "kMsgChallenge",  "auth_id challenge"),
    ("NVME_AUTH_DHCHAP_MESSAGE_REPLY",       "kMsgReply",      "auth_id reply"),
    ("NVME_AUTH_DHCHAP_MESSAGE_SUCCESS1",    "kMsgSuccess1",   "auth_id success1"),
    ("NVME_AUTH_DHCHAP_MESSAGE_SUCCESS2",    "kMsgSuccess2",   "auth_id success2"),
    ("NVME_AUTH_DHCHAP_MESSAGE_FAILURE2",    "kMsgFailure2",   "auth_id failure2 (0xF0)"),
    ("NVME_AUTH_DHCHAP_MESSAGE_FAILURE1",    "kMsgFailure1",   "auth_id failure1 (0xF1)"),
    ("NVME_AUTH_HASH_SHA256",                "kHashIdSha256",  "hash id SHA-256"),
    ("NVME_AUTH_HASH_SHA384",                "kHashIdSha384",  "hash id SHA-384"),
    ("NVME_AUTH_HASH_SHA512",                "kHashIdSha512",  "hash id SHA-512"),
    ("NVME_AUTH_DHCHAP_FAILURE_REASON_FAILED","kFailureFailed", "Failure1 rescode: FAILED"),
    ("NVME_AUTH_DHCHAP_FAILURE_NOT_USABLE",  "kFailureNotUsable",       "Failure1 rescode_exp: NOT_USABLE"),
    ("NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE","kFailureHashUnusable",   "Failure1 rescode_exp: HASH_UNUSABLE"),
    ("NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE","kFailureDhgroupUnusable","Failure1 rescode_exp: DHGROUP_UNUSABLE"),
    ("NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD","kFailureIncorrectPayload","Failure1 rescode_exp: INCORRECT_PAYLOAD"),
    ("NVME_AUTH_DHCHAP_FAILURE_INCORRECT_MESSAGE","kFailureIncorrectMessage","Failure1 rescode_exp: INCORRECT_MESSAGE"),
    ("NVME_AUTH_DHCHAP_MAX_HASH_IDS",        "kMaxHashIds",    "30 hash ids in the idlist"),
    ("NVME_AUTH_DHCHAP_MAX_DH_IDS",          "kMaxDhIds",      "30 DH ids in the idlist"),
]

# The RDMA transport's own header.  These are the values exchanged in the
# RDMA-CM private data, which is where three real interoperability bugs lived:
# a Connect with no private data, an hsqsize sent 1-based, and an error code
# table that has to match what a Linux target sends back in its Reject.
PAIRS_RDMA = [
    ("NVME_RDMA_CM_FMT_1_0",           "NVMEOF_RDMA_CM_FMT_1_0",           "private-data record format"),
    ("NVME_RDMA_CM_INVALID_LEN",       "NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH", "reject: no/!private data"),
    ("NVME_RDMA_CM_INVALID_RECFMT",    "NVMEOF_RDMA_ERROR_INVALID_RECFMT", "reject: bad recfmt"),
    ("NVME_RDMA_CM_INVALID_QID",       "NVMEOF_RDMA_ERROR_INVALID_QID",    "reject: bad qid"),
    ("NVME_RDMA_CM_INVALID_HSQSIZE",   "NVMEOF_RDMA_ERROR_INVALID_HSQSIZE","reject: host SQ size"),
    ("NVME_RDMA_CM_INVALID_HRQSIZE",   "NVMEOF_RDMA_ERROR_INVALID_HRQSIZE","reject: host RQ size"),
    ("NVME_RDMA_CM_NO_RSC",            "NVMEOF_RDMA_ERROR_NO_RESOURCES",   "reject: out of resources"),
    ("NVME_RDMA_CM_INVALID_IRD",       "NVMEOF_RDMA_ERROR_INVALID_IRD",    "reject: invalid IRD"),
    ("NVME_RDMA_CM_INVALID_ORD",       "NVMEOF_RDMA_ERROR_INVALID_ORD",    "reject: invalid ORD"),
    ("NVME_RDMA_CM_INVALID_CNTLID",    "NVMEOF_RDMA_ERROR_INVALID_CNTLID", "reject: invalid cntlid"),
    ("NVME_RDMA_IP_PORT",              "NVMEOF_RDMA_IP_PORT",              "the port a Linux target listens on"),
    ("NVME_RDMA_DEFAULT_QUEUE_SIZE",   "NVMEOF_RDMA_DEFAULT_QUEUE_SIZE",   "nvmet's default queue size"),
    ("NVME_RDMA_MAX_QUEUE_SIZE",       "NVMEOF_RDMA_MAX_QUEUE_SIZE",       "nvmet's maximum queue size"),
]

# Families where OUR namespace must not contain anything the reference does not
# define.  The pair lists above check the constants someone remembered to list;
# these check that nobody ADDED a member to a wire enum from memory.
#
# This is not hypothetical.  `NVMEOF_FCTYPE_DISCONNECT = 0x08` lived in the wire
# header for the whole project - a fabrics command type no NVMe header defines -
# with a target branch that accepted it and a host step that sent it and asserted
# it was acknowledged.  Both ends agreed; the world answers Invalid Opcode.  The
# pair lists could not catch it, because a constant with no counterpart is exactly
# what they skip.  (DESIGN 8.44.)
#
# (our prefix, reference prefix, how to build the reference name, files, what it is)
FAMILIES = [
    ("NVMEOF_FCTYPE_", "nvme_fabrics_type_", "lower", ("linux_nvme.h",), "fabrics command type"),
    ("NVMEOF_PROP_",   "NVME_REG_",          "keep",  ("linux_nvme.h",), "property register offset"),
]

# Members of those families that are ours on purpose.  Every entry needs a reason,
# because the point of the check is that a NEW member cannot be added from memory
# without someone writing down why it is not a wire value.
FAMILY_OURS_ONLY = {
    "NVMEOF_PROP_ATTRIB_SIZE_MASK": "the width field inside the attrib byte, not a register",
    "NVMEOF_PROP_SIZE_4":           "a value of that field, not a register",
    "NVMEOF_PROP_SIZE_8":           "a value of that field, not a register",
}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def parse_constants(text: str) -> dict:
    """Collect NAME = <int expr> from enums, and #define NAME <int expr>."""
    text = strip_comments(text)
    out = {}

    def value_of(expr: str):
        expr = expr.strip()
        expr = expr.rstrip(",").strip()
        if not expr:
            return None
        # Drop a trailing comma-separated list continuation.
        expr = expr.split(",")[0].strip()
        # An expression may only contain digits, operators and parentheses - but
        # C integer suffixes are letters, so strip those FIRST.  Rejecting every
        # letter outright silently discarded `(1u << 2)` and made this checker
        # report almost nothing, which is how a checker fails quietly instead of
        # loudly.
        expr = re.sub(r"(?i)(?<=[0-9a-fA-F])(ull|llu|ul|lu|ll|u|l)(?![0-9A-Za-z_])", "", expr)
        # Substitute identifiers we have already resolved, so that constants
        # defined in terms of earlier ones (`= 1 << NVME_AEN_BIT_NS_ATTR`) can be
        # evaluated instead of being reported as absent from the reference.
        for _ in range(4):
            names = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", expr)
            if not names:
                break
            changed = False
            for n in names:
                if n in out:
                    expr = re.sub(r"\b%s\b" % re.escape(n), str(out[n]), expr)
                    changed = True
            if not changed:
                break
        # Hex literals contain 'x'.  Leaving it out of the character class made
        # every 0x.. value fail to parse, which looked exactly like "the
        # reference does not define this constant".
        if not re.fullmatch(r"[0-9a-fA-FxX()\s<|&+*~\-]+", expr):
            return None
        try:
            return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - digits only
        except Exception:
            return None

    # enum bodies.  The tag is optional and commonly present (`enum
    # nvmf_capsule_command {`), and a regex that only matched `enum {` skipped
    # every tagged enum in the reference - which is most of the ones that matter.
    #
    # The underlying type is optional too, and it is not decoration: `enum : uint8_t`
    # matched neither `<tag> {` nor `{`, so every member of a scoped enum was
    # invisible - six Failure1 reason codes reported as "not in ours" while they sat
    # in the file being read.  Same shape as the four parser bugs below it.
    for body in re.findall(r"enum\s*(?:[A-Za-z_][A-Za-z0-9_]*)?\s*"
                           r"(?::\s*[A-Za-z_][A-Za-z0-9_:]*)?\s*\{([^}]*)\}",
                           text, flags=re.S):
        for m in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*([^,\n}]+))?", body):
            name, expr = m.group(1), m.group(2)
            if expr is None:
                continue
            v = value_of(expr)
            if v is not None:
                out.setdefault(name, v)

    # #defines
    #
    # The separator has to be [ \t] and NOT \s: \s matches newlines, so a
    # `#define` with no value of its own (an include guard, for instance) consumed
    # the following #define as its value and silently removed that constant from
    # the reference.  The symptom was "the reference does not define
    # NVME_RDMA_IP_PORT" - a checker reporting the absence of something it had
    # just eaten.  Same failure mode as the three parser bugs above; the
    # difference is that this time the pair list caught it, because a name that
    # stops resolving is a FAIL rather than a skip.
    for m in re.finditer(r"#define[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+([^\n/]+)", text):
        v = value_of(m.group(2))
        if v is not None:
            out.setdefault(m.group(1), v)

    # `static const uint8_t kThing = 0x01;` - the spelling nvmeof_dhchap.h uses for
    # the auth wire values.  Without this the pair list for that header would report
    # every constant as "not in ours", which is a checker failing on its own parser
    # rather than on the code (the three parser bugs above have the same shape).
    for m in re.finditer(r"(?:static\s+)?const\s+[A-Za-z_][A-Za-z0-9_]*\s+"
                         r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);", text):
        v = value_of(m.group(2))
        if v is not None:
            out.setdefault(m.group(1), v)

    return out


def main() -> int:
    for p in (REF, REF_RDMA, OURS, OURS_AUTH):
        if not p.exists():
            print("missing file: %s" % p)
            return 2

    ours_text = OURS.read_text(encoding="utf-8", errors="replace")
    ours = parse_constants(ours_text)
    ours_auth = parse_constants(OURS_AUTH.read_text(encoding="utf-8", errors="replace"))

    checked = mismatched = missing_ref = missing_ours = 0
    problems = []
    for ref_path, pairs, our_set in ((REF, PAIRS, ours), (REF_RDMA, PAIRS_RDMA, ours),
                                     (REF, PAIRS_AUTH, ours_auth)):
        ref = parse_constants(ref_path.read_text(encoding="utf-8", errors="replace"))
        print("reference %-22s parsed: %d constants" % (ref_path.name, len(ref)))
        for ref_name, our_name, why in pairs:
            if ref_name not in ref:
                missing_ref += 1
                problems.append("  [no reference] %-34s (%s)" % (ref_name, why))
                continue
            if our_name not in our_set:
                missing_ours += 1
                problems.append("  [not in ours ] %-34s (%s)" % (our_name, why))
                continue
            checked += 1
            if ref[ref_name] != our_set[our_name]:
                mismatched += 1
                problems.append("  [MISMATCH] %-26s ours=0x%X  reference %s=0x%X   (%s)"
                                % (our_name, our_set[our_name], ref_name, ref[ref_name], why))

    # ---- families: nothing in OUR wire enums that the reference does not define ----
    refs = {}
    for ref_path, _ in ((REF, PAIRS), (REF_RDMA, PAIRS_RDMA)):
        refs[ref_path.name] = parse_constants(ref_path.read_text(encoding="utf-8", errors="replace"))
    orphans = []
    for our_prefix, ref_prefix, case, ref_names, what in FAMILIES:
        ref_names_here = set()
        for rn in ref_names:
            for name in refs.get(rn, {}):
                if name.startswith(ref_prefix):
                    ref_names_here.add(name)
        if not ref_names_here:
            orphans.append("  [no reference] family %-16s: nothing named %s* in %s"
                           % (our_prefix, ref_prefix, "/".join(ref_names)))
            continue
        for name in sorted(ours):
            if not name.startswith(our_prefix) or name in FAMILY_OURS_ONLY:
                continue
            stem = name[len(our_prefix):]
            want = ref_prefix + (stem.lower() if case == "lower" else stem)
            if want not in ref_names_here:
                orphans.append("  [INVENTED]    %-34s has no %s in the reference  (%s)"
                               % (name, want, what))

    print("constant cross-check")
    print("  parsed from ours: %d constants (%s) + %d (%s)"
          % (len(ours), OURS.name, len(ours_auth), OURS_AUTH.name))
    print("  compared : %d" % checked)
    print("  mismatched: %d" % mismatched)
    print("  not found in reference: %d, not found in ours: %d"
          % (missing_ref, missing_ours))
    print("  family members with no reference counterpart: %d" % len(orphans))
    if problems or orphans:
        print("")
        for line in problems:
            print(line)
        for line in orphans:
            print(line)

    # A pair list that has drifted out of sync with the sources is itself a
    # failure: silently checking fewer constants is how this kind of test rots.
    if missing_ref or missing_ours:
        print("\nRESULT: FAIL - the pair list names constants that no longer exist")
        return 1
    if mismatched:
        print("\nRESULT: FAIL - %d constant(s) disagree with the reference" % mismatched)
        return 1
    if orphans:
        print("\nRESULT: FAIL - %d wire constant(s) exist only in this project" % len(orphans))
        return 1
    print("\nRESULT: PASS - every checked constant matches the reference header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
