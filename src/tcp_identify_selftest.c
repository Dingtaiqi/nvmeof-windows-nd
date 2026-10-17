/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * tcp_identify_selftest.c - a real NVMe command carried over NVMe/TCP on loopback.
 *
 * ROADMAP E1a step 3.  Step 1 proved the PDU encodings byte for byte, step 2 the connection
 * handshake.  This proves an NVMe command actually travels: a 64-byte SQE in a CapsuleCmd,
 * a 16-byte completion in a CapsuleResp, and 4096 bytes of Identify data carried inside that
 * response capsule.
 *
 * Everything is user mode on 127.0.0.1, which is the point: no RDMA stack, no NetworkDirect
 * provider, no kernel driver, nothing that can bugcheck the machine.
 *
 * The lesson from the first version of this file: the two ends must be driven in the order a
 * real connection uses - send, serve, read - and each side must be a separate call.  A single
 * helper that both sent and waited for a reply deadlocked against a controller that had not
 * run yet, and only the five-second receive timeout turned that into visible failures.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "nvmeof_tcp.h"

static int g_pass = 0;
static int g_fail = 0;
static void ok(int cond, const char* what)
{
    if (cond) { g_pass++; printf("  [PASS] %s\n", what); }
    else      { g_fail++; printf("  [FAIL] %s\n", what); }
}

/* ---- the NVMe bits ---------------------------------------------------------------- */
/* SQE: +0 CDW0 (opcode | cid<<16), +4 NSID, +24 PRP1, +40 CDW10 ... +60 CDW15            */
/* CQE: +8 DW2 (SQHD | SQID<<16), +12 DW3 (CID | P<<16 | SC<<17)                          */

#define NVME_IDENTIFY          0x06u
#define NVME_READ              0x02u
#define NVME_WRITE             0x01u
#define NVME_CNS_CONTROLLER    0x01u
#define NVME_CNS_NAMESPACE     0x00u
#define NVME_SC_SUCCESS        0x00u
#define NVME_SC_INVALID_FIELD  0x02u

static void sqeBuild(uint8_t* sqe, uint8_t opcode, uint32_t nsid, uint16_t cid,
                     uint32_t cdw10, uint32_t cdw11, uint32_t cdw12)
{
    memset(sqe, 0, 64);
    sqe[0] = opcode;
    sqe[2] = (uint8_t)(cid & 0xFF);
    sqe[3] = (uint8_t)((cid >> 8) & 0xFF);
    sqe[4] = (uint8_t)(nsid & 0xFF);
    sqe[5] = (uint8_t)((nsid >> 8) & 0xFF);
    sqe[6] = (uint8_t)((nsid >> 16) & 0xFF);
    sqe[7] = (uint8_t)((nsid >> 24) & 0xFF);
    nvmeofTcpPut32(sqe, 40, cdw10);
    nvmeofTcpPut32(sqe, 44, cdw11);
    nvmeofTcpPut32(sqe, 48, cdw12);
}
static uint8_t  sqeOpcode(const uint8_t* q) { return q[0]; }
static uint16_t sqeCid(const uint8_t* q)    { return (uint16_t)(q[2] | ((uint16_t)q[3] << 8)); }
static uint32_t sqeCdw10(const uint8_t* q)  { return nvmeofTcpGet32(q, 40); }

static void cqeBuild(uint8_t* cqe, uint16_t sqhd, uint16_t cid, uint8_t sc)
{
    memset(cqe, 0, 16);
    cqe[8] = (uint8_t)(sqhd & 0xFF);
    cqe[9] = (uint8_t)((sqhd >> 8) & 0xFF);
    nvmeofTcpPut32(cqe, 12, ((uint32_t)cid) | (1u << 16) | (((uint32_t)sc) << 17));
}
static uint16_t cqeCid(const uint8_t* q)    { return (uint16_t)(nvmeofTcpGet32(q, 12) & 0xFFFF); }
static uint8_t  cqeStatus(const uint8_t* q) { return (uint8_t)((nvmeofTcpGet32(q, 12) >> 17) & 0xFF); }

/* ---- sockets ----------------------------------------------------------------------- */

static void setRecvTimeout(SOCKET s, int ms)
{
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
}

static int recvAll(SOCKET s, uint8_t* b, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(s, (char*)b + got, n - got, 0);
        if (r <= 0) { return -1; }
        got += r;
    }
    return got;
}

