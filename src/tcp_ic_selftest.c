/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * tcp_ic_selftest.c - the NVMe/TCP connection handshake, end to end over loopback.
 *
 * ROADMAP E1a step 2.  The PDU layer (nvmeof_tcp.h) is checked byte for byte by
 * tcp_selftest.c; this file proves the part that file cannot - that the handshake actually
 * happens between two real sockets on this machine, in the order the transport specifies,
 * and that both sides agree on what was negotiated.
 *
 * It is deliberately both ends in one process.  The whole reason NVMe/TCP matters here is
 * that it needs no RDMA hardware, no second machine and no exemption from the hypervisor,
 * so the first milestone should be provable on one box with nothing attached.  The server
 * half of this file is also the shape the real target will take: accept, read an ICReq,
 * answer with an ICResp, and only then let capsules flow.  A connection that has not
 * completed the handshake must never see a capsule, which is why the state is explicit
 * rather than implied.
 *
 *   build:  cl /nologo /W4 /std:c17 tcp_ic_selftest.c /Fe:tcp_ic_selftest.exe /link ws2_32.lib
 *   run:    .\tcp_ic_selftest.exe
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

/* Read exactly n bytes, or fail.  A short read on a stream socket is normal and must be
 * handled: TCP gives no message boundaries, so "one recv per PDU" is a bug that works
 * until the day the peer writes its header and its payload in separate segments. */
/* A receive timeout, so a peer that stops talking produces a failed check rather than
 * a test run that never ends.  5 seconds is far longer than loopback needs. */
static void setRecvTimeout(SOCKET s, int ms)
{
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
}

static int recvAll(SOCKET s, uint8_t* buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(s, (char*)buf + got, n - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    return got;
}

static int sendAll(SOCKET s, const uint8_t* buf, int n)
{
    int sent = 0;
    while (sent < n) {
        int r = send(s, (const char*)buf + sent, n - sent, 0);
        if (r <= 0) {
            return -1;
        }
        sent += r;
    }
    return sent;
}

/* --------------------------------------------------------------------------- */
/* The controller side: what the real target will do, in miniature.            */
/* --------------------------------------------------------------------------- */

typedef struct {
    SOCKET  listenSock;
    SOCKET  conn;
    uint16_t port;
    NVMEOF_TCP_CONN state;
    unsigned icreqSeen;
    unsigned badPdus;
} NVMEOF_TCP_SRV;

static int srvStart(NVMEOF_TCP_SRV* s, uint16_t port)
{
    struct sockaddr_in a;
    int one = 1;

    memset(s, 0, sizeof(*s));
    s->listenSock = INVALID_SOCKET;
    s->conn = INVALID_SOCKET;
    s->state.state = NVMEOF_TCP_ST_FRESH;

    s->listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->listenSock == INVALID_SOCKET) {
        return -1;
    }
    setsockopt(s->listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(s->listenSock, (struct sockaddr*)&a, sizeof(a)) != 0) {
        return -2;
    }
    if (listen(s->listenSock, 4) != 0) {
        return -3;
    }
    /* Port 0 means "pick one"; read back what the stack chose so the client can use it. */
    {
        int len = sizeof(a);
        if (getsockname(s->listenSock, (struct sockaddr*)&a, &len) == 0) {
            s->port = ntohs(a.sin_port);
        }
    }
    return 0;
}

/* One ICReq in, one ICResp out.  Returns 0 on success. */
static int srvHandshake(NVMEOF_TCP_SRV* s)
{
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN + NVMEOF_TCP_DIGEST_LEN];
    NVMEOF_TCP_IC ic;
    NVMEOF_TCP_IC mine;
    uint8_t resp[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_HDR h;
    int rc;

    /* The very first thing on a fresh connection must be an ICReq.  Anything else means
     * the peer is not speaking this protocol, and guessing would only hide that. */
    rc = recvAll(s->conn, buf, NVMEOF_TCP_HDR_LEN);
    if (rc != NVMEOF_TCP_HDR_LEN) {
        return -1;
    }
    nvmeofTcpReadHdr(buf, &h);
    if (h.type != NVMEOF_TCP_ICREQ) {
        s->badPdus++;
        return -2;
    }
    if (h.hlen != NVMEOF_TCP_ICREQ_LEN) {
        s->badPdus++;
        return -3;
    }
    /* We already hold the 8-byte header; read the remaining 120 bytes of the body. */
    if (recvAll(s->conn, buf + NVMEOF_TCP_HDR_LEN, NVMEOF_TCP_IC_BODY_LEN) != (int)NVMEOF_TCP_IC_BODY_LEN) {
        return -4;
    }
    if (nvmeofTcpParseIc(buf, NVMEOF_TCP_ICREQ_LEN, &ic) != 0) {
        s->badPdus++;
        return -5;
    }
    s->state.peer = ic;
    s->icreqSeen++;
    s->state.state = NVMEOF_TCP_ST_IC_DONE;

    /* Our answer.  maxr2t = 0 states that we never ask the host to send a range back, which
     * is the simplest thing that is still conformant: the sender pushes all data and we
     * never issue an R2T.  cpda is echoed at the minimum the specification allows. */
    memset(&mine, 0, sizeof(mine));
    mine.pfv = 0;
    mine.cpda = NVMEOF_TCP_PDU_ALIGN;
    mine.dgst = ic.dgst;                     /* honour whatever the host asked for */
    mine.maxr2t = NVMEOF_TCP_MAXR2T_DEFAULT;
    mine.hdgst = ic.hdgst;
    mine.ddgst = ic.ddgst;
    nvmeofTcpBuildIc(resp, NVMEOF_TCP_ICRESP, &mine);

    if (sendAll(s->conn, resp, NVMEOF_TCP_ICREQ_LEN) != (int)NVMEOF_TCP_ICREQ_LEN) {
        return -6;
    }
    return 0;
}

