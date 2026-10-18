/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * nvmeof_tcp.c - NVMe/TCP, both ends, user mode.
 *
 * ROADMAP E1a steps 3 (B) and 4 (C) together, as one tool with two modes:
 *
 *   nvmeof_tcp.exe -target <ip> <port> [-nsfile <path>] [-blocks <n>] [-serve]
 *   nvmeof_tcp.exe -initiator <ip> <port> [-blocks <n>] [-queues <n>] [-write <lba>]
 *
 * Why one file: the target and the initiator have to agree about every byte, and keeping
 * them next to each other makes a disagreement obvious instead of something to discover in
 * a bugcheck.  Both run on 127.0.0.1 with nothing else present, which is the whole point -
 * no RDMA stack, no NetworkDirect provider, no kernel driver, nothing that can take the
 * machine down.
 *
 * What the transport does, stated once because it is the design difference from RDMA
 * (DESIGN.md L99 makes the same point from the other side): a queue pair carries a queue,
 * but here every I/O queue is a separate TCP connection and the data path is not one-sided
 * memory access.  A command's data either rides inside the capsule or moves as explicit
 * H2CData/C2HData PDUs.  This implementation uses the in-capsule form, which is legal for
 * any transfer that fits in one PDU and is what a controller must support anyway; the
 * PDU-driven form for larger transfers is the next piece of work and needs R2T, which the
 * PDU layer already encodes.
 *
 * The namespace is a file: -nsfile on the target, 512-byte blocks, read and written with
 * aligned I/O.  A RAM namespace is the default when no file is given, so the tool always
 * runs.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "nvmeof_tcp.h"

#define BLOCK_SIZE      512u
#define DEFAULT_BLOCKS  2048u          /* 1 MiB namespace */

/* ------------------------------------------------------------------------------------ */
/* NVMe command set: only the handful this tool needs, written out explicitly.           */
/* ------------------------------------------------------------------------------------ */

#define NVME_ADMIN_FLUSH       0x00u
#define NVME_ADMIN_WRITE       0x01u
#define NVME_ADMIN_READ        0x02u
#define NVME_ADMIN_IDENTIFY    0x06u

#define NVME_CNS_NAMESPACE     0x00u
#define NVME_CNS_CONTROLLER    0x01u

#define NVME_SC_SUCCESS        0x00u
#define NVME_SC_INVALID_OPCODE 0x01u
#define NVME_SC_INVALID_FIELD  0x02u
#define NVME_SC_INVALID_NS     0x0Bu
#define NVME_SC_DATA_XFER_ERR  0x04u

static void sqeBuild(uint8_t* q, uint8_t opcode, uint32_t nsid, uint16_t cid,
                     uint32_t cdw10, uint32_t cdw11, uint32_t cdw12)
{
    memset(q, 0, 64);
    q[0] = opcode;
    q[2] = (uint8_t)(cid & 0xFF);
    q[3] = (uint8_t)((cid >> 8) & 0xFF);
    q[4] = (uint8_t)(nsid & 0xFF);
    q[5] = (uint8_t)((nsid >> 8) & 0xFF);
    q[6] = (uint8_t)((nsid >> 16) & 0xFF);
    q[7] = (uint8_t)((nsid >> 24) & 0xFF);
    nvmeofTcpPut32(q, 40, cdw10);
    nvmeofTcpPut32(q, 44, cdw11);
    nvmeofTcpPut32(q, 48, cdw12);
}
static uint8_t  sqeOpcode(const uint8_t* q) { return q[0]; }
static uint16_t sqeCid(const uint8_t* q)    { return (uint16_t)(q[2] | ((uint16_t)q[3] << 8)); }
static uint32_t sqeNsid(const uint8_t* q)   { return (uint32_t)q[4] | ((uint32_t)q[5] << 8) | ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24); }
static uint32_t sqeCdw10(const uint8_t* q)  { return nvmeofTcpGet32(q, 40); }
static uint32_t sqeCdw12(const uint8_t* q)  { return nvmeofTcpGet32(q, 48); }

static void cqeBuild(uint8_t* c, uint16_t sqhd, uint16_t cid, uint8_t sc)
{
    memset(c, 0, 16);
    c[8] = (uint8_t)(sqhd & 0xFF);
    c[9] = (uint8_t)((sqhd >> 8) & 0xFF);
    nvmeofTcpPut32(c, 12, ((uint32_t)cid) | (1u << 16) | (((uint32_t)sc) << 17));
}
static uint16_t cqeCid(const uint8_t* c)    { return (uint16_t)(nvmeofTcpGet32(c, 12) & 0xFFFF); }
static uint8_t  cqeStatus(const uint8_t* c) { return (uint8_t)((nvmeofTcpGet32(c, 12) >> 17) & 0xFF); }