static int sendAll(SOCKET s, const uint8_t* b, int n)
{
    int sent = 0;
    while (sent < n) {
        int r = send(s, (const char*)b + sent, n - sent, 0);
        if (r <= 0) { return -1; }
        sent += r;
    }
    return sent;
}

static int listenOnLoopback(uint16_t* portOut)
{
    SOCKET l;
    struct sockaddr_in a;
    int one = 1, len;
    l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) { return -1; }
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(l, (struct sockaddr*)&a, sizeof(a)) != 0) { closesocket(l); return -2; }
    if (listen(l, 4) != 0) { closesocket(l); return -3; }
    len = sizeof(a);
    if (getsockname(l, (struct sockaddr*)&a, &len) == 0) { *portOut = ntohs(a.sin_port); }
    return (int)l;
}

static SOCKET connectLoopback(uint16_t port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    if (s == INVALID_SOCKET) { return INVALID_SOCKET; }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
    setRecvTimeout(s, 5000);
    return s;
}

/* ---- controller -------------------------------------------------------------------- */

static void fillIdentifyController(uint8_t* d)
{
    memset(d, 0, 4096);
    d[0] = 0x36; d[1] = 0x1C;
    d[2] = 0x36; d[3] = 0x1C;
    memcpy(d + 4,  "NDTCP000000000000001", 20);
    memcpy(d + 24, "NVMe-oF/TCP self-test controller          ", 40);
    memcpy(d + 64, "1.0.0   ", 8);
}

/* Identify Namespace with the LBA Format table where NVMe 1.4 puts it: NSZE at 0,
 * NLBAF at 25, FLBAS at 26 and LBAF0 at 128 (MS 128..129, LBADS 130, RP 131).  The
 * offsets are written one per line because writing this table four bytes late is a
 * mistake that a length check cannot see: the data is still 4096 bytes and every
 * other field still decodes, only the block size silently reads back as zero. */
#define SELFTEST_NS_BLOCKS 32768u

static void fillIdentifyNamespace(uint8_t* d, uint32_t blocks)
{
    uint64_t nsze = (uint64_t)blocks;
    memset(d, 0, 4096);
    memcpy(d, &nsze, 8);                     /* NSZE  at 0  */
    memcpy(d + 8, &nsze, 8);                 /* NCAP  at 8  */
    memcpy(d + 16, &nsze, 8);                /* NUSE  at 16 */
    d[25] = 0;                               /* NLBAF at 25: one format (zero based) */
    d[26] = 0;                               /* FLBAS at 26: format 0, no metadata */
    d[128] = 0;                              /* LBAF0 MS low  */
    d[129] = 0;                              /* LBAF0 MS high */
    d[130] = 9;                              /* LBAF0 LBADS: 2^9 = 512 bytes */
    d[131] = 0;                              /* LBAF0 RP */
}

/* Handshake: read ICReq, answer ICResp. */
static int controllerHandshake(SOCKET s)
{
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN], resp[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_IC ic, mine;
    NVMEOF_TCP_HDR h;
    if (recvAll(s, buf, 8) != 8) { return -1; }
    nvmeofTcpReadHdr(buf, &h);
    if (h.type != NVMEOF_TCP_ICREQ) { return -2; }
    if (recvAll(s, buf + 8, NVMEOF_TCP_IC_BODY_LEN) != (int)NVMEOF_TCP_IC_BODY_LEN) { return -3; }
    if (nvmeofTcpParseIc(buf, NVMEOF_TCP_ICREQ_LEN, &ic) != 0) { return -4; }
    memset(&mine, 0, sizeof(mine));
    mine.cpda = NVMEOF_TCP_PDU_ALIGN;
    mine.dgst = ic.dgst;
    nvmeofTcpBuildIc(resp, NVMEOF_TCP_ICRESP, &mine);
    return sendAll(s, resp, NVMEOF_TCP_ICREQ_LEN) == (int)NVMEOF_TCP_ICREQ_LEN ? 0 : -5;
}

