/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * nvmeof_tcp.h - the NVMe/TCP transport, PDU layer.
 *
 * Why this file exists at all: ROADMAP.md G-9 recorded that this project had exactly one
 * transport (RDMA), and section 9.1 recorded why the second one is worth writing by hand -
 * every mature NVMe/TCP implementation (Linux nvme-tcp/nvmet-tcp, SPDK, libnvme) is Linux
 * kernel or Linux user space, and there is no usable Windows port. So the Windows side of
 * NVMe/TCP is unwritten territory, and the NVMe layer above it (capsules, fabrics, command
 * set, DH-HMAC-CHAP in nvmeof_wire.h and friends) is already transport-agnostic and gets
 * reused verbatim.
 *
 * What changes against RDMA, and it is worth stating plainly because it is the whole design
 * difference (DESIGN.md L99 says the same thing from the other side): an RDMA queue pair is
 * a queue pair, whereas here EVERY I/O QUEUE IS A SEPARATE TCP CONNECTION, and the data path
 * is not one-sided memory access any more. A command's data either rides inside the capsule
 * (in-capsule, for small transfers) or moves as explicit H2CData/C2HData PDUs that the
 * *sender* paces, with the receiver able to ask for a specific byte range back with an R2T.
 * There is no RDMA Read and no RDMA Write anywhere in this file, and no memory registration.
 *
 * Everything here is header-only and static, matching nvmeof_wire.h, so the self-test in
 * src/tcp_selftest.c can include it and check the encodings byte for byte without linking
 * anything.
 *
 * References: NVMe-oF specification, TP 8000 (TCP transport). Field layouts below carry the
 * offsets they must have on the wire, because that is what a peer will check.
 */

#ifndef NVMEOF_TCP_H
#define NVMEOF_TCP_H

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------------------- */
/* PDU types.  TP 8000 figure: the type byte in the common header.                        */
/* ------------------------------------------------------------------------------------- */

#define NVMEOF_TCP_ICREQ        0x00u   /* host   -> controller, first thing on a connection */
#define NVMEOF_TCP_ICRESP       0x01u   /* controller -> host                               */
#define NVMEOF_TCP_H2CTERMREQ   0x02u   /* host   -> controller, ordered shutdown            */
#define NVMEOF_TCP_C2HTERMREQ   0x03u   /* controller -> host                                */
#define NVMEOF_TCP_CAPSULECMD   0x04u   /* host   -> controller: 64-byte SQE (+ optional data)*/
#define NVMEOF_TCP_CAPSULERESP  0x05u   /* controller -> host: 16-byte completion (+ data)    */
#define NVMEOF_TCP_H2CDATA      0x06u   /* host   -> controller: write payload                */
#define NVMEOF_TCP_C2HDATA      0x07u   /* controller -> host: read payload                   */
#define NVMEOF_TCP_R2T          0x09u   /* controller -> host: "send me this range"           */

/* ------------------------------------------------------------------------------------- */
/* Common header flags.  Which bits are meaningful depends on the PDU type.               */
/* ------------------------------------------------------------------------------------- */

#define NVMEOF_TCP_F_HDGST      0x01u   /* header digest present (CRC32C after the header)   */
#define NVMEOF_TCP_F_DDGST      0x02u   /* data digest present                               */
#define NVMEOF_TCP_F_SUCCESS    0x10u   /* CapsuleResp only: the command succeeded           */
#define NVMEOF_TCP_F_LAST       0x40u   /* C2HData only: last PDU of this command's data     */

/* ------------------------------------------------------------------------------------- */
/* Fixed sizes.  These are not ours to choose: a peer compares them.                       */
/* ------------------------------------------------------------------------------------- */