/* ------------------------------------------------------------------------------------ */
/* Namespace: a file (or RAM), 512-byte blocks.                                          */
/* ------------------------------------------------------------------------------------ */

typedef struct {
    uint8_t* ram;
    FILE*    fp;
    uint32_t blocks;
    CRITICAL_SECTION lock;      /* the target serves several connections at once */
} NS;

static int nsOpen(NS* ns, const char* path, uint32_t blocks)
{
    ns->ram = NULL;
    ns->fp = NULL;
    ns->blocks = blocks;
    InitializeCriticalSection(&ns->lock);

    if (path != NULL && path[0] != 0) {
        ns->fp = fopen(path, "r+b");
        if (ns->fp == NULL) {
            ns->fp = fopen(path, "w+b");        /* create a fresh namespace */
            if (ns->fp == NULL) { return -1; }
            if (blocks > 0) {
                /* Give it its full length immediately so reads past the end are not a
                 * special case: an unwritten block reads as zero, which is what a real
                 * namespace does. */
                if (_fseeki64(ns->fp, (__int64)blocks * BLOCK_SIZE - 1, SEEK_SET) != 0) { return -2; }
                fputc(0, ns->fp);
                fflush(ns->fp);
            }
        } else {
            __int64 sz = _fseeki64(ns->fp, 0, SEEK_END);
            _fseeki64(ns->fp, 0, SEEK_SET);
            if (sz > 0) { ns->blocks = (uint32_t)(sz / BLOCK_SIZE); }
        }
        return 0;
    }
    ns->ram = (uint8_t*)calloc(blocks, BLOCK_SIZE);
    return ns->ram ? 0 : -3;
}

static int nsRead(NS* ns, uint32_t lba, uint32_t nblocks, uint8_t* out)
{
    size_t len = (size_t)nblocks * BLOCK_SIZE;
    if (lba >= ns->blocks || lba + nblocks > ns->blocks) { return -1; }
    EnterCriticalSection(&ns->lock);
    if (ns->fp) {
        _fseeki64(ns->fp, (__int64)lba * BLOCK_SIZE, SEEK_SET);
        if (fread(out, 1, len, ns->fp) != len) { LeaveCriticalSection(&ns->lock); return -2; }
    } else {
        memcpy(out, ns->ram + (size_t)lba * BLOCK_SIZE, len);
    }
    LeaveCriticalSection(&ns->lock);
    return 0;
}

static int nsWrite(NS* ns, uint32_t lba, uint32_t nblocks, const uint8_t* in)
{
    size_t len = (size_t)nblocks * BLOCK_SIZE;
    if (lba >= ns->blocks || lba + nblocks > ns->blocks) { return -1; }
    EnterCriticalSection(&ns->lock);
    if (ns->fp) {
        _fseeki64(ns->fp, (__int64)lba * BLOCK_SIZE, SEEK_SET);
        if (fwrite(in, 1, len, ns->fp) != len) { LeaveCriticalSection(&ns->lock); return -2; }
        fflush(ns->fp);
    } else {
        memcpy(ns->ram + (size_t)lba * BLOCK_SIZE, in, len);
    }
    LeaveCriticalSection(&ns->lock);
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Sockets.                                                                              */
/* ------------------------------------------------------------------------------------ */

static void setRecvTimeout(SOCKET s, int ms)
{
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
}

/* A 64 KiB data PDU plus its 24-byte header is 24 bytes past the default 64 KiB
 * socket window, and the peer then stalls on a zero window for the rest of the
 * command (measured: 64 KiB reads fell to 13 MB/s, 48 KiB held 63 MB/s).
 * 1 MiB each way keeps the window open across several PDUs. */
static void setSocketBuffers(SOCKET s)
{
    int v = 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof(v));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&v, sizeof(v));
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

/* ------------------------------------------------------------------------------------ */
/* Target.                                                                               */
/* ------------------------------------------------------------------------------------ */

typedef struct {

    SOCKET   sock;
    NS*      ns;
    uint32_t qid;
} TARGET_CONN;


/* The namespace is opened once in main and shared by every connection thread.  Declared
 * here, just above the target code, rather than threaded through everything. */
static void* g_targetNs = NULL;

static void fillIdentifyController(uint8_t* d, uint32_t blocks);
static int  targetServeOne(void* conn);

/* Target-side handshake: read ICReq, answer ICResp.  Returns 0 when the connection is
 * ready for capsules. */
