/* tcp_bwprobe.c - raw loopback TCP bandwidth, no NVMe/TCP anywhere.
 *
 * Discriminates "the platform's loopback TCP is ~100 MB/s per stream" from "our
 * NVMe/TCP stack is": every path measured so far (kernel miniport, user-mode
 * bridge) converges on ~85-100 MB/s per stream, which is suspiciously low for
 * loopback.  This moves bytes over a plain TCP socket and prints MB/s.
 *
 *   tcp_bwprobe -serve [port] [mib]          receive M MiB, discard, print
 *   tcp_bwprobe -runs [port] [mib]           send M MiB, print (client)
 *   tcp_bwprobe -runr [port] [mib]           receive M MiB as client, print
 *                                            (pair with -serves)
 *   tcp_bwprobe -sends [port] [mib]          send M MiB as server-side peer
 *
 * blocks are 64 KiB by default - same size as the transport's PDUs.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

static int sendAll(SOCKET s, const char* p, int n)
{
    int off = 0;
    while (off < n) {
        int k = send(s, p + off, n - off, 0);
        if (k <= 0) { return -1; }
        off += k;
    }
    return 0;
}

static double nowSec(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

int main(int argc, char** argv)
{
    WSADATA w;
    const char* mode = (argc > 1) ? argv[1] : "-runr";
    int port = (argc > 2) ? atoi(argv[2]) : 4590;
    long mib = (argc > 3) ? atol(argv[3]) : 256;
    int blockKib = (argc > 4) ? atoi(argv[4]) : 64;
    size_t block = (size_t)blockKib * 1024;
    long long total = (long long)mib << 20;
    char* buf = (char*)malloc(block);
    SOCKET a, b;
    double t0, dt;
    long long got = 0;
    struct sockaddr_in sa;
    int one = 1;

    if (buf == NULL || block == 0) { return 2; }
    memset(buf, 0x5A, block);
    WSAStartup(MAKEWORD(2, 2), &w);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

    if (!strcmp(mode, "-serve") || !strcmp(mode, "-serves")) {
        struct sockaddr_in ca;
        int cl = sizeof(ca);
        a = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(a, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
        if (bind(a, (struct sockaddr*)&sa, sizeof(sa)) != 0 || listen(a, 1) != 0) {
            printf("bind/listen failed %d\n", WSAGetLastError());
            return 1;
        }
        b = accept(a, (struct sockaddr*)&ca, &cl);
        if (b == INVALID_SOCKET) { printf("accept failed\n"); return 1; }
        if (!strcmp(mode, "-serves")) {
            t0 = nowSec();
            while (got < total) {
                long long want = total - got;
                if (want > block) { want = block; }
                if (sendAll(b, buf, (int)want) != 0) { printf("send broke\n"); return 1; }
                got += want;
            }
            dt = nowSec() - t0;
            printf("SENT %lld %.6f  %.1f MB/s\n", got, dt, got / 1048576.0 / dt);
        } else {
            t0 = nowSec();
            while (got < total) {
                int k = recv(b, buf, block, 0);
                if (k <= 0) { break; }
                got += k;
            }
            dt = nowSec() - t0;
            printf("RECV %lld %.6f  %.1f MB/s\n", got, dt, got / 1048576.0 / dt);
        }
        closesocket(b);
        closesocket(a);
    } else {
        b = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(b, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
            printf("connect failed %d\n", WSAGetLastError());
            return 1;
        }
        if (!strcmp(mode, "-runs")) {
            t0 = nowSec();
            while (got < total) {
                long long want = total - got;
                if (want > block) { want = block; }
                if (sendAll(b, buf, (int)want) != 0) { printf("send broke\n"); return 1; }
                got += want;
            }
            dt = nowSec() - t0;
            printf("SENT %lld %.6f  %.1f MB/s\n", got, dt, got / 1048576.0 / dt);
        } else {
            t0 = nowSec();
            while (got < total) {
                int k = recv(b, buf, block, 0);
                if (k <= 0) { break; }
                got += k;
            }
            dt = nowSec() - t0;
            printf("RECV %lld %.6f  %.1f MB/s\n", got, dt, got / 1048576.0 / dt);
        }
        closesocket(b);
    }
    free(buf);
    WSACleanup();
    return 0;
}