/* Serve one capsule.  Returns 0 when a response was sent. */
static int controllerServeOne(SOCKET s, uint8_t* ns)
{
    uint8_t hdr[8], sqe[64], cqe[16], resp[NVMEOF_TCP_CRSP_HDR_LEN + 4096];
    NVMEOF_TCP_HDR h;
    uint16_t cid;
    uint8_t opcode;
    uint32_t cdw10;
    unsigned hlen, dataLen = 0;
    int n;

    if (recvAll(s, hdr, 8) != 8) { return -1; }
    nvmeofTcpReadHdr(hdr, &h);
    if (h.type != NVMEOF_TCP_CAPSULECMD || h.hlen != NVMEOF_TCP_CCMD_HDR_LEN) { return -2; }
    if (recvAll(s, sqe, 64) != 64) { return -3; }
    /* In-capsule write data would follow the SQE; this controller does not accept it. */
    if (h.plen != 0) { return -4; }

    cid = sqeCid(sqe);
    opcode = sqeOpcode(sqe);
    cdw10 = sqeCdw10(sqe);

    if (opcode == NVME_IDENTIFY && (cdw10 & 0xFF) == NVME_CNS_CONTROLLER) {
        fillIdentifyController(resp + NVMEOF_TCP_CRSP_HDR_LEN);
        dataLen = 4096;
        cqeBuild(cqe, 0, cid, NVME_SC_SUCCESS);
    } else if (opcode == NVME_IDENTIFY && (cdw10 & 0xFF) == NVME_CNS_NAMESPACE) {
        fillIdentifyNamespace(resp + NVMEOF_TCP_CRSP_HDR_LEN, SELFTEST_NS_BLOCKS);
        dataLen = 4096;
        cqeBuild(cqe, 0, cid, NVME_SC_SUCCESS);
    } else if (opcode == NVME_READ && ns != NULL) {
        /* One 512-byte block at LBA cdw10.  The namespace is a plain byte array, so this is
         * a memcpy rather than a real device read - the transport is what is under test. */
        uint32_t lba = cdw10;
        memcpy(resp + NVMEOF_TCP_CRSP_HDR_LEN, ns + (size_t)lba * 512u, 512);
        dataLen = 512;
        cqeBuild(cqe, 0, cid, NVME_SC_SUCCESS);
    } else {
        dataLen = 0;
        cqeBuild(cqe, 0, cid, NVME_SC_INVALID_FIELD);
    }

    hlen = nvmeofTcpBuildCapsuleResp(resp, cqe, cqeStatus(cqe) == NVME_SC_SUCCESS ? 1 : 0,
                                     dataLen, NVMEOF_TCP_CRSP_HDR_LEN);
    n = (int)(hlen + dataLen);
    return sendAll(s, resp, n) == n ? 0 : -5;
}

/* ---- host -------------------------------------------------------------------------- */

static int hostHandshake(SOCKET s)
{
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_IC ic;
    NVMEOF_TCP_HDR h;
    memset(&ic, 0, sizeof(ic));
    ic.pfv = 0; ic.cpda = 4;
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);
    if (sendAll(s, buf, NVMEOF_TCP_ICREQ_LEN) != (int)NVMEOF_TCP_ICREQ_LEN) { return -1; }
    if (recvAll(s, buf, 8) != 8) { return -2; }
    nvmeofTcpReadHdr(buf, &h);
    if (h.type != NVMEOF_TCP_ICRESP) { return -3; }
    if (recvAll(s, buf + 8, NVMEOF_TCP_IC_BODY_LEN) != (int)NVMEOF_TCP_IC_BODY_LEN) { return -4; }
    return 0;
}

/* Send one capsule and read the response.  dataOut may be NULL when no data is expected. */
static int hostCommand(SOCKET s, uint8_t opcode, uint32_t nsid, uint16_t cid,
                       uint32_t cdw10, uint8_t* dataOut, unsigned* dataLenOut, uint16_t* statusOut)
{
    uint8_t sqe[64], cap[NVMEOF_TCP_CCMD_HDR_LEN], hdr[8], cqe[16];
    NVMEOF_TCP_HDR h;
    unsigned n;
    sqeBuild(sqe, opcode, nsid, cid, cdw10, 0, 0);
    n = nvmeofTcpBuildCapsuleCmd(cap, sqe, 0, NVMEOF_TCP_CCMD_HDR_LEN);
    if (sendAll(s, cap, (int)n) != (int)n) { return -1; }
    if (recvAll(s, hdr, 8) != 8) { return -2; }
    nvmeofTcpReadHdr(hdr, &h);
    if (h.type != NVMEOF_TCP_CAPSULERESP) { return -3; }
    if (recvAll(s, cqe, 16) != 16) { return -4; }
    if (statusOut) { *statusOut = cqeStatus(cqe); }
    if (dataLenOut) { *dataLenOut = h.plen; }
    if (h.plen > 0) {
        if (dataOut == NULL) { return -5; }
        if (recvAll(s, dataOut, (int)h.plen) != (int)h.plen) { return -6; }
    }
    return 0;
}