static void srvStop(NVMEOF_TCP_SRV* s)
{
    if (s->conn != INVALID_SOCKET) { closesocket(s->conn); s->conn = INVALID_SOCKET; }
    if (s->listenSock != INVALID_SOCKET) { closesocket(s->listenSock); s->listenSock = INVALID_SOCKET; }
}

/* --------------------------------------------------------------------------- */
/* The host side.                                                              */
/* --------------------------------------------------------------------------- */

static SOCKET hostConnect(uint16_t port)
{
    struct sockaddr_in a;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    setRecvTimeout(s, 5000);
    return s;
}

static int hostSendIcReq(SOCKET s, const NVMEOF_TCP_IC* ic)
{
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN];
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, ic);
    return sendAll(s, buf, NVMEOF_TCP_ICREQ_LEN) == (int)NVMEOF_TCP_ICREQ_LEN ? 0 : -1;
}

static int hostReadIcResp(SOCKET s, NVMEOF_TCP_IC* out)
{
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_HDR h;
    if (recvAll(s, buf, NVMEOF_TCP_HDR_LEN) != NVMEOF_TCP_HDR_LEN) {
        return -1;
    }
    nvmeofTcpReadHdr(buf, &h);
    if (h.type != NVMEOF_TCP_ICRESP) {
        return -2;
    }
    if (recvAll(s, buf + NVMEOF_TCP_HDR_LEN, NVMEOF_TCP_IC_BODY_LEN) != (int)NVMEOF_TCP_IC_BODY_LEN) {
        return -3;
    }
    return nvmeofTcpParseIc(buf, NVMEOF_TCP_ICREQ_LEN, out);
}

/* --------------------------------------------------------------------------- */

static void test_handshake(void)
{
    NVMEOF_TCP_SRV srv;
    NVMEOF_TCP_IC host, ctrl;
    SOCKET c;
    int rc;

    printf("\n== ICReq / ICResp over a real loopback socket ==\n");

    ok(srvStart(&srv, 0) == 0, "controller listens on a loopback port");
    ok(srv.port != 0, "the stack assigned a port");
    if (srv.port == 0) { srvStop(&srv); return; }
    printf("         (port %u)\n", srv.port);

    c = hostConnect(srv.port);
    ok(c != INVALID_SOCKET, "host connects");
    if (c == INVALID_SOCKET) { srvStop(&srv); return; }

    srv.conn = accept(srv.listenSock, NULL, NULL);
    if (srv.conn != INVALID_SOCKET) { setRecvTimeout(srv.conn, 5000); }
    ok(srv.conn != INVALID_SOCKET, "controller accepts");
    if (srv.conn == INVALID_SOCKET) { closesocket(c); srvStop(&srv); return; }

    /* Ask for digests and a larger alignment than the minimum, so the negotiation has
     * something to actually negotiate about. */
    memset(&host, 0, sizeof(host));
    host.pfv = 0;
    host.cpda = 8;
    host.dgst = 1;
    host.maxr2t = 0;
    host.hdgst = 1;
    host.ddgst = 1;

    ok(srv.state.state == NVMEOF_TCP_ST_FRESH, "a fresh connection starts in the fresh state");
    ok(hostSendIcReq(c, &host) == 0, "host sends ICReq (128 bytes)");

    rc = srvHandshake(&srv);
    ok(rc == 0, "controller completes the handshake");
    ok(srv.icreqSeen == 1, "controller saw exactly one ICReq");
    ok(srv.state.state == NVMEOF_TCP_ST_IC_DONE, "controller moved to ic-done");
    ok(srv.state.peer.cpda == 8 && srv.state.peer.dgst == 1 && srv.state.peer.hdgst == 1,
       "controller parsed the host's ICReq fields");

    rc = hostReadIcResp(c, &ctrl);
    ok(rc == 0, "host parses ICResp");
    ok(ctrl.cpda == NVMEOF_TCP_PDU_ALIGN, "ICResp announces the minimum alignment (4)");
    ok(ctrl.maxr2t == 0, "ICResp says the controller will not issue R2T");
    ok(ctrl.dgst == 1 && ctrl.hdgst == 1 && ctrl.ddgst == 1,
       "digests are honoured, not silently dropped");
    ok(ctrl.pfv == 0, "PDU format version is 0");

    closesocket(c);
    srvStop(&srv);
}