#define NVMEOF_TCP_HDR_LEN          8u    /* common PDU header                        */
#define NVMEOF_TCP_ICREQ_LEN      128u    /* ICReq/ICResp, header included            */
#define NVMEOF_TCP_CCMD_HDR_LEN    72u    /* CapsuleCmd: 8 + the 64-byte SQE          */
#define NVMEOF_TCP_CRSP_HDR_LEN    24u    /* CapsuleResp: 8 + the 16-byte completion  */
#define NVMEOF_TCP_DATA_HDR_LEN    24u    /* H2CData / C2HData / R2T                  */
#define NVMEOF_TCP_SQE_LEN         64u
#define NVMEOF_TCP_CQE_LEN         16u
#define NVMEOF_TCP_PDU_ALIGN        4u    /* H2C/C2H data PDUs are padded up to this  */
#define NVMEOF_TCP_DEFAULT_PORT  4420u
#define NVMEOF_TCP_MAXR2T_DEFAULT 0u      /* 0 = no R2T, all data pushed by the sender */

/* The ICReq/ICResp body is 120 bytes after the 8-byte header. */
#define NVMEOF_TCP_IC_BODY_LEN    (NVMEOF_TCP_ICREQ_LEN - NVMEOF_TCP_HDR_LEN)

/* Digest sizes: CRC32C in both cases (TP 8000 says CRC32C for header and data digests). */
#define NVMEOF_TCP_DIGEST_LEN       4u

/* ------------------------------------------------------------------------------------- */
/* PDU header accessors.  Kept as byte offsets rather than structs on purpose: the wire    */
/* layout is fixed and little-endian, and a packed struct silently depends on how the      */
/* compiler decides to pad.  Offsets are in the comments because a peer will check them.   */
/* ------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t  type;        /* +0                                    */
    uint8_t  flags;       /* +1                                    */
    uint8_t  hlen;        /* +2  header length, header included    */
    uint8_t  pdo;         /* +3  padding / data offset             */
    uint32_t plen;        /* +4  payload length, little-endian     */
} NVMEOF_TCP_HDR;

static void nvmeofTcpReadHdr(const uint8_t* p, NVMEOF_TCP_HDR* h)
{
    h->type  = p[0];
    h->flags = p[1];
    h->hlen  = p[2];
    h->pdo   = p[3];
    h->plen  = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
}

static void nvmeofTcpWriteHdr(uint8_t* p, const NVMEOF_TCP_HDR* h)
{
    p[0] = h->type;
    p[1] = h->flags;
    p[2] = h->hlen;
    p[3] = h->pdo;
    p[4] = (uint8_t)(h->plen & 0xFF);
    p[5] = (uint8_t)((h->plen >> 8) & 0xFF);
    p[6] = (uint8_t)((h->plen >> 16) & 0xFF);
    p[7] = (uint8_t)((h->plen >> 24) & 0xFF);
}

/* A 32-bit little-endian field at a byte offset.  Used for every multi-byte field below
 * so nothing depends on struct packing. */