static int targetHandshakeSafe(void* p)
{
    SOCKET s = ((struct { SOCKET sock; void* ns; uint32_t qid; }*)p)->sock;
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
static void fillIdentifyController(uint8_t* d, uint32_t blocks)
{
    memset(d, 0, 4096);
    d[0] = 0x36; d[1] = 0x1C;                     /* VID          */
    d[2] = 0x36; d[3] = 0x1C;                     /* SSVID        */
    memcpy(d + 4,  "NDTCP000000000000001", 20);   /* SN, offset 4  */
    memcpy(d + 24, "NVMe-oF/TCP for Windows                   ", 40); /* MN, offset 24 */
    memcpy(d + 64, "1.0.0   ", 8);                /* FR           */
    d[516] = 0x02;                                 /* CNTRLTYPE = I/O controller */
    nvmeofTcpPut32(d, 512, blocks);                /* TNVMCAP low  (namespace size hint) */
}

/* Identify Namespace (CNS = 0) now lives in nvmeof_tcp.h, so the self-test in
 * tcp_identify_selftest.c runs the same function this target runs.  The LBA Format
 * table is at 128 and LBADS at 130; see the comment there. */

/* ------------------------------------------------------------------------------------ */
/* Out-of-capsule data: C2HData (target -> host) and H2CData (host -> target).           */
/*                                                                                      */
/* A capsule PDU carries at most one 4096-byte payload, which is why this target used to */
/* refuse any Read or Write larger than that.  A bigger transfer keeps the command in the */
/* capsule and moves the data in explicit data PDUs: a 24-byte header, the bytes, and    */
/* padding up to the 4-byte capsule alignment advertised in ICResp.  One data PDU now    */
/* carries up to 64 KiB, because every PDU costs the host a receive IRP (see below).     */
/* The ICResp also advertises maxr2t = 0, i.e. write data is host-driven and needs no    */
/* R2T - the same flow the Linux host and this project's own clients use.                */
/* ------------------------------------------------------------------------------------ */
#define NVMEOF_TCP_IO_CHUNK 63488u   /* 62 KiB: a 64 KiB+24 PDU hits a kernel receive boundary */

static int sendC2HData(SOCKET s, uint16_t cid, const uint8_t* data, uint32_t len)
{
    uint32_t off = 0;
    uint8_t zero[4] = { 0, 0, 0, 0 };

    while (off < len) {
        NVMEOF_TCP_DATA d;
        uint8_t hdr[NVMEOF_TCP_DATA_HDR_LEN];
        uint32_t chunk = len - off;
        unsigned pad;

        if (chunk > NVMEOF_TCP_IO_CHUNK) { chunk = NVMEOF_TCP_IO_CHUNK; }
        memset(&d, 0, sizeof(d));
        d.cccid = cid;
        d.ttag = 0;
        d.datao = off;
        d.datal = chunk;
        nvmeofTcpBuildData(hdr, NVMEOF_TCP_C2HDATA,
                           (off + chunk == len) ? NVMEOF_TCP_F_LAST : 0, &d);
        if (sendAll(s, hdr, (int)sizeof(hdr)) != (int)sizeof(hdr)) { return -1; }
        if (sendAll(s, data + off, (int)chunk) != (int)chunk) { return -1; }
        pad = (unsigned)((NVMEOF_TCP_PDU_ALIGN - (chunk % NVMEOF_TCP_PDU_ALIGN)) % NVMEOF_TCP_PDU_ALIGN);
        if (pad != 0 && sendAll(s, zero, (int)pad) != (int)pad) { return -1; }
        off += chunk;
    }
    return 0;
}

/* Read exactly len bytes of H2CData for this command into buf.  Any malformed PDU means
 * the stream can no longer be trusted, so the caller closes the connection. */
static int recvH2CData(SOCKET s, uint16_t cid, uint8_t* buf, uint32_t len)
{
    uint32_t got = 0;
    uint8_t zero[4];

    while (got < len) {
        NVMEOF_TCP_HDR h;
        NVMEOF_TCP_DATA d;
        uint8_t hdr[NVMEOF_TCP_DATA_HDR_LEN];
        unsigned pad;

        if (recvAll(s, hdr, (int)sizeof(hdr)) != (int)sizeof(hdr)) { return -1; }
        nvmeofTcpReadHdr(hdr, &h);
        if (h.type != NVMEOF_TCP_H2CDATA || h.hlen != NVMEOF_TCP_DATA_HDR_LEN) { return -1; }
        nvmeofTcpReadData(hdr, &d);
        if (d.cccid != cid || d.datal == 0) { return -1; }
        if ((uint64_t)d.datao + (uint64_t)d.datal > (uint64_t)len) { return -1; }
        if (recvAll(s, buf + d.datao, (int)d.datal) != (int)d.datal) { return -1; }
        pad = (unsigned)((NVMEOF_TCP_PDU_ALIGN - (d.datal % NVMEOF_TCP_PDU_ALIGN)) % NVMEOF_TCP_PDU_ALIGN);
        if (pad != 0 && recvAll(s, zero, (int)pad) != (int)pad) { return -1; }
        got += d.datal;
    }
    return 0;
}

/* Serve one capsule.  Returns 0 to keep the connection, -1 to close it. */
static int targetServeOne(TARGET_CONN* c)
{
    uint8_t hdr[8], sqe[64], cqe[16], cq[16];
    uint8_t* data = NULL;
    uint8_t* resp = NULL;
    uint8_t* big = NULL;          /* out-of-capsule read data */
    uint32_t bigLen = 0;
    int bigRead = 0;
    unsigned capLen, respLen = 0, dataLen = 0;
    NVMEOF_TCP_HDR h;
    uint16_t cid;
    uint8_t opcode, sc = NVME_SC_SUCCESS;
    uint32_t nsid, cdw10, cdw12;
    int rc = 0;

    if (recvAll(c->sock, hdr, 8) != 8) { return -1; }
    nvmeofTcpReadHdr(hdr, &h);
    if (h.type == NVMEOF_TCP_H2CTERMREQ) { return -1; }
    if (h.type != NVMEOF_TCP_CAPSULECMD || h.hlen != NVMEOF_TCP_CCMD_HDR_LEN) { return -1; }
    if (recvAll(c->sock, sqe, 64) != 64) { return -1; }

    cid = sqeCid(sqe);
    opcode = sqeOpcode(sqe);
    nsid = sqeNsid(sqe);
    cdw10 = sqeCdw10(sqe);
    cdw12 = sqeCdw12(sqe);

    /* In-capsule command data (a Write) follows the SQE. */
    if (h.plen > 0) {
        data = (uint8_t*)malloc(h.plen);
        if (data == NULL) { return -1; }
        if (recvAll(c->sock, data, (int)h.plen) != (int)h.plen) { free(data); return -1; }
    }

    /* Build the response capsule in one buffer: header, completion, then data. */
    capLen = NVMEOF_TCP_CRSP_HDR_LEN + 4096;
    resp = (uint8_t*)malloc(capLen);
    if (resp == NULL) { free(data); return -1; }
    memset(resp, 0, capLen);

    if (opcode == NVME_ADMIN_IDENTIFY) {
        uint8_t cns = (uint8_t)(cdw10 & 0xFF);
        if (cns == NVME_CNS_CONTROLLER) {
            fillIdentifyController(resp + NVMEOF_TCP_CRSP_HDR_LEN, c->ns->blocks);
            dataLen = 4096;
        } else if (cns == NVME_CNS_NAMESPACE) {
            if (nsid == 0 || nsid > 1) { sc = NVME_SC_INVALID_NS; }
            else { nvmeofTcpFillIdentifyNamespace(resp + NVMEOF_TCP_CRSP_HDR_LEN, c->ns->blocks); dataLen = 4096; }
        } else {
            sc = NVME_SC_INVALID_FIELD;
        }
    } else if (opcode == NVME_ADMIN_READ || opcode == NVME_ADMIN_WRITE) {
        uint32_t lba = cdw10;
        uint32_t nlb = (cdw12 & 0xFFFF) + 1;      /* NLB is zero-based */
        unsigned bytes = nlb * BLOCK_SIZE;

        if (nsid == 0 || nsid > 1) {
            sc = NVME_SC_INVALID_NS;
        } else if (opcode == NVME_ADMIN_READ) {
            if (bytes > 4096) {
                /* Larger than one capsule payload: the data goes out as C2HData PDUs
                 * after a capsule that carries only the completion. */
                big = (uint8_t*)malloc(bytes);
                if (big == NULL) {
                    sc = NVME_SC_DATA_XFER_ERR;
                } else if (nsRead(c->ns, lba, nlb, big) != 0) {
                    sc = NVME_SC_DATA_XFER_ERR;
                } else {
                    bigLen = bytes;
                    bigRead = 1;
                }
            } else if (nsRead(c->ns, lba, nlb, resp + NVMEOF_TCP_CRSP_HDR_LEN) != 0) {
                sc = NVME_SC_DATA_XFER_ERR;
            } else {
                dataLen = bytes;
            }
        } else {
            if (bytes > 4096) {
                /* Host-driven write: the capsule carried no data, so read the H2CData
                 * PDUs that follow it.  A malformed one desynchronises the stream, so
                 * the connection is closed instead of guessing. */
                uint8_t* w = (uint8_t*)malloc(bytes);
                if (w == NULL) {
                    sc = NVME_SC_DATA_XFER_ERR;
                } else {
                    if (h.plen > 0) {
                        memcpy(w, data, (h.plen < bytes) ? h.plen : bytes);
                    }
                    if (recvH2CData(c->sock, cid, w, bytes) != 0) {
                        free(w);
                        free(data);
                        free(resp);
                        return -1;
                    }
                    if (nsWrite(c->ns, lba, nlb, w) != 0) {
                        sc = NVME_SC_DATA_XFER_ERR;
                    }
                    free(w);
                }
            } else if (h.plen < bytes) {
                sc = NVME_SC_DATA_XFER_ERR;
            } else if (nsWrite(c->ns, lba, nlb, data) != 0) {
                sc = NVME_SC_DATA_XFER_ERR;
            } else {
                dataLen = 0;
            }
        }
    } else if (opcode == NVME_ADMIN_FLUSH) {
        dataLen = 0;
    } else {
        sc = NVME_SC_INVALID_OPCODE;
    }

    /* The completion must be filled for EVERY command.  Only some branches called
     * cqeBuild, so FLUSH and any similar path shipped uninitialised stack as the
     * 16-byte CQE while the capsule SUCCESS flag was right.  A user-mode client that
     * trusts the flag never notices; a real host reads that field.  Found by the
     * iSCSI bridge being driven by a genuine Windows initiator. */
    cqeBuild(cqe, 0, cid, sc);
    respLen = nvmeofTcpBuildCapsuleResp(resp, cqe, sc == NVME_SC_SUCCESS ? 1 : 0,
                                        dataLen, NVMEOF_TCP_CRSP_HDR_LEN);
    (void)cq;
    /* Data PDUs first, completion last.  A host is allowed to stop reading when it
     * sees the CapsuleResp; sending it first leaves the C2HData train in the socket
     * and desynchronises the host's next command.  This transport did exactly that
     * until 2026-10-18, and it corrupted Z: through the kernel miniport. */
    if (bigRead) {
        if (sendC2HData(c->sock, cid, big, bigLen) != 0) {
            rc = -1;                 /* the host went away mid-transfer: close */
        }
    }
    if (rc == 0 && sendAll(c->sock, resp, (int)(respLen + dataLen)) != (int)(respLen + dataLen)) {
        rc = -1;
    }

    free(big);
    free(data);
    free(resp);
    return rc;
}

static DWORD WINAPI targetConnThread(LPVOID param)
{
    TARGET_CONN* c = (TARGET_CONN*)param;
    int rc = 0;
    setRecvTimeout(c->sock, 60000);
    setSocketBuffers(c->sock);

    rc = targetHandshakeSafe(c);
    while (rc == 0) {
        rc = targetServeOne(c);
    }
    closesocket(c->sock);
    free(c);
    return 0;
}

static int targetRun(const char* ip, uint16_t port, const char* nsfile, uint32_t blocks, int serveForever)
{
    SOCKET l;
    struct sockaddr_in a;
    int one = 1;
    unsigned served = 0;

    l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) { printf("socket failed\n"); return 1; }
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { printf("bad address %s\n", ip); return 1; }
    if (bind(l, (struct sockaddr*)&a, sizeof(a)) != 0) { printf("bind %s:%u failed (%d)\n", ip, port, WSAGetLastError()); return 1; }
    if (listen(l, 8) != 0) { printf("listen failed\n"); return 1; }

    printf("[listener armed] %s:%u is accepting connections\n", ip, port);
    setvbuf(stdout, NULL, _IONBF, 0);

    for (;;) {
        SOCKET s = accept(l, NULL, NULL);
        TARGET_CONN* c;
        if (s == INVALID_SOCKET) { break; }
        c = (TARGET_CONN*)calloc(1, sizeof(*c));
        c->sock = s;
        c->ns = (NS*)g_targetNs;
        c->qid = served++;
        printf("[conn %u] accepted\n", c->qid);
        {
            HANDLE t = CreateThread(NULL, 0, targetConnThread, c, 0, NULL);
            if (t) { CloseHandle(t); }
        }
        if (!serveForever && served >= 1) {
            /* One connection is enough for a self-test; keep serving anyway when asked. */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Initiator.                                                                            */
/* ------------------------------------------------------------------------------------ */

typedef struct {
    SOCKET   sock;
    uint16_t nextCid;
} HOST_CONN;

static int hostOpen(HOST_CONN* h, const char* ip, uint16_t port)
{
    struct sockaddr_in a;
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_IC ic;
    NVMEOF_TCP_HDR hdr;

    h->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (h->sock == INVALID_SOCKET) { return -1; }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { return -2; }
    if (connect(h->sock, (struct sockaddr*)&a, sizeof(a)) != 0) {
        printf("  connect %s:%u failed (%d)\n", ip, port, WSAGetLastError());
        return -3;
    }
    setRecvTimeout(h->sock, 15000);
    setSocketBuffers(h->sock);
    h->nextCid = 1;

    memset(&ic, 0, sizeof(ic));
    ic.pfv = 0; ic.cpda = 4; ic.maxr2t = 0;
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);
    if (sendAll(h->sock, buf, NVMEOF_TCP_ICREQ_LEN) != (int)NVMEOF_TCP_ICREQ_LEN) { return -4; }
    if (recvAll(h->sock, buf, 8) != 8) { return -5; }
    nvmeofTcpReadHdr(buf, &hdr);
    if (hdr.type != NVMEOF_TCP_ICRESP) { return -6; }
    if (recvAll(h->sock, buf + 8, NVMEOF_TCP_IC_BODY_LEN) != (int)NVMEOF_TCP_IC_BODY_LEN) { return -7; }
    printf("  handshake ok (%s)\n", nvmeofTcpTypeName(hdr.type));
    return 0;
}

/* One command.  inData may be NULL; outData may be NULL. */
static int hostCmd(HOST_CONN* h, uint8_t opcode, uint32_t nsid, uint32_t cdw10, uint32_t cdw12,
                   const uint8_t* inData, unsigned inLen, uint8_t* outData, unsigned* outLen,
                   uint8_t* statusOut)
{
    uint8_t sqe[64], cap[NVMEOF_TCP_CCMD_HDR_LEN], hdr[8], cqe[16];
    uint8_t* big;
    unsigned n, total;
    NVMEOF_TCP_HDR hh;
    uint16_t cid = h->nextCid++;

    sqeBuild(sqe, opcode, nsid, cid, cdw10, 0, cdw12);
    n = nvmeofTcpBuildCapsuleCmd(cap, sqe, inLen, NVMEOF_TCP_CCMD_HDR_LEN);
    total = n + inLen;
    big = (uint8_t*)malloc(total);
    if (big == NULL) { return -1; }
    memcpy(big, cap, n);
    if (inLen > 0 && inData) { memcpy(big + n, inData, inLen); }
    if (sendAll(h->sock, big, (int)total) != (int)total) { free(big); return -2; }
    free(big);

    if (recvAll(h->sock, hdr, 8) != 8) { return -3; }
    nvmeofTcpReadHdr(hdr, &hh);
    if (hh.type != NVMEOF_TCP_CAPSULERESP) { return -4; }
    if (recvAll(h->sock, cqe, 16) != 16) { return -5; }
    if (statusOut) { *statusOut = cqeStatus(cqe); }
    if (outLen) { *outLen = hh.plen; }
    if (hh.plen > 0) {
        if (outData == NULL) {
            uint8_t drain[512];
            if (recvAll(h->sock, drain, (int)(hh.plen < 512 ? hh.plen : 512)) < 0) { return -6; }
        } else if (recvAll(h->sock, outData, (int)hh.plen) != (int)hh.plen) {
            return -7;
        }
    }
    return 0;
}

static int initiatorRun(const char* ip, uint16_t port, uint32_t blocks, int queues, long writeLba)
{
    HOST_CONN q[8];
    uint8_t buf[4096];
    uint8_t status = 0xFF;
    unsigned len = 0;
    int i, failures = 0;
    char sn[21], mn[41];

    if (queues < 1) { queues = 1; }
    if (queues > 8) { queues = 8; }

    printf("=== NVMe/TCP initiator -> %s:%u (%d Queue%s)\n", ip, port, queues, queues == 1 ? "" : "s");
    for (i = 0; i < queues; i++) {
        printf("-- queue %d\n", i);
        if (hostOpen(&q[i], ip, port) != 0) { printf("  [FAIL] queue %d did not open\n", i); return 1; }
    }

    /* Identify Controller on queue 0. */
    memset(buf, 0, sizeof(buf));
    if (hostCmd(&q[0], NVME_ADMIN_IDENTIFY, 0, NVME_CNS_CONTROLLER, 0, NULL, 0, buf, &len, &status) != 0) {
        printf("  [FAIL] Identify did not complete\n"); return 1;
    }
    memcpy(sn, buf + 4, 20);  sn[20] = 0;
    memcpy(mn, buf + 24, 40); mn[40] = 0;
    printf("  Identify Controller: status=0x%02X len=%u\n", status, len);
    printf("    VID=0x%04X  SN=%.20s\n", (unsigned)(buf[0] | ((unsigned)buf[1] << 8)), sn);
    printf("    MN=%.40s\n", mn);
    if (status != NVME_SC_SUCCESS || len != 4096) { printf("  [FAIL] Identify returned bad data\n"); failures++; }

    /* Identify Namespace on queue 0: the size the target reports. */
    memset(buf, 0, sizeof(buf));
    if (hostCmd(&q[0], NVME_ADMIN_IDENTIFY, 1, NVME_CNS_NAMESPACE, 0, NULL, 0, buf, &len, &status) == 0) {
        uint64_t nsze = 0;
        memcpy(&nsze, buf, 8);
        printf("  Identify Namespace 1: status=0x%02X nsze=%llu blocks (%llu MiB)\n",
               status, (unsigned long long)nsze, (unsigned long long)(nsze * 512 / (1024 * 1024)));
        if (nsze == 0) { printf("  [FAIL] namespace reports zero size\n"); failures++; }
    }

    /* Read a block, modify nothing, then write a pattern and read it back.  The pattern is
     * chosen so that a single swapped byte shows up. */
    if (writeLba >= 0 && (uint32_t)writeLba < blocks) {
        uint8_t pattern[512], back[512];
        for (i = 0; i < 512; i++) { pattern[i] = (uint8_t)((writeLba * 31 + i * 7) & 0xFF); }

        printf("  Write LBA %ld (in-capsule, 512 bytes)\n", writeLba);
        if (hostCmd(&q[0], NVME_ADMIN_WRITE, 1, (uint32_t)writeLba, 0, pattern, 512, NULL, &len, &status) != 0 || status != 0) {
            printf("  [FAIL] write failed (status=0x%02X)\n", status); failures++;
        } else {
            printf("    -> status=0x%02X\n", status);
            memset(back, 0, sizeof(back));
            if (hostCmd(&q[0], NVME_ADMIN_READ, 1, (uint32_t)writeLba, 0, NULL, 0, back, &len, &status) != 0 || status != 0) {
                printf("  [FAIL] read-back failed (status=0x%02X)\n", status); failures++;
            } else if (memcmp(pattern, back, 512) != 0) {
                printf("  [FAIL] read-back differs from what was written\n"); failures++;
            } else {
                printf("    -> read back 512 bytes, byte-identical [PASS]\n");
            }
        }
    }

    /* Every queue issues a Read of its own block, which is what makes this multi-queue
     * rather than one queue used several times. */
    for (i = 0; i < queues; i++) {
        uint32_t lba = (uint32_t)((i * 17u) % (blocks ? blocks : 1u));
        memset(buf, 0, sizeof(buf));
        if (hostCmd(&q[i], NVME_ADMIN_READ, 1, lba, 0, NULL, 0, buf, &len, &status) != 0 || status != 0) {
            printf("  [FAIL] queue %d read LBA %u failed (status=0x%02X)\n", i, lba, status); failures++;
        } else {
            uint32_t sum = 0;
            for (int k = 0; k < 512; k++) { sum += buf[k]; }
            printf("  queue %d: read LBA %u, 512 bytes, checksum %08X\n", i, lba, sum);
        }
    }

    for (i = 0; i < queues; i++) { closesocket(q[i].sock); }

    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    WSADATA wsa;
    const char* mode;
    const char* ip;
    uint16_t port;
    const char* nsfile = NULL;
    uint32_t blocks = DEFAULT_BLOCKS;
    int queues = 1;
    long writeLba = -1;
    int serve = 0;
    int i;

    if (argc < 4) {
        printf("usage:\n");
        printf("  %s -target    <ip> <port> [-nsfile <path>] [-blocks <n>] [-serve]\n", argv[0]);
        printf("  %s -initiator <ip> <port> [-blocks <n>] [-queues <n>] [-write <lba>]\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    ip = argv[2];
    port = (uint16_t)atoi(argv[3]);

    for (i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-nsfile") == 0 && i + 1 < argc) { nsfile = argv[++i]; }
        else if (strcmp(argv[i], "-blocks") == 0 && i + 1 < argc) { blocks = (uint32_t)atoi(argv[++i]); }
        else if (strcmp(argv[i], "-queues") == 0 && i + 1 < argc) { queues = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-write") == 0 && i + 1 < argc) { writeLba = atol(argv[++i]); }
        else if (strcmp(argv[i], "-serve") == 0) { serve = 1; }
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup failed\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);

    if (strcmp(mode, "-target") == 0) {
        static NS ns;                                   /* one namespace, shared by connections */
        if (nsOpen(&ns, nsfile, blocks) != 0) { printf("cannot open namespace\n"); return 1; }
        printf("=== NVMe/TCP target on %s:%u\n", ip, port);
        printf("    namespace: %s, %u blocks x 512 = %u KiB\n",
               nsfile ? nsfile : "(RAM)", ns.blocks, (unsigned)(ns.blocks * 512 / 1024));
        g_targetNs = &ns;
        return targetRun(ip, port, nsfile, blocks, serve);
    }
    if (strcmp(mode, "-initiator") == 0) {
        return initiatorRun(ip, port, blocks, queues, writeLba);
    }
    printf("unknown mode %s\n", mode);
    return 2;
}


/* ---- out-of-capsule data: C2HData and H2CData ------------------------------------- */
/*
 * Send a payload as a series of data PDUs.  The receiver gets a 24-byte header and then the
 * data, with LAST set on the final PDU - that is how it knows the transfer is complete
 * without counting bytes itself.  Each PDU is padded up to the capsule alignment, and both
 * sides have to agree about the padding or every following PDU in the stream is misaligned.
 *
 * This is the part of NVMe/TCP that has no RDMA equivalent at all.  There is no RDMA Write
 * and no remote memory to write into; the sender paces the transfer in chunks and the
 * receiver follows headers.
 */
static int sendDataPdus(SOCKET s, uint8_t type, uint16_t cccid, uint16_t ttag,
                        const uint8_t* data, unsigned len, unsigned maxPerPdu, unsigned align)
{
    unsigned off = 0;
    if (maxPerPdu == 0) { maxPerPdu = 65536; }
    while (off < len) {
        unsigned chunk = len - off;
        unsigned total;
        int last;
        uint8_t hdr[NVMEOF_TCP_DATA_HDR_LEN];
        uint8_t* buf;
        NVMEOF_TCP_DATA d;
        if (chunk > maxPerPdu) { chunk = maxPerPdu; }
        last = (off + chunk >= len);
        d.cccid = cccid; d.ttag = ttag; d.datao = off; d.datal = chunk;
        nvmeofTcpBuildData(hdr, type, last ? NVMEOF_TCP_F_LAST : 0, &d);
        total = nvmeofTcpPduTotal(NVMEOF_TCP_DATA_HDR_LEN, chunk, align);
        buf = (uint8_t*)calloc(1, total);
        if (buf == NULL) { return -1; }
        memcpy(buf, hdr, NVMEOF_TCP_DATA_HDR_LEN);
        memcpy(buf + NVMEOF_TCP_DATA_HDR_LEN, data + off, chunk);
        if (sendAll(s, buf, (int)total) != (int)total) { free(buf); return -2; }
        free(buf);
        off += chunk;
    }
    return 0;
}

/* Receive data PDUs until LAST arrives.  Returns the number of bytes accumulated, or -1. */
static int recvDataPdus(SOCKET s, uint8_t* out, unsigned cap, unsigned align, unsigned* gotOut)
{
    unsigned got = 0;
    for (;;) {
        uint8_t hdr[NVMEOF_TCP_DATA_HDR_LEN];
        NVMEOF_TCP_HDR h;
        NVMEOF_TCP_DATA d;
        unsigned pad;
        if (recvAll(s, hdr, NVMEOF_TCP_DATA_HDR_LEN) != (int)NVMEOF_TCP_DATA_HDR_LEN) { return -1; }
        nvmeofTcpReadHdr(hdr, &h);
        if (h.type != NVMEOF_TCP_C2HDATA && h.type != NVMEOF_TCP_H2CDATA) { return -2; }
        nvmeofTcpReadData(hdr, &d);
        if (d.datal > cap - got) { return -3; }
        if (d.datal > 0) {
            if (recvAll(s, out + got, (int)d.datal) != (int)d.datal) { return -4; }
        }
        pad = nvmeofTcpPduTotal(NVMEOF_TCP_DATA_HDR_LEN, d.datal, align)
            - NVMEOF_TCP_DATA_HDR_LEN - d.datal;
        if (pad > 0) {
            uint8_t skip[8];
            if (pad > sizeof(skip)) { return -5; }
            if (recvAll(s, skip, (int)pad) != (int)pad) { return -6; }
        }
        got += d.datal;
        if ((h.flags & NVMEOF_TCP_F_LAST) != 0) { break; }
    }
    *gotOut = got;
    return (int)got;
}


