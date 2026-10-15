/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * tcp_selftest.c - byte-exact checks for the NVMe/TCP PDU layer.
 *
 * Same discipline as wire_selftest.c: every claim about the wire format is a comparison
 * against a literal byte sequence or a published constant, because a self-consistent
 * encoder and decoder can agree with each other while both disagree with the spec.  That
 * lesson is in DESIGN.md and it cost this project eleven defects once already.
 *
 *   build:  cl /nologo /W4 /std:c17 tcp_selftest.c /Fe:tcp_selftest.exe
 *   run:    .\tcp_selftest.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nvmeof_tcp.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(int cond, const char* what)
{
    if (cond) {
        g_pass++;
        printf("  [PASS] %s\n", what);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void okHex(const uint8_t* got, const uint8_t* want, unsigned n, const char* what)
{
    unsigned i;
    int same = 1;
    for (i = 0; i < n; i++) {
        if (got[i] != want[i]) { same = 0; break; }
    }
    if (same) {
        g_pass++;
        printf("  [PASS] %s\n", what);
        return;
    }
    g_fail++;
    printf("  [FAIL] %s\n", what);
    printf("         want:");
    for (i = 0; i < n; i++) { printf(" %02X", want[i]); }
    printf("\n         got :");
    for (i = 0; i < n; i++) { printf(" %02X", got[i]); }
    printf("\n");
}

/* --------------------------------------------------------------------------- */

static void test_header(void)
{
    uint8_t buf[8];
    uint8_t want[8];
    NVMEOF_TCP_HDR h, back;

    printf("\n== PDU header ==\n");

    h.type = NVMEOF_TCP_CAPSULECMD;
    h.flags = NVMEOF_TCP_F_HDGST;
    h.hlen = NVMEOF_TCP_CCMD_HDR_LEN;
    h.pdo = 8;
    h.plen = 0x11223344u;
    nvmeofTcpWriteHdr(buf, &h);

    /* The header is fixed: type, flags, hlen, pdo, then a little-endian 32-bit length.
     * These eight bytes are what a peer parses, so they are compared literally. */
    want[0] = 0x04; want[1] = 0x01; want[2] = 0x48; want[3] = 0x08;
    want[4] = 0x44; want[5] = 0x33; want[6] = 0x22; want[7] = 0x11;
    okHex(buf, want, 8, "header encodes to the literal bytes a peer expects");

    nvmeofTcpReadHdr(buf, &back);
    ok(back.type == h.type && back.flags == h.flags && back.hlen == h.hlen &&
       back.pdo == h.pdo && back.plen == h.plen, "header round-trips");

    ok(nvmeofTcpGet32(buf, 4) == 0x11223344u, "plen is little-endian at offset 4");
}

static void test_icreq(void)
{
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_IC ic, back;
    int rc;

    printf("\n== ICReq / ICResp ==\n");

    ic.pfv = 0; ic.cpda = 4; ic.dgst = 1; ic.maxr2t = 0; ic.hdgst = 0; ic.ddgst = 0;
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);

    ok(buf[0] == NVMEOF_TCP_ICREQ, "type is ICReq");
    ok(buf[2] == 128, "hlen is 128, header included");
    ok(buf[3] == 0 && buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0,
       "pdo and plen are zero");
    ok(buf[10] == 4, "cpda at offset 10");
    ok(buf[11] == 1, "dgst at offset 11");
    ok(nvmeofTcpGet32(buf, 12) == 0, "maxr2t at offset 12");
    ok(buf[16] == 0 && buf[17] == 0, "hdgst and ddgst at offsets 16 and 17");
    {
        unsigned i;
        int zeros = 1;
        for (i = 18; i < 128; i++) { if (buf[i] != 0) { zeros = 0; break; } }
        ok(zeros, "everything after offset 18 is zero");
    }

    rc = nvmeofTcpParseIc(buf, sizeof(buf), &back);
    ok(rc == 0, "ICReq parses");
    ok(back.cpda == 4 && back.dgst == 1 && back.maxr2t == 0, "parsed fields match");

    /* A peer announcing an alignment that is not a power of two, or below 4, is not
     * speaking this protocol.  Refusing it up front beats chasing corruption later. */
    ic.cpda = 3;
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);
    ok(nvmeofTcpParseIc(buf, sizeof(buf), &back) == -5, "cpda = 3 is refused (not a power of two)");
    ic.cpda = 2;
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);
    ok(nvmeofTcpParseIc(buf, sizeof(buf), &back) == -5, "cpda = 2 is refused (below 4)");
    ic.cpda = 4; ic.pfv = 1;
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);
    ok(nvmeofTcpParseIc(buf, sizeof(buf), &back) == -4, "pfv other than 0 is refused");

    /* A short read must not be mistaken for a complete ICReq. */
    ok(nvmeofTcpParseIc(buf, 64, &back) == -1, "a truncated ICReq is refused");
}