/* ---- the tests --------------------------------------------------------------------- */

static void test_identify(void)
{
    uint16_t port = 0, status = 0xFFFF;
    unsigned len = 0;
    int ls;
    SOCKET c, srv;
    uint8_t ctrl[4096];

    printf("\n== Identify CNS=1 over NVMe/TCP, with 4096 bytes in the response capsule ==\n");
    ls = listenOnLoopback(&port);
    ok(ls >= 0 && port != 0, "controller listens");
    if (ls < 0) { return; }
    c = connectLoopback(port);
    ok(c != INVALID_SOCKET, "host connects");
    srv = accept((SOCKET)ls, NULL, NULL);
    ok(srv != INVALID_SOCKET, "controller accepts");
    setRecvTimeout(srv, 5000);

    /* send, serve, read - one step each. */
    memset(&(uint8_t){0}, 0, 1);
    {
        uint8_t buf[NVMEOF_TCP_ICREQ_LEN];
        NVMEOF_TCP_IC ic;
        NVMEOF_TCP_HDR h;
        memset(&ic, 0, sizeof(ic)); ic.cpda = 4;
        nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);
        ok(sendAll(c, buf, NVMEOF_TCP_ICREQ_LEN) == (int)NVMEOF_TCP_ICREQ_LEN, "host sends ICReq");
        ok(controllerHandshake(srv) == 0, "controller answers with ICResp");
        ok(recvAll(c, buf, NVMEOF_TCP_ICREQ_LEN) == (int)NVMEOF_TCP_ICREQ_LEN, "host reads ICResp");
        nvmeofTcpReadHdr(buf, &h);
        ok(h.type == NVMEOF_TCP_ICRESP, "the reply really is an ICResp");
    }
    (void)hostHandshake;

    memset(ctrl, 0, sizeof(ctrl));
    {
        uint8_t sqe[64], cap[NVMEOF_TCP_CCMD_HDR_LEN], hdr[8], cqe[16];
        NVMEOF_TCP_HDR h;
        unsigned n;
        sqeBuild(sqe, NVME_IDENTIFY, 0, 0x1234, NVME_CNS_CONTROLLER, 0, 0);
        n = nvmeofTcpBuildCapsuleCmd(cap, sqe, 0, NVMEOF_TCP_CCMD_HDR_LEN);
        ok(sendAll(c, cap, (int)n) == (int)n, "host sends an Identify capsule");
        ok(controllerServeOne(srv, NULL) == 0, "controller answers it");
        ok(recvAll(c, hdr, 8) == 8, "host reads the response header");
        nvmeofTcpReadHdr(hdr, &h);
        ok(h.type == NVMEOF_TCP_CAPSULERESP, "it is a CapsuleResp");
        ok((h.flags & NVMEOF_TCP_F_SUCCESS) != 0, "SUCCESS is set");
        ok(h.plen == 4096, "the response capsule announces 4096 bytes of data");
        ok(recvAll(c, cqe, 16) == 16, "host reads the 16-byte completion");
        status = cqeStatus(cqe);
        ok(status == NVME_SC_SUCCESS, "the completion says success");
        ok(cqeCid(cqe) == 0x1234, "the completion carries the command id it was given");
        ok(recvAll(c, ctrl, 4096) == 4096, "host reads all 4096 bytes of Identify data");
        len = 4096;
    }

    ok(len == 4096, "4096 bytes arrived");
    {
        uint16_t vid = (uint16_t)(ctrl[0] | ((uint16_t)ctrl[1] << 8));
        char sn[21], mn[41];
        memcpy(sn, ctrl + 4, 20);  sn[20] = 0;
        memcpy(mn, ctrl + 24, 40); mn[40] = 0;
        ok(vid == 0x1C36, "VID came back as 0x1C36");
        ok(strncmp(sn, "NDTCP000000000000001", 20) == 0, "the serial number survived the trip intact");
        ok(strncmp(mn, "NVMe-oF/TCP self-test controller", 32) == 0, "the model number survived the trip intact");
    }
    closesocket(c); closesocket(srv); closesocket((SOCKET)ls);
}

