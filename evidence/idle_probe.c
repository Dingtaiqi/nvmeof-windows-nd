/* idle_probe.c - user-mode reproducer for the kernel transport's send failure.
 *
 * It speaks the same wire protocol as src/driver/nvmeof_tcpk.c (the PDU layer
 * comes from the same header, src/nvmeof_tcp.h, so the bytes are identical),
 * issues a few commands, then goes silent for longer than the target's
 * SO_RCVTIMEO (60000 ms in src/nvmeof_tcp.c, targetConnThread) and reports
 * exactly which socket call fails and with which Winsock error.
 *
 * Built and kept as evidence; it is not part of the driver and is never loaded.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/nvmeof_tcp.h"

static SOCKET g_sock = INVALID_SOCKET;
static uint16_t g_cid = 1;

static void stamp(const char* what)
{
    printf("[%6llu ms] %s\n", (unsigned long long)GetTickCount64(), what);
    fflush(stdout);
}

/* Returns the number of bytes sent, or -1 and prints the Winsock error. */
static int sendAll(const char* tag, const uint8_t* b, int n)
{
    int sent = 0;
    while (sent < n) {
        int r = send(g_sock, (const char*)b + sent, n - sent, 0);
        if (r <= 0) {
            printf("    %s: send() returned %d, WSAGetLastError=%d\n",
                   tag, r, WSAGetLastError());
            return -1;
        }
        sent += r;
    }
    return sent;
}

static int recvAll(const char* tag, uint8_t* b, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(g_sock, (char*)b + got, n - got, 0);
        if (r <= 0) {
            printf("    %s: recv() returned %d, WSAGetLastError=%d\n",
                   tag, r, WSAGetLastError());
            return -1;
        }
        got += r;
    }
    return got;
}

/* One NVMe read of one block, exactly the capsule the driver sends. */
static int doRead(uint32_t lba, const char* tag)
{
    uint8_t cmd[128];
    uint8_t sqe[NVMEOF_TCP_SQE_LEN];
    uint8_t hdr[NVMEOF_TCP_HDR_LEN];
    uint8_t cqe[NVMEOF_TCP_CQE_LEN];
    uint8_t data[4096];
    NVMEOF_TCP_HDR h;
    uint16_t cid = g_cid++;

    memset(cmd, 0, sizeof(cmd));
    memset(sqe, 0, sizeof(sqe));
    sqe[0] = 0x02;                                  /* NVMe READ            */
    sqe[2] = (uint8_t)(cid & 0xFF);
    sqe[3] = (uint8_t)((cid >> 8) & 0xFF);
    nvmeofTcpPut32(sqe, 4, 1);                      /* NSID 1               */
    nvmeofTcpPut32(sqe, 40, lba);                   /* CDW10 = SLBA         */
    nvmeofTcpPut32(sqe, 48, 0);                     /* CDW12 = NLB 0 (1 blk)*/
    nvmeofTcpBuildCapsuleCmd(cmd, sqe, 0, NVMEOF_TCP_CCMD_HDR_LEN);

    if (sendAll(tag, cmd, (int)NVMEOF_TCP_CCMD_HDR_LEN) != (int)NVMEOF_TCP_CCMD_HDR_LEN) {
        return -1;
    }
    if (recvAll(tag, hdr, (int)NVMEOF_TCP_HDR_LEN) != (int)NVMEOF_TCP_HDR_LEN) {
        return -1;
    }
    nvmeofTcpReadHdr(hdr, &h);
    if (h.type != NVMEOF_TCP_CAPSULERESP) {
        printf("    %s: response type 0x%02X, expected 0x05\n", tag, (unsigned)h.type);
        return -1;
    }
    if (recvAll(tag, cqe, (int)NVMEOF_TCP_CQE_LEN) != (int)NVMEOF_TCP_CQE_LEN) {
        return -1;
    }
    if (h.plen != 0 && recvAll(tag, data, (int)h.plen) != (int)h.plen) {
        return -1;
    }
    printf("    %s: cid=%u plen=%u status=0x%02X first=%02X %02X %02X %02X\n",
           tag, (unsigned)cid, (unsigned)h.plen,
           (unsigned)((nvmeofTcpGet32(cqe, 12) >> 17) & 0xFF),
           data[0], data[1], data[2], data[3]);
    return 0;
}

int main(int argc, char** argv)
{
    WSADATA wsa;
    struct sockaddr_in a;
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_IC ic;
    NVMEOF_TCP_HDR h;
    const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 4421;
    int idle = (argc > 3) ? atoi(argv[3]) : 70;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == INVALID_SOCKET) { printf("socket failed %d\n", WSAGetLastError()); return 1; }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    stamp("connecting");
    if (connect(g_sock, (struct sockaddr*)&a, sizeof(a)) != 0) {
        printf("connect failed %d\n", WSAGetLastError());
        return 2;
    }
    stamp("connected");

    memset(&ic, 0, sizeof(ic));
    ic.cpda = 4;
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);
    if (sendAll("icreq", buf, (int)NVMEOF_TCP_ICREQ_LEN) != (int)NVMEOF_TCP_ICREQ_LEN) { return 3; }
    if (recvAll("icresp", buf, (int)NVMEOF_TCP_ICREQ_LEN) != (int)NVMEOF_TCP_ICREQ_LEN) { return 4; }
    nvmeofTcpReadHdr(buf, &h);
    if (h.type != NVMEOF_TCP_ICRESP) { printf("not an ICResp: 0x%02X\n", (unsigned)h.type); return 5; }
    stamp("handshake ok");

    if (doRead(0, "read-before-1") != 0) { printf("FAIL before any idle\n"); return 6; }
    if (doRead(0, "read-before-2") != 0) { printf("FAIL before any idle\n"); return 6; }
    stamp("two commands served; the connection is healthy");

    printf("--- going silent for %d s (target SO_RCVTIMEO is 60000 ms) ---\n", idle);
    fflush(stdout);
    Sleep((DWORD)idle * 1000);

    stamp("woke up; sending the same capsule the driver sends");
    if (doRead(8, "read-after-idle") != 0) {
        stamp("RESULT: the command after the idle period FAILED");
        closesocket(g_sock);
        WSACleanup();
        return 7;
    }
    stamp("RESULT: the command after the idle period succeeded");
    closesocket(g_sock);
    WSACleanup();
    return 0;
}