static void test_capsule(void)
{
    uint8_t buf[256];
    uint8_t sqe[64];
    uint8_t cqe[16];
    unsigned n;
    unsigned i;

    printf("\n== CapsuleCmd / CapsuleResp ==\n");

    for (i = 0; i < 64; i++) { sqe[i] = (uint8_t)i; }
    n = nvmeofTcpBuildCapsuleCmd(buf, sqe, 0, 8);
    ok(n == 72, "CapsuleCmd header is 72 bytes (8 + 64)");
    ok(buf[0] == NVMEOF_TCP_CAPSULECMD, "type is CapsuleCmd");
    ok(buf[2] == 72, "hlen is 72");
    ok(buf[3] == 8, "pdo carries the data offset");
    ok(buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0, "plen is zero for a no-data command");
    okHex(buf + 8, sqe, 64, "the 64-byte SQE sits at offset 8, unchanged");

    n = nvmeofTcpBuildCapsuleCmd(buf, sqe, 4096, 8);
    ok(nvmeofTcpGet32(buf, 4) == 4096, "plen carries the data length when there is data");

    for (i = 0; i < 16; i++) { cqe[i] = (uint8_t)(0xA0 + i); }
    n = nvmeofTcpBuildCapsuleResp(buf, cqe, 1, 0, 0);
    ok(n == 24, "CapsuleResp header is 24 bytes (8 + 16)");
    ok(buf[0] == NVMEOF_TCP_CAPSULERESP, "type is CapsuleResp");
    ok((buf[1] & NVMEOF_TCP_F_SUCCESS) != 0, "SUCCESS is set in the flags byte");
    okHex(buf + 8, cqe, 16, "the 16-byte completion sits at offset 8, unchanged");

    n = nvmeofTcpBuildCapsuleResp(buf, cqe, 0, 0, 0);
    ok((buf[1] & NVMEOF_TCP_F_SUCCESS) == 0, "SUCCESS is clear on failure");
}

static void test_data_pdus(void)
{
    uint8_t buf[NVMEOF_TCP_DATA_HDR_LEN];
    NVMEOF_TCP_DATA d, back;
    uint8_t want[24];

    printf("\n== H2CData / C2HData / R2T ==\n");

    d.cccid = 0x1234; d.ttag = 0x5678; d.datao = 0x00010000u; d.datal = 0x00001000u;
    nvmeofTcpBuildData(buf, NVMEOF_TCP_H2CDATA, 0, &d);

    /* This one is worth pinning literally: cccid and ttag are the two fields a peer uses
     * to attach an out-of-order data PDU to the right command, and getting either byte
     * wrong shows up as data landing in the wrong place rather than as an error. */
    want[0]  = 0x06; want[1]  = 0x00; want[2]  = 0x18; want[3]  = 0x00;
    want[4]  = 0x00; want[5]  = 0x10; want[6]  = 0x00; want[7]  = 0x00;   /* plen = datal */
    want[8]  = 0x34; want[9]  = 0x12;                                     /* cccid        */
    want[10] = 0x78; want[11] = 0x56;                                     /* ttag         */
    want[12] = 0x00; want[13] = 0x00; want[14] = 0x01; want[15] = 0x00;   /* datao        */
    want[16] = 0x00; want[17] = 0x10; want[18] = 0x00; want[19] = 0x00;   /* datal        */
    want[20] = 0x00; want[21] = 0x00; want[22] = 0x00; want[23] = 0x00;
    okHex(buf, want, 24, "H2CData encodes to the literal 24 bytes a peer expects");

    nvmeofTcpReadData(buf, &back);
    ok(back.cccid == d.cccid && back.ttag == d.ttag &&
       back.datao == d.datao && back.datal == d.datal, "H2CData round-trips");

    nvmeofTcpBuildData(buf, NVMEOF_TCP_C2HDATA, NVMEOF_TCP_F_LAST, &d);
    ok(buf[0] == NVMEOF_TCP_C2HDATA, "type is C2HData");
    ok((buf[1] & NVMEOF_TCP_F_LAST) != 0, "LAST is set, so this is the final data PDU");

    nvmeofTcpBuildData(buf, NVMEOF_TCP_R2T, 0, &d);
    ok(buf[0] == NVMEOF_TCP_R2T, "type is R2T");
    ok(nvmeofTcpGet32(buf, 12) == d.datao && nvmeofTcpGet32(buf, 16) == d.datal,
       "R2T reuses offset 12 and 16 for the requested range");

    ok(nvmeofTcpTypeName(NVMEOF_TCP_R2T)[0] == 'R', "type names resolve");
}