static void test_identify_namespace(void)
{
    uint16_t port = 0, status = 0xFFFF;
    int ls;
    SOCKET c, srv;
    uint8_t ns[4096];

    printf("\n== Identify Namespace: the LBA Format table at 128, LBADS read back at 130 ==\n");
    ls = listenOnLoopback(&port);
    ok(ls >= 0, "controller listens");
    if (ls < 0) { return; }
    c = connectLoopback(port);
    srv = accept((SOCKET)ls, NULL, NULL);
    ok(c != INVALID_SOCKET && srv != INVALID_SOCKET, "connected");
    setRecvTimeout(srv, 5000);

    memset(ns, 0, sizeof(ns));
    {
        uint8_t sqe[64], cap[NVMEOF_TCP_CCMD_HDR_LEN], hdr[8], cqe[16];
        NVMEOF_TCP_HDR h;
        unsigned n;
        sqeBuild(sqe, NVME_IDENTIFY, 1, 0x3333, NVME_CNS_NAMESPACE, 0, 0);
        n = nvmeofTcpBuildCapsuleCmd(cap, sqe, 0, NVMEOF_TCP_CCMD_HDR_LEN);
        ok(sendAll(c, cap, (int)n) == (int)n, "host sends Identify CNS=0 for namespace 1");
        ok(controllerServeOne(srv, NULL) == 0, "controller answers it");
        ok(recvAll(c, hdr, 8) == 8, "host reads the response header");
        nvmeofTcpReadHdr(hdr, &h);
        ok(h.type == NVMEOF_TCP_CAPSULERESP, "it is a CapsuleResp");
        ok((h.flags & NVMEOF_TCP_F_SUCCESS) != 0, "SUCCESS is set");
        ok(h.plen == 4096, "the response capsule announces 4096 bytes of data");
        ok(recvAll(c, cqe, 16) == 16, "host reads the 16-byte completion");
        status = cqeStatus(cqe);
        ok(status == NVME_SC_SUCCESS, "the completion says success");
        ok(cqeCid(cqe) == 0x3333, "the completion carries the command id it was given");
        ok(recvAll(c, ns, 4096) == 4096, "host reads all 4096 bytes of namespace data");
    }

    /* Read the fields back out of the bytes that actually crossed the wire. */
    {
        uint64_t nsze = 0;
        unsigned nlbaFormats;
        memcpy(&nsze, ns, 8);
        ok(nsze == (uint64_t)SELFTEST_NS_BLOCKS, "NSZE at offset 0 is the block count the controller was given");
        /* NLBAF is zero based: 0 means one format, so the count is byte 25 + 1. */
        nlbaFormats = (unsigned)ns[25] + 1u;
        ok(nlbaFormats >= 1u && (unsigned)(ns[26] & 0x0F) <= (unsigned)ns[25],
           "NLBAF at 25 says at least one LBA format exists and FLBAS selects one of them");
        ok(ns[26] == 0, "FLBAS at 26 is 0");
        ok(ns[130] == 9, "LBADS read back at byte offset 130 is 9, so blocks are 2^9 = 512 bytes");
        ok((1u << ns[130]) == 512u, "the advertised block size really is 512 bytes");
        if (ns[130] != 9) {
            printf("       (LBAF0 bytes at 128..131 are %u %u %u %u; LBADS belongs in the third)\n",
                   (unsigned)ns[128], (unsigned)ns[129], (unsigned)ns[130], (unsigned)ns[131]);
        }
    }
    closesocket(c); closesocket(srv); closesocket((SOCKET)ls);
}