static void test_bad_first_pdu(void)
{
    NVMEOF_TCP_SRV srv;
    SOCKET c;
    uint8_t capsule[NVMEOF_TCP_CCMD_HDR_LEN];
    uint8_t sqe[64];

    printf("\n== a connection whose first PDU is not an ICReq ==\n");

    ok(srvStart(&srv, 0) == 0, "controller listens");
    c = hostConnect(srv.port);
    ok(c != INVALID_SOCKET, "host connects");
    srv.conn = accept(srv.listenSock, NULL, NULL);
    if (srv.conn != INVALID_SOCKET) { setRecvTimeout(srv.conn, 5000); }
    ok(srv.conn != INVALID_SOCKET, "controller accepts");

    /* This is the mistake worth guarding against: sending a capsule before the handshake.
     * A controller that accepts it would be inventing a protocol, so this one must refuse
     * it and count it. */
    memset(sqe, 0, sizeof(sqe));
    nvmeofTcpBuildCapsuleCmd(capsule, sqe, 0, 8);
    sendAll(c, capsule, NVMEOF_TCP_CCMD_HDR_LEN);

    ok(srvHandshake(&srv) == -2, "a CapsuleCmd before the handshake is refused");
    ok(srv.badPdus == 1, "the refusal is counted, not swallowed");
    ok(srv.icreqSeen == 0, "no ICReq was recorded");

    closesocket(c);
    srvStop(&srv);
}

static void test_two_connections(void)
{
    NVMEOF_TCP_SRV srv;
    NVMEOF_TCP_SRV q1, q2;          /* one of these per connection, which is one per queue */
    NVMEOF_TCP_IC host, ctrl;
    SOCKET c1, c2;

    printf("\n== two connections at once, which is how queues work here ==\n");

    /* In the RDMA transport an I/O queue is a queue pair.  Here it is a TCP connection, so
     * several at once is the normal case and the controller has to hold them apart.  The
     * previous version of this test copied the server struct before accepting, which mixed
     * the two connections up; accepting both first and then working on them separately is
     * both simpler and closer to how the real target will hold its queues. */
    ok(srvStart(&srv, 0) == 0, "controller listens");
    if (srv.listenSock == INVALID_SOCKET) { return; }

    c1 = hostConnect(srv.port);
    ok(c1 != INVALID_SOCKET, "first host connects");
    memset(&q1, 0, sizeof(q1));
    q1.listenSock = srv.listenSock;
    q1.state.state = NVMEOF_TCP_ST_FRESH;
    q1.conn = accept(srv.listenSock, NULL, NULL);
    ok(q1.conn != INVALID_SOCKET, "controller accepts the first");
    setRecvTimeout(q1.conn, 5000);

    c2 = hostConnect(srv.port);
    ok(c2 != INVALID_SOCKET, "second host connects (a second queue)");
    memset(&q2, 0, sizeof(q2));
    q2.listenSock = srv.listenSock;
    q2.state.state = NVMEOF_TCP_ST_FRESH;
    q2.conn = accept(srv.listenSock, NULL, NULL);
    ok(q2.conn != INVALID_SOCKET, "controller accepts the second");
    setRecvTimeout(q2.conn, 5000);

    /* The two connections must be distinct sockets.  If they are not, everything below
     * would pass while only one queue was really being exercised. */
    ok(q1.conn != q2.conn, "the two connections are separate sockets");
    ok(q1.conn != c1 && q2.conn != c2, "client and controller ends are different handles");

    memset(&host, 0, sizeof(host));
    host.pfv = 0; host.cpda = 4; host.maxr2t = 0;
    ok(hostSendIcReq(c1, &host) == 0, "first ICReq sent");
    ok(hostSendIcReq(c2, &host) == 0, "second ICReq sent");

    ok(srvHandshake(&q1) == 0, "first connection handshakes");
    ok(srvHandshake(&q2) == 0, "second connection handshakes");
    ok(q1.icreqSeen == 1 && q2.icreqSeen == 1, "each connection saw exactly one ICReq");
    ok(q1.state.state == NVMEOF_TCP_ST_IC_DONE && q2.state.state == NVMEOF_TCP_ST_IC_DONE,
       "both connections reached ic-done independently");

    ok(hostReadIcResp(c1, &ctrl) == 0, "first ICResp read");
    ok(hostReadIcResp(c2, &ctrl) == 0, "second ICResp read");

    closesocket(c1); closesocket(c2);
    closesocket(q1.conn); closesocket(q2.conn);
    srvStop(&srv);
}

int main(void)
{
    WSADATA wsa;
    int rc;

    printf("NVMe/TCP connection handshake self-test\n");
    printf("=======================================\n");

    rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    /* Unbuffered: when a test can block, a buffered stdout turns a hang into a
     * silent hang, and the first thing anyone needs to know is how far it got. */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (rc != 0) {
        printf("WSAStartup failed: %d\n", rc);
        return 2;
    }

    test_handshake();
    test_bad_first_pdu();
    test_two_connections();

    printf("\n=======================================\n");
    printf("pass %d  fail %d\n", g_pass, g_fail);

    WSACleanup();
    return g_fail == 0 ? 0 : 1;
}
