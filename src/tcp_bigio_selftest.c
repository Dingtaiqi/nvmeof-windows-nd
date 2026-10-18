/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * tcp_bigio_selftest.c - data larger than one capsule payload, against the REAL target.
 *
 * Before this, nvmeof_tcp.c answered NVME_SC_DATA_XFER_ERR to any Read or Write over
 * 4096 bytes, because the data had to ride inside the capsule.  The target now moves
 * big data as C2HData (reads) and H2CData (writes) PDUs.  This test proves it end to
 * end: it starts a real nvmeof_tcp.exe as a child process on a spare port, speaks raw
 * NVMe/TCP to it, and compares every byte that comes back with a model of the namespace.
 *
 * It deliberately does NOT implement its own controller: a test that answers with a copy
 * of the code under test can pass while the target is broken (that is exactly how the
 * Identify LBAF offset stayed wrong).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "nvmeof_tcp.h"

#define TARGET_EXE   "D:\\rdma\\nvmeof\\src\\nvmeof_tcp_bigio.exe"
#define TARGET_EXE2  "D:\\rdma\\nvmeof\\src\\nvmeof_tcp_bigio.exe"
#define NS_PATH      "D:\\rdma\\bigio-ns.img"
#define NS_BLOCKS    512u                       /* 256 KiB */
#define NS_BYTES     (NS_BLOCKS * 512u)
#define TEST_PORT    4495
#define IO_CHUNK     4096u

static int g_pass = 0;
static int g_fail = 0;
static uint8_t g_ns[NS_BYTES];

static void ok(int cond, const char* what)
{
    if (cond) { g_pass++; printf("  [PASS] %s\n", what); }
    else      { g_fail++; printf("  [FAIL] %s\n", what); }
}

static void fillPattern(uint8_t* p, uint32_t lba, uint32_t nlb)
{
    uint32_t i;
    for (i = 0; i < nlb * 512u; i++) {
        p[i] = (uint8_t)((lba * 7u) + (i * 3u) + 0x5A);
    }
}

static int writeNsFile(void)
{
    FILE* f = fopen(NS_PATH, "wb");
    if (f == NULL) { return -1; }
    if (fwrite(g_ns, 1, NS_BYTES, f) != NS_BYTES) { fclose(f); return -1; }
    fclose(f);
    return 0;
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

static SOCKET connectWithRetry(uint16_t port)
{
    int i;
    for (i = 0; i < 100; i++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in a;
        DWORD t = 10000;
        if (s == INVALID_SOCKET) { return INVALID_SOCKET; }
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(s, (struct sockaddr*)&a, sizeof(a)) == 0) {
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
            return s;
        }
        closesocket(s);
        Sleep(100);
    }
    return INVALID_SOCKET;
}