static void test_alignment(void)
{
    printf("\n== alignment ==\n");

    /* H2C and C2H data PDUs are padded up to the capsule alignment.  A 24-byte header plus
     * 4096 bytes of payload is already a multiple of 4, but the capsule header is not: 72
     * bytes of header with 6 bytes of in-capsule data is 78, which pads to 80.  Getting
     * this wrong shifts every following PDU in the stream, so it is tested rather than
     * assumed. */
    ok(nvmeofTcpPduTotal(24, 4096, 4) == 4120, "24 + 4096 pads to 4120");
    ok(nvmeofTcpPduTotal(72, 6, 4) == 80, "72 + 6 pads to 80");
    ok(nvmeofTcpPduTotal(72, 0, 4) == 72, "an exact multiple is unchanged");
    ok(nvmeofTcpPduTotal(24, 1, 4) == 28, "1 byte of payload still costs a full 4-byte slot");
    ok(nvmeofTcpPduTotal(24, 4096, 1) == 4120, "alignment of 1 means no padding");
}

static void test_crc32c(void)
{
    const uint8_t* v = (const uint8_t*)"123456789";

    printf("\n== CRC32C (the digest NVMe/TCP uses) ==\n");

    /* The published CRC32C check value for "123456789" is 0xE3069283.  Anything else means
     * the polynomial, the reflection or the final xor is wrong - and a digest that is
     * merely self-consistent will be rejected by every real peer. */
    ok(nvmeofTcpCrc32c(0, v, 9) == 0xE3069283u, "CRC32C(\"123456789\") == 0xE3069283");
    ok(nvmeofTcpCrc32c(0, (const uint8_t*)"", 0) == 0x00000000u, "CRC32C of nothing is 0");
    ok(nvmeofTcpCrc32c(0, (const uint8_t*)"a", 1) == 0xC1D04330u, "CRC32C(\"a\") == 0xC1D04330");
    /* Seeding lets a digest cover a header and its payload without concatenating them. */
    {
        uint32_t whole = nvmeofTcpCrc32c(0, v, 9);
        uint32_t split = nvmeofTcpCrc32c(nvmeofTcpCrc32c(0, v, 4), v + 4, 5);
        ok(whole == split, "seeded CRC32C over two halves equals the whole");
    }
}

static void test_state_names(void)
{
    printf("\n== connection state ==\n");
    ok(strcmp(nvmeofTcpStateName(NVMEOF_TCP_ST_FRESH), "fresh") == 0, "state names resolve");
    ok(strcmp(nvmeofTcpStateName(NVMEOF_TCP_ST_IC_DONE), "ic-done") == 0, "ic-done resolves");
    ok(NVMEOF_TCP_DEFAULT_PORT == 4420, "the default port is 4420, same as the RDMA transport");
}

int main(void)
{
    printf("NVMe/TCP PDU layer self-test\n");
    printf("============================\n");

    test_header();
    test_icreq();
    test_capsule();
    test_data_pdus();
    test_alignment();
    test_crc32c();
    test_state_names();

    printf("\n============================\n");
    printf("pass %d  fail %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
