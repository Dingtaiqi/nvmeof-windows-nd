// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
//
//  tools_iscsi_proxy.cpp - a TCP proxy that dumps the target->initiator direction.
//
//  WHY THIS EXISTS
//  ---------------
//  The Windows iSCSI bridge in this project could not write more than one first burst
//  (64 KiB): a larger WRITE needs the target to send an R2T, and Windows answered every
//  R2T this bridge sent with "Target sent an invalid iSCSI PDU" (iScsiPrt event 23,
//  STATUS_NO_MEMORY) and dropped the connection.  Thirteen hypotheses died against
//  that wall (DESIGN 8.60) because our R2T is already byte-correct against the kernel's
//  struct iscsi_r2t_hdr.  What was missing was a REFERENCE: the R2T of a target
//  Windows accepts, to lay beside ours.
//
//  tgt on the Linux peer is that reference.  Capturing on the peer was the obvious
//  move and failed - tcpdump there is broken (undefined symbol pcap_findalldevs_ex, a
//  libpcap mismatch, not something to fix by upgrading somebody's system for a debug
//  run).  So the capture moved to this side, where the tooling is ours: the initiator
//  talks to this proxy, the proxy forwards to the reference target and records what
//  the TARGET sent.  The R2T is a target->initiator PDU, so that is the direction
//  worth having - and it is exactly 48 bytes with opcode 0x31, which this tool also
//  highlights so the interesting packet is not buried in a hex dump.
//
//  Usage:
//    tools_iscsi_proxy <listenPort> <targetHost> <targetPort> [dumpFile] [maxDumpBytes]
//
//  Only the first maxDumpBytes (default 16384) of the target->initiator stream are
//  written: the login exchange and the R2T both happen long before any bulk data, and
//  a megabyte of payload hex helps nobody.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <io.h>        // _fsopen
#include <share.h>     // _SH_DENYNO: let the dump be read while the proxy runs
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

static std::mutex g_dumpMutex;
static FILE*      g_dump = nullptr;
static size_t     g_maxDump = 16384;

// Everything the target sends, in order, as it arrives.  Two connections can be alive
// at once (Windows opens a discovery session and then a normal one), so the dump is
// labelled per connection rather than interleaved silently.
static void dumpBytes(int connId, const unsigned char* b, size_t n) {
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    if (!g_dump) return;
    // A PDU opcode is the low 6 bits of byte 0; R2T is 0x31 and is what we came for.
    if (n >= 1 && (b[0] & 0x3F) == 0x31 && n >= 48) {
        auto rd32 = [](const unsigned char* p) {
            return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
        };
        fprintf(g_dump, "\n*** conn %d: R2T (opcode 0x31) - 48 bytes ***\n", connId);
        for (int i = 0; i < 48; i += 16) {
            fprintf(g_dump, "  %2d:", i);
            for (int k = 0; k < 16; k++) fprintf(g_dump, " %02X", b[i + k]);
            fprintf(g_dump, "\n");
        }
        fprintf(g_dump, "  flags=0x%02X dlength=%u lun=%u (bytes 8..15 are zero)\n",
                b[1], (unsigned)b[5] << 16 | (unsigned)b[6] << 8 | b[7],
                rd32(b + 8));
        fprintf(g_dump, "  itt=0x%08X ttt=%u statsn=0x%08X expcmdsn=%u maxcmdsn=%u "
                        "r2tsn=%u offset=%u len=%u\n",
                rd32(b + 16), rd32(b + 20), rd32(b + 24), rd32(b + 28),
                rd32(b + 32), rd32(b + 36), rd32(b + 40), rd32(b + 44));
    }
    fprintf(g_dump, "[conn %d] %zu bytes:", connId, n);
    // Up to 512 bytes per chunk, not 64: the login response is a 300+ byte text blob
    // and the negotiated keys in it are the difference this tool exists to find.
    for (size_t i = 0; i < n && i < 512; i++) {
        fprintf(g_dump, " %02X", b[i]);
        if ((i % 16) == 15 && i + 1 < n && i + 1 < 512) fprintf(g_dump, "\n   ");
    }
    fprintf(g_dump, "\n");
    // Flushed per write, so the dump can be read while the proxy is still running -
    // the first version buffered, and "the file is 0 bytes" then looks exactly like
    // "the proxy never saw a connection".
    fflush(g_dump);
}

static void pump(SOCKET from, SOCKET to, int connId, bool record) {
    std::vector<unsigned char> buf(65536);
    size_t recorded = 0;
    for (;;) {
        int n = recv(from, (char*)buf.data(), (int)buf.size(), 0);
        if (n <= 0) break;
        if (record && recorded < g_maxDump) {
            size_t take = (size_t)n;
            if (recorded + take > g_maxDump) take = g_maxDump - recorded;
            dumpBytes(connId, buf.data(), take);
            recorded += take;
        }
        int sent = 0;
        while (sent < n) {
            int k = send(to, (const char*)buf.data() + sent, n - sent, 0);
            if (k <= 0) return;
            sent += k;
        }
    }
    shutdown(to, SD_BOTH);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: %s <listenPort> <targetHost> <targetPort> [dumpFile] [maxDumpBytes]\n", argv[0]);
        return 2;
    }
    int         listenPort = atoi(argv[1]);
    const char* host       = argv[2];
    int         targetPort = atoi(argv[3]);
    const char* dumpPath   = (argc > 4) ? argv[4] : "iscsi_proxy_dump.txt";
    if (argc > 5) g_maxDump = (size_t)atol(argv[5]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup failed\n"); return 1; }

    g_dump = _fsopen(dumpPath, "w", _SH_DENYNO);
    if (!g_dump) { printf("cannot open %s\n", dumpPath); return 1; }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { printf("socket failed\n"); return 1; }
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in me = {};
    me.sin_family = AF_INET;
    me.sin_port = htons((u_short)listenPort);
    inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
    if (bind(listener, (sockaddr*)&me, sizeof(me)) != 0) { printf("bind %d failed\n", listenPort); return 1; }
    if (listen(listener, 8) != 0) { printf("listen failed\n"); return 1; }
    printf("proxy: 127.0.0.1:%d -> %s:%d   dump=%s (first %zu bytes of each direction)\n",
           listenPort, host, targetPort, dumpPath, g_maxDump);
    fflush(stdout);

    int connId = 0;
    for (;;) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        int id = ++connId;

        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char portStr[16];
        sprintf_s(portStr, "%d", targetPort);
        if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) {
            printf("[conn %d] cannot resolve %s\n", id, host);
            closesocket(client);
            continue;
        }
        SOCKET upstream = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (upstream == INVALID_SOCKET || connect(upstream, res->ai_addr, (int)res->ai_addrlen) != 0) {
            printf("[conn %d] cannot reach %s:%d\n", id, host, targetPort);
            closesocket(client);
            if (upstream != INVALID_SOCKET) closesocket(upstream);
            freeaddrinfo(res);
            continue;
        }
        freeaddrinfo(res);
        BOOL nodelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        setsockopt(upstream, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        printf("[conn %d] connected\n", id);
        fflush(stdout);

        std::thread up([client, upstream, id]() { pump(client, upstream, id, false); });
        pump(upstream, client, id, true);      // the direction that carries the R2T
        up.join();
        closesocket(client);
        closesocket(upstream);
        printf("[conn %d] closed\n", id);
        fflush(stdout);
    }
    if (g_dump) fclose(g_dump);
    WSACleanup();
    return 0;
}