static void test_read_block(void)
{
    uint16_t port = 0, status = 0xFFFF;
    unsigned len = 0;
    int ls, i;
    SOCKET c, srv;
    uint8_t ns[512 * 8];
    uint8_t got[512];

    printf("\n== an NVMe Read whose 512 bytes ride inside the response capsule ==\n");
    for (i = 0; i < (int)sizeof(ns); i++) { ns[i] = (uint8_t)(i * 7 + 3); }

    ls = listenOnLoopback(&port);
    ok(ls >= 0, "controller listens");
    if (ls < 0) { return; }
    c = connectLoopback(port);
    srv = accept((SOCKET)ls, NULL, NULL);
    ok(c != INVALID_SOCKET && srv != INVALID_SOCKET, "connected");
    setRecvTimeout(srv, 5000);

    /* Skip the handshake here on purpose: the capsule path was proved above, and this test
     * is about the data payload of a response. */
    {
        uint8_t sqe[64], cap[NVMEOF_TCP_CCMD_HDR_LEN], hdr[8], cqe[16];
        NVMEOF_TCP_HDR h;
        unsigned n;
        sqeBuild(sqe, NVME_READ, 1, 0x2222, 3 /* LBA 3 */, 0, 0);
        n = nvmeofTcpBuildCapsuleCmd(cap, sqe, 0, NVMEOF_TCP_CCMD_HDR_LEN);
        ok(sendAll(c, cap, (int)n) == (int)n, "host sends a Read for LBA 3");
        ok(controllerServeOne(srv, ns) == 0, "controller answers");
        ok(recvAll(c, hdr, 8) == 8, "host reads the response header");
        nvmeofTcpReadHdr(hdr, &h);
        ok(h.plen == 512, "the response carries 512 bytes");
        ok(recvAll(c, cqe, 16) == 16, "completion arrives");
        status = cqeStatus(cqe);
        ok(status == NVME_SC_SUCCESS, "the Read succeeded");
        ok(recvAll(c, got, 512) == 512, "all 512 bytes arrive");
        len = 512;
    }
    ok(len == 512, "512 bytes arrived");
    ok(memcmp(got, ns + 3 * 512, 512) == 0, "the bytes match LBA 3 of the namespace exactly");
    closesocket(c); closesocket(srv); closesocket((SOCKET)ls);
}

static void test_refusal(void)
{
    uint16_t port = 0, status = 0xFFFF;
    int ls;
    SOCKET c, srv;

    printf("\n== an unsupported opcode comes back as a refusal, not silence ==\n");
    ls = listenOnLoopback(&port);
    ok(ls >= 0, "controller listens");
    if (ls < 0) { return; }
    c = connectLoopback(port);
    srv = accept((SOCKET)ls, NULL, NULL);
    ok(c != INVALID_SOCKET && srv != INVALID_SOCKET, "connected");
    setRecvTimeout(srv, 5000);
    {
        uint8_t sqe[64], cap[NVMEOF_TCP_CCMD_HDR_LEN], hdr[8], cqe[16];
        NVMEOF_TCP_HDR h;
        unsigned n;
        sqeBuild(sqe, 0xFE, 0, 0x4321, 0, 0, 0);
        n = nvmeofTcpBuildCapsuleCmd(cap, sqe, 0, NVMEOF_TCP_CCMD_HDR_LEN);
        sendAll(c, cap, (int)n);
        ok(controllerServeOne(srv, NULL) == 0, "controller answered");
        ok(recvAll(c, hdr, 8) == 8, "host got a reply header");
        nvmeofTcpReadHdr(hdr, &h);
        ok(h.type == NVMEOF_TCP_CAPSULERESP, "it is a CapsuleResp");
        ok((h.flags & NVMEOF_TCP_F_SUCCESS) == 0, "SUCCESS is clear");
        ok(recvAll(c, cqe, 16) == 16, "the completion arrived");
        status = cqeStatus(cqe);
        ok(status == NVME_SC_INVALID_FIELD, "status is Invalid Field (0x02)");
        ok(cqeCid(cqe) == 0x4321, "the command id came back");
    }
    closesocket(c); closesocket(srv); closesocket((SOCKET)ls);
}

int main(void)
{
    WSADATA wsa;
    printf("NVMe/TCP identify and read over loopback\n");
    printf("========================================\n");
    setvbuf(stdout, NULL, _IONBF, 0);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup failed\n"); return 2; }
    test_identify();
    test_identify_namespace();
    test_read_block();
    test_refusal();
    printf("\n========================================\n");
    printf("pass %d  fail %d\n", g_pass, g_fail);
    WSACleanup();
    return g_fail == 0 ? 0 : 1;
}