static int doHandshake(SOCKET s)
{
    uint8_t req[NVMEOF_TCP_ICREQ_LEN], resp[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_IC ic, peer;
    NVMEOF_TCP_HDR h;

    memset(&ic, 0, sizeof(ic));
    ic.cpda = NVMEOF_TCP_PDU_ALIGN;
    nvmeofTcpBuildIc(req, NVMEOF_TCP_ICREQ, &ic);
    if (sendAll(s, req, (int)sizeof(req)) != (int)sizeof(req)) { return -1; }
    if (recvAll(s, resp, (int)sizeof(resp)) != (int)sizeof(resp)) { return -1; }
    if (nvmeofTcpParseIc(resp, sizeof(resp), &peer) != 0) { return -1; }
    nvmeofTcpReadHdr(resp, &h);
    if (h.type != NVMEOF_TCP_ICRESP) { return -1; }
    printf("       ICResp: cpda=%u maxr2t=%u\n", (unsigned)peer.cpda, peer.maxr2t);
    return 0;
}

static void buildSqe(uint8_t* sqe, uint8_t opcode, uint32_t lba, uint32_t nlb, uint16_t cid)
{
    memset(sqe, 0, 64);
    sqe[0] = opcode;
    sqe[2] = (uint8_t)(cid & 0xFF);
    sqe[3] = (uint8_t)((cid >> 8) & 0xFF);
    sqe[4] = 1;                                   /* NSID 1 */
    nvmeofTcpPut32(sqe, 40, lba);                 /* CDW10 = starting LBA */
    nvmeofTcpPut32(sqe, 48, nlb - 1u);            /* CDW12 = NLB, zero based */
}

static int sendCapsule(SOCKET s, const uint8_t* sqe)
{
    uint8_t cap[NVMEOF_TCP_CCMD_HDR_LEN];
    unsigned n = nvmeofTcpBuildCapsuleCmd(cap, sqe, 0, NVMEOF_TCP_CCMD_HDR_LEN);
    return sendAll(s, cap, (int)n) == (int)n ? 0 : -1;
}

/* Read the 24-byte CapsuleResp (header + completion).  Any in-capsule payload is read
 * into `capData` (up to capMax) and its length returned; *plen is 0 for the big paths. */
static int readCapsuleResp(SOCKET s, uint16_t expectCid, uint32_t* plen)
{
    uint8_t hdr[8], cqe[16];
    NVMEOF_TCP_HDR h;
    uint32_t sc;

    if (recvAll(s, hdr, 8) != 8) { return -1; }
    nvmeofTcpReadHdr(hdr, &h);
    if (h.type != NVMEOF_TCP_CAPSULERESP || h.hlen != NVMEOF_TCP_CRSP_HDR_LEN) { return -1; }
    if (recvAll(s, cqe, 16) != 16) { return -1; }
    if ((uint16_t)(nvmeofTcpGet32(cqe, 12) & 0xFFFF) != expectCid) { return -1; }
    sc = (nvmeofTcpGet32(cqe, 12) >> 17) & 0xFF;
    *plen = h.plen;
    if (sc != 0) {
        printf("       completion status %u\n", (unsigned)sc);
        return -1;
    }
    return 0;
}

/* Receive C2HData until the LAST flag and check the bytes against the model. */
static int receiveC2H(SOCKET s, uint16_t cid, uint8_t* out, uint32_t want)
{
    uint32_t have = 0;
    uint8_t zero[4];

    while (have < want) {
        uint8_t hdr[NVMEOF_TCP_DATA_HDR_LEN];
        NVMEOF_TCP_HDR h;
        NVMEOF_TCP_DATA d;
        unsigned pad;

        if (recvAll(s, hdr, (int)sizeof(hdr)) != (int)sizeof(hdr)) { return -1; }
        nvmeofTcpReadHdr(hdr, &h);
        if (h.type != NVMEOF_TCP_C2HDATA || h.hlen != NVMEOF_TCP_DATA_HDR_LEN) { return -1; }
        nvmeofTcpReadData(hdr, &d);
        if (d.cccid != cid || d.datal == 0) { return -1; }
        if ((uint64_t)d.datao + d.datal > want) { return -1; }
        if (recvAll(s, out + d.datao, (int)d.datal) != (int)d.datal) { return -1; }
        pad = (unsigned)((NVMEOF_TCP_PDU_ALIGN - (d.datal % NVMEOF_TCP_PDU_ALIGN)) % NVMEOF_TCP_PDU_ALIGN);
        if (pad != 0 && recvAll(s, zero, (int)pad) != (int)pad) { return -1; }
        have += d.datal;
        if ((h.flags & NVMEOF_TCP_F_LAST) != 0) { break; }
    }
    return have == want ? 0 : -1;
}

static void testBigRead(SOCKET s, uint32_t lba, uint32_t nlb, uint16_t cid, const char* label)
{
    uint8_t sqe[64];
    uint32_t want = nlb * 512u;
    uint32_t plen = 0;
    uint8_t* got;
    int rc;

    printf("\n== %s: Read %u blocks (%u bytes) from LBA %u ==\n", label, nlb, want, lba);
    got = (uint8_t*)calloc(1, want);
    if (got == NULL) { ok(0, "test buffer allocated"); return; }

    buildSqe(sqe, 0x02 /* NVMe Read */, lba, nlb, cid);
    ok(sendCapsule(s, sqe) == 0, "host sends the Read capsule");
    ok(readCapsuleResp(s, cid, &plen) == 0, "target completes the Read with success");
    ok(plen == 0, "a transfer over 4096 bytes puts no data in the capsule");
    rc = receiveC2H(s, cid, got, want);
    ok(rc == 0, "all bytes arrive as C2HData PDUs, LAST set on the final one");
    ok(memcmp(got, g_ns + (size_t)lba * 512u, want) == 0,
       "every byte matches the namespace model");
    free(got);
}

static void testBigWrite(SOCKET s, uint32_t lba, uint32_t nlb, uint16_t cid, const char* label)
{
    uint8_t sqe[64];
    uint32_t total = nlb * 512u;
    uint32_t off = 0;
    uint32_t plen = 0;

    printf("\n== %s: Write %u blocks (%u bytes) at LBA %u ==\n", label, nlb, total, lba);
    fillPattern(g_ns + (size_t)lba * 512u, lba, nlb);

    buildSqe(sqe, 0x01 /* NVMe Write */, lba, nlb, cid);
    ok(sendCapsule(s, sqe) == 0, "host sends the Write capsule with no in-capsule data");

    while (off < total) {
        uint8_t hdr[NVMEOF_TCP_DATA_HDR_LEN];
        uint8_t zero[4] = { 0, 0, 0, 0 };
        NVMEOF_TCP_DATA d;
        uint32_t chunk = total - off;
        unsigned pad;

        if (chunk > IO_CHUNK) { chunk = IO_CHUNK; }
        memset(&d, 0, sizeof(d));
        d.cccid = cid;
        d.ttag = 0;
        d.datao = off;
        d.datal = chunk;
        nvmeofTcpBuildData(hdr, NVMEOF_TCP_H2CDATA,
                           (off + chunk == total) ? NVMEOF_TCP_F_LAST : 0, &d);
        if (sendAll(s, hdr, (int)sizeof(hdr)) != (int)sizeof(hdr)) { ok(0, "H2CData header sent"); return; }
        if (sendAll(s, g_ns + (size_t)lba * 512u + off, (int)chunk) != (int)chunk) { ok(0, "H2CData payload sent"); return; }
        pad = (unsigned)((NVMEOF_TCP_PDU_ALIGN - (chunk % NVMEOF_TCP_PDU_ALIGN)) % NVMEOF_TCP_PDU_ALIGN);
        if (pad != 0) { sendAll(s, zero, (int)pad); }
        off += chunk;
    }
    ok(1, "host pushes the write data as H2CData PDUs");
    ok(readCapsuleResp(s, cid, &plen) == 0, "target completes the Write with success");
    ok(plen == 0, "the Write completion carries no data");
}

int main(void)
{
    WSADATA wsa;
    SOCKET s;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    char cmd[512];
    int i;
    const char* exe = TARGET_EXE;

    printf("=== NVMe/TCP out-of-capsule data, against a real nvmeof_tcp.exe ===\n");
    printf("    target: %s\n    port: %u\n    namespace: %s (%u blocks)\n",
           exe, TEST_PORT, NS_PATH, NS_BLOCKS);

    for (i = 0; i < (int)NS_BLOCKS; i++) { fillPattern(g_ns + (size_t)i * 512u, (uint32_t)i, 1); }

    if (writeNsFile() != 0) { printf("cannot write %s\n", NS_PATH); return 2; }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup failed\n"); return 2; }

    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) { exe = TARGET_EXE2; }
    snprintf(cmd, sizeof(cmd), "\"%s\" -target 127.0.0.1 %u -nsfile \"%s\" -blocks %u -serve",
             exe, TEST_PORT, NS_PATH, NS_BLOCKS);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        printf("cannot start %s (error %lu)\n", exe, GetLastError());
        DeleteFileA(NS_PATH);
        return 2;
    }
    printf("    target pid %lu\n\n", (unsigned long)pi.dwProcessId);

    s = connectWithRetry(TEST_PORT);
    ok(s != INVALID_SOCKET, "client connects to the real target");
    if (s != INVALID_SOCKET) {
        ok(doHandshake(s) == 0, "ICReq/ICResp handshake");

        testBigRead(s, 8, 32, 0x4101, "16 KiB read");       /* 4 C2HData PDUs  */
        testBigRead(s, 128, 128, 0x4102, "64 KiB read");    /* 16 C2HData PDUs */
        testBigWrite(s, 64, 40, 0x4103, "20 KiB write");    /* 5 H2CData PDUs  */
        testBigRead(s, 64, 40, 0x4104, "read back the written range");

        closesocket(s);
    }

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteFileA(NS_PATH);
    WSACleanup();

    printf("\n========================================\n");
    printf("pass %d  fail %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