static uint32_t nvmeofTcpGet32(const uint8_t* p, unsigned off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static void nvmeofTcpPut32(uint8_t* p, unsigned off, uint32_t v)
{
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ------------------------------------------------------------------------------------- */
/* ICReq / ICResp - the handshake that opens every TCP connection.                         */
/*                                                                                        */
/*   +0  type, +1 flags, +2 hlen = 128, +3 pdo = 0, +4 plen = 0                            */
/*   +8  pfv     :2   PDU format version, 0                                           */
/*   +10 cpda    :1   capsule data alignment, a power of two, at least 4              */
/*   +11 dgst    :1   0 = no digests, 1 = CRC32C                                      */
/*   +12 maxr2t  :4   max outstanding R2Ts the sender of ICResp will honour           */
/*   +16 hdgst   :1   1 = header digest required                                      */
/*   +17 ddgst   :1   1 = data digest required                                        */
/*   +18..127    reserved, zero                                                       */
/* ------------------------------------------------------------------------------------- */

typedef struct {
    uint16_t pfv;
    uint8_t  cpda;
    uint8_t  dgst;
    uint32_t maxr2t;
    uint8_t  hdgst;
    uint8_t  ddgst;
} NVMEOF_TCP_IC;

static void nvmeofTcpBuildIc(uint8_t* buf, uint8_t type, const NVMEOF_TCP_IC* ic)
{
    NVMEOF_TCP_HDR h;
    memset(buf, 0, NVMEOF_TCP_ICREQ_LEN);
    h.type = type; h.flags = 0; h.hlen = (uint8_t)NVMEOF_TCP_ICREQ_LEN; h.pdo = 0; h.plen = 0;
    nvmeofTcpWriteHdr(buf, &h);
    buf[8]  = (uint8_t)(ic->pfv & 0xFF);
    buf[9]  = (uint8_t)((ic->pfv >> 8) & 0xFF);
    buf[10] = ic->cpda;
    buf[11] = ic->dgst;
    nvmeofTcpPut32(buf, 12, ic->maxr2t);
    buf[16] = ic->hdgst;
    buf[17] = ic->ddgst;
}

static int nvmeofTcpParseIc(const uint8_t* buf, unsigned len, NVMEOF_TCP_IC* ic)
{
    NVMEOF_TCP_HDR h;
    if (len < NVMEOF_TCP_ICREQ_LEN) {
        return -1;
    }
    nvmeofTcpReadHdr(buf, &h);
    if (h.type != NVMEOF_TCP_ICREQ && h.type != NVMEOF_TCP_ICRESP) {
        return -2;
    }
    if (h.hlen != NVMEOF_TCP_ICREQ_LEN) {
        return -3;
    }
    ic->pfv    = (uint16_t)(buf[8] | ((uint16_t)buf[9] << 8));
    ic->cpda   = buf[10];
    ic->dgst   = buf[11];
    ic->maxr2t = nvmeofTcpGet32(buf, 12);
    ic->hdgst  = buf[16];
    ic->ddgst  = buf[17];
    /* A peer that asks for a capsule alignment below 4, or not a power of two, is not
     * speaking this protocol.  Rejecting it here is cheaper than debugging a data
     * corruption later. */
    if (ic->pfv != 0) {
        return -4;
    }
    if (ic->cpda < NVMEOF_TCP_PDU_ALIGN || (ic->cpda & (ic->cpda - 1)) != 0) {
        return -5;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------- */
/* CapsuleCmd / CapsuleResp                                                               */
/*                                                                                        */
/*   CapsuleCmd   +0 type=0x04, +1 flags, +2 hlen = 72 (or 8 when there is no SQE),        */
/*                +3 pdo = data offset, +4 plen = data length                             */
/*                +8 the 64-byte SQE                                                     */
/*   CapsuleResp  +0 type=0x05, +1 flags (bit 4 SUCCESS), +2 hlen = 24 (or 8),             */
/*                +3 pdo, +4 plen, +8 the 16-byte completion                              */
/* ------------------------------------------------------------------------------------- */

static unsigned nvmeofTcpBuildCapsuleCmd(uint8_t* buf, const uint8_t* sqe64,
                                         unsigned dataLen, unsigned dataOffset)
{
    NVMEOF_TCP_HDR h;
    h.type  = NVMEOF_TCP_CAPSULECMD;
    h.flags = 0;
    h.hlen  = (uint8_t)NVMEOF_TCP_CCMD_HDR_LEN;
    h.pdo   = (uint8_t)dataOffset;
    h.plen  = (uint32_t)dataLen;
    nvmeofTcpWriteHdr(buf, &h);
    memcpy(buf + NVMEOF_TCP_HDR_LEN, sqe64, NVMEOF_TCP_SQE_LEN);
    return NVMEOF_TCP_CCMD_HDR_LEN;
}

/* The PDU length on the wire: header + payload, rounded up to the alignment. */
static unsigned nvmeofTcpPduTotal(unsigned hlen, unsigned plen, unsigned align)
{
    if (align < 1) {
        align = 1;
    }
    return ((hlen + plen + align - 1) / align) * align;
}

static unsigned nvmeofTcpBuildCapsuleResp(uint8_t* buf, const uint8_t* cqe16, int success,
                                          unsigned dataLen, unsigned dataOffset)
{
    NVMEOF_TCP_HDR h;
    h.type  = NVMEOF_TCP_CAPSULERESP;
    h.flags = success ? NVMEOF_TCP_F_SUCCESS : 0;
    h.hlen  = (uint8_t)NVMEOF_TCP_CRSP_HDR_LEN;
    h.pdo   = (uint8_t)dataOffset;
    h.plen  = (uint32_t)dataLen;
    nvmeofTcpWriteHdr(buf, &h);
    memcpy(buf + NVMEOF_TCP_HDR_LEN, cqe16, NVMEOF_TCP_CQE_LEN);
    return NVMEOF_TCP_CRSP_HDR_LEN;
}

/* ------------------------------------------------------------------------------------- */
/* Identify Namespace (CNS = 0)                                                           */
/*                                                                                       */
/* This lives in the header, not in nvmeof_tcp.c, so the self-test runs the exact function */
/* the target runs instead of a copy that can drift from it.  NVMe 1.4 puts NSZE at 0,     */
/* NCAP at 8, NUSE at 16, NSFEAT at 24, NLBAF at 25, FLBAS at 26, and the LBA Format table */
/* at 128 with 4-byte entries: MS at +0..1, LBADS at +2, RP at +3.  So LBAF0 is 128..131   */
/* and LBADS is byte 130.  The first version of the target wrote the table four bytes late */
/* (LBADS at 132, with DLFEAT on top of LBAF0's MS), and the kernel miniport had to work   */
/* around it; the offsets are one per line because that is exactly how it went wrong.      */
/* ------------------------------------------------------------------------------------- */
static void nvmeofTcpFillIdentifyNamespace(uint8_t* d, uint32_t blocks)
{
    uint64_t nsze = (uint64_t)blocks;
    memset(d, 0, 4096);
    memcpy(d, &nsze, 8);                     /* NSZE  at 0  */
    memcpy(d + 8, &nsze, 8);                 /* NCAP  at 8  */
    memcpy(d + 16, &nsze, 8);                /* NUSE  at 16 */
    d[25] = 0;                               /* NLBAF at 25: one format (zero based) */
    d[26] = 0;                               /* FLBAS at 26: format 0, no metadata */
    d[128] = 0;                              /* LBAF0 MS low byte: no metadata bytes */
    d[129] = 0;                              /* LBAF0 MS high byte */
    d[130] = 9;                              /* LBAF0 LBADS: 2^9 = 512-byte blocks */
    d[131] = 0;                              /* LBAF0 RP: relative performance */
}

/* ------------------------------------------------------------------------------------- */
/* H2CData / C2HData / R2T                                                                */
/*                                                                                        */
/*   +0 type, +1 flags, +2 hlen = 24, +3 pdo = 0, +4 plen = data length                   */
/*   +8  cccid :2   command capsule id                                                    */
/*   +10 ttag  :2   transfer tag                                                          */
/*   +12 datao :4   data offset within the command's data buffer                          */
/*   +16 datal :4   data length in this PDU                                               */
/*   +20..23      reserved, zero                                                          */
/*   R2T is the same shape but +12 is r2tto and +16 is r2ttl.                             */
/* ------------------------------------------------------------------------------------- */

typedef struct {
    uint16_t cccid;
    uint16_t ttag;
    uint32_t datao;
    uint32_t datal;
} NVMEOF_TCP_DATA;

static void nvmeofTcpReadData(const uint8_t* p, NVMEOF_TCP_DATA* d)
{
    d->cccid = (uint16_t)(p[8] | ((uint16_t)p[9] << 8));
    d->ttag  = (uint16_t)(p[10] | ((uint16_t)p[11] << 8));
    d->datao = nvmeofTcpGet32(p, 12);
    d->datal = nvmeofTcpGet32(p, 16);
}

static void nvmeofTcpBuildData(uint8_t* buf, uint8_t type, uint8_t flags,
                               const NVMEOF_TCP_DATA* d)
{
    NVMEOF_TCP_HDR h;
    memset(buf, 0, NVMEOF_TCP_DATA_HDR_LEN);
    h.type  = type;
    h.flags = flags;
    h.hlen  = (uint8_t)NVMEOF_TCP_DATA_HDR_LEN;
    h.pdo   = 0;
    h.plen  = d->datal;
    nvmeofTcpWriteHdr(buf, &h);
    buf[8]  = (uint8_t)(d->cccid & 0xFF);
    buf[9]  = (uint8_t)((d->cccid >> 8) & 0xFF);
    buf[10] = (uint8_t)(d->ttag & 0xFF);
    buf[11] = (uint8_t)((d->ttag >> 8) & 0xFF);
    nvmeofTcpPut32(buf, 12, d->datao);
    nvmeofTcpPut32(buf, 16, d->datal);
}

/* ------------------------------------------------------------------------------------- */
/* Connection state.  One of these per TCP connection, which - and this is the structural  */
/* difference from RDMA - means one per I/O queue.  There is no queue pair to create and   */
/* nothing to register; the connection itself is the queue.                                */
/* ------------------------------------------------------------------------------------- */

typedef enum {
    NVMEOF_TCP_ST_FRESH = 0,   /* accepted, nothing read yet            */
    NVMEOF_TCP_ST_IC_SENT,     /* ICReq sent, waiting for ICResp         */
    NVMEOF_TCP_ST_IC_DONE,     /* handshake complete, capsules may flow  */
    NVMEOF_TCP_ST_TERM,        /* TermReq seen, drain and close          */
    NVMEOF_TCP_ST_CLOSED
} NVMEOF_TCP_STATE;

typedef struct {
    NVMEOF_TCP_STATE state;
    int              fd;
    unsigned         qid;          /* which queue this connection is        */
    NVMEOF_TCP_IC    peer;         /* the peer's ICReq/ICResp, once parsed  */
    unsigned         cccidNext;    /* next command capsule id to hand out   */
    unsigned         inFlight;     /* capsules issued, not yet completed    */
} NVMEOF_TCP_CONN;

static const char* nvmeofTcpStateName(NVMEOF_TCP_STATE s)
{
    switch (s) {
    case NVMEOF_TCP_ST_FRESH:   return "fresh";
    case NVMEOF_TCP_ST_IC_SENT: return "ic-sent";
    case NVMEOF_TCP_ST_IC_DONE: return "ic-done";
    case NVMEOF_TCP_ST_TERM:    return "term";
    case NVMEOF_TCP_ST_CLOSED:  return "closed";
    }
    return "?";
}

static const char* nvmeofTcpTypeName(uint8_t t)
{
    switch (t) {
    case NVMEOF_TCP_ICREQ:       return "ICReq";
    case NVMEOF_TCP_ICRESP:      return "ICResp";
    case NVMEOF_TCP_H2CTERMREQ:  return "H2CTermReq";
    case NVMEOF_TCP_C2HTERMREQ:  return "C2HTermReq";
    case NVMEOF_TCP_CAPSULECMD:  return "CapsuleCmd";
    case NVMEOF_TCP_CAPSULERESP: return "CapsuleResp";
    case NVMEOF_TCP_H2CDATA:     return "H2CData";
    case NVMEOF_TCP_C2HDATA:     return "C2HData";
    case NVMEOF_TCP_R2T:         return "R2T";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------------------- */
/* CRC32C - the digest NVMe/TCP uses (Castagnoli polynomial, reflected, 0x82F63B78).      */
/* Table is built on first use so this header stays self-contained.                        */
/* ------------------------------------------------------------------------------------- */

static uint32_t g_nvmeofTcpCrc32cTable[256];
static int      g_nvmeofTcpCrc32cReady = 0;

static void nvmeofTcpCrc32cInit(void)
{
    uint32_t i, j, c;
    if (g_nvmeofTcpCrc32cReady) {
        return;
    }
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        }
        g_nvmeofTcpCrc32cTable[i] = c;
    }
    g_nvmeofTcpCrc32cReady = 1;
}

static uint32_t nvmeofTcpCrc32c(uint32_t seed, const uint8_t* p, size_t len)
{
    uint32_t c = seed ^ 0xFFFFFFFFu;
    size_t   i;
    nvmeofTcpCrc32cInit();
    for (i = 0; i < len; i++) {
        c = g_nvmeofTcpCrc32cTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

#endif /* NVMEOF_TCP_H */
