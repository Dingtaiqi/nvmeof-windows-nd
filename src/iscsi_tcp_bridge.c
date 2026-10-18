/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * iscsi_tcp_bridge.c - a minimal iSCSI target whose SCSI READ(10)/WRITE(10) become NVMe
 * Read/Write commands carried over this project's own NVMe/TCP implementation.
 *
 * WHY THIS EXISTS.  Windows has a kernel-mode iSCSI initiator in every SKU and no
 * user-mode path into the volume stack, so the only way to make this project's NVMe/TCP
 * code produce a real disk on this machine is to speak iSCSI at one end and NVMe/TCP at
 * the other.  This file is that other end:
 *
 *   Windows iSCSI initiator  --iSCSI/TCP-->  this program  --NVMe/TCP-->  nvmeof_tcp.exe
 *        (127.0.0.1:3260)                (our own iSCSI PDU layer)      -target (127.0.0.1:4420)
 *
 * The bytes therefore travel over the transport written by hand in src/nvmeof_tcp.h: the
 * ICReq/ICResp handshake, the command capsules and the in-capsule data path are that code,
 * unmodified.  This file only *uses* it; nothing existing in the project was changed.
 *
 * MODES
 *   iscsi_tcp_bridge.exe -listen <ip> <port> -target <ip> <port> -lun <lba0> <nblocks>
 *   iscsi_tcp_bridge.exe -selftest
 *
 *   -listen   the iSCSI portal Windows connects to
 *   -target   the NVMe/TCP target (nvmeof_tcp.exe -target ... -serve)
 *   -lun      maps SCSI LBA 0 .. nblocks-1 onto NVMe LBA lba0 .. lba0+nblocks-1
 *
 * THE 4096-BYTE LIMIT, STATED UP FRONT.  The NVMe/TCP path in nvmeof_tcp.c carries command
 * data *inside* the capsule and refuses anything over 4096 bytes (a larger transfer needs
 * C2HData/R2T, which that file documents as not wired yet).  Windows, on the other hand,
 * issues 64 KiB SCSI transfers.  So every SCSI READ(10)/WRITE(10) here is split into NVMe
 * commands of at most 4096 bytes = 8 blocks and reassembled on the way back.  A 64 KiB
 * READ(10) is therefore sixteen NVMe Reads over the one TCP connection, not one.  That is a
 * throughput compromise, not a correctness one, and it is the price of staying inside the
 * proven path instead of extending it.
 *
 * WHAT IS IMPLEMENTED (and nothing else):
 *   Login        three stages, no authentication, no digests, no CHAP.  Keys that are not
 *                negotiated are simply not answered (RFC 7143 section 6.2 lets the
 *                initiator's own value stand), except AuthMethod=None, TargetAlias and
 *                TargetPortalGroupTag, which are target-only keys and must be sent.
 *   Text         SendTargets=All -> one target name, one address.
 *   SCSI         INQUIRY (+VPD 0x00/0x80/0x83), TEST UNIT READY, REQUEST SENSE,
 *                READ CAPACITY(10), READ(10), WRITE(10), SYNCHRONIZE CACHE(10),
 *                REPORT LUNS, MODE SENSE(6)/(10), START STOP UNIT.
 *                WRITE SAME(10) is logged and refused with CHECK CONDITION / ILLEGAL
 *                REQUEST (ASC 0x20); so is any CDB not in that list.
 *   Data         Data-In (0x25) and Data-Out (0x05) PDUs, 4-byte data-segment padding.
 *                Written data arrives unsolicited (InitialR2T=No) in a first burst of at
 *                most FirstBurstLength; a write larger than one burst gets an R2T for the
 *                remainder, which Windows' 64 KiB write CDBs never need.
 *   NOP          NOP-Out echoed as NOP-In.
 *   Logout       answered, connection closed.
 *
 * PDU FRAMING, the part that decides whether anything works at all: 48-byte basic header,
 * then optional AHS, then the data segment, then 0..3 bytes of padding.  The data segment is
 * padded to a multiple of four and DataSegmentLength does NOT count the padding, so a
 * 13-byte payload is 48 + 13 + 3 bytes on the wire.  Get that wrong in either direction and
 * the initiator hangs or reports sense errors, which is why -selftest checks it with a
 * deliberately non-multiple-of-4 payload and further PDUs queued behind it.
 *
 * FIELD OFFSETS.  Not from memory: from RFC 7143 section 11, from a capture of the real
 * Windows initiator checked into this repository (src/iscsi_capture.txt), and cross-checked
 * against the iSCSI target already in this tree (src/nvmeof_iscsi.h) which was driven by
 * that same initiator.  The layout that matters, and the one place it is easy to be wrong:
 *
 *   +0  opcode (low 6 bits) | 0x40 immediate | 0x80 F
 *   +1  opcode-specific flags: login T|CSG|NSG, Data-In F, SCSI Command F|R, Text F
 *   +2  opcode-specific (login: version-max)
 *   +3  opcode-specific (login: version-active)
 *   +4  TotalAHSLength, in 4-byte words
 *   +5  24-bit DataSegmentLength, big-endian          <- NOT +2, and NOT two bytes
 *   +8  8-byte LUN (SCSI Command) / ISID+TSIH (Login)
 *   +16 InitiatorTaskTag
 *   +20 ExpectedDataTransferLength (Cmd) / TTT (Data-In, R2T) / CID (Login)
 *   +24 CmdSN (Command) / StatSN (Response) / ExpStatSN (Data-Out)
 *   +28 ExpStatSN (Command) / ExpCmdSN (Response)
 *   +32 MaxCmdSN (Response) / CDB (Command) / DataSN (Data-Out)
 *   +36 DataSN (Data-In) / Buffer Offset (Data-Out)
 *   +40 Buffer Offset (Data-In) / residual (SCSI Response)
 *
 *   A SCSI Response carries the status in byte 3 and the response code in byte 2 - the other
 *   way round produces a disk that answers every command and is never usable.
 *
 * ASCII only, user mode only, loopback only.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "nvmeof_tcp.h"

/* ==================================================================================== */
/* Constants                                                                            */
/* ==================================================================================== */

#define BLOCK_SIZE          512u
#define NVME_CHUNK_BLOCKS   128u                        /* 64 KiB: one ISCSI_MAX_DSL burst  */
#define NVME_CHUNK_BYTES    (NVME_CHUNK_BLOCKS * BLOCK_SIZE)
#define NVME_CAPSULE_DATA_MAX 4096u                     /* what fits inside one capsule PDU  */
#define NVME_PDU_DATA_MAX    65536u                     /* one out-of-capsule data PDU       */
#define MAX_XFER            (1024u * 1024u)             /* largest SCSI transfer accepted */
#define ISCSI_MAX_DSL       65536u                      /* our MaxRecvDataSegmentLength   */
#define CMD_WINDOW          8u                          /* MaxCmdSN - ExpCmdSN + 1        */
#define TARGET_IQN          "iqn.2026-10.local.dingtaiqi:nvmeof-tcp"
#define TARGET_ALIAS        "nvmeof-tcp bridge"
#define TARGET_PORTAL_TAG   "1"
#define MAX_DEFER           8                           /* PDUs held while a burst runs   */

/* iSCSI opcodes (RFC 7143 section 11). */
#define ISCSI_OP_NOP_OUT        0x00u
#define ISCSI_OP_SCSI_CMD       0x01u
#define ISCSI_OP_TASK_MGMT      0x02u
#define ISCSI_OP_LOGIN_REQ      0x03u
#define ISCSI_OP_TEXT_REQ       0x04u
#define ISCSI_OP_DATA_OUT       0x05u
#define ISCSI_OP_LOGOUT_REQ     0x06u
#define ISCSI_OP_SNACK          0x10u
#define ISCSI_OP_NOP_IN         0x20u
#define ISCSI_OP_SCSI_RSP       0x21u
#define ISCSI_OP_TASK_MGMT_RSP  0x22u
#define ISCSI_OP_LOGIN_RSP      0x23u
#define ISCSI_OP_TEXT_RSP       0x24u
#define ISCSI_OP_DATA_IN        0x25u
#define ISCSI_OP_LOGOUT_RSP     0x26u
#define ISCSI_OP_R2T            0x31u
#define ISCSI_OP_REJECT         0x3Fu

/* Login stages.  There is no stage 2. */
#define ISCSI_STAGE_SECURITY       0
#define ISCSI_STAGE_OPERATIONAL    1
#define ISCSI_STAGE_FULL_FEATURE   3

/* SCSI status, sense keys and additional sense codes. */
#define SCSI_STATUS_GOOD           0x00u
#define SCSI_STATUS_CHECK_COND     0x02u
#define SENSE_NO_SENSE             0x00u
#define SENSE_MEDIUM_ERROR         0x03u
#define SENSE_HARDWARE_ERROR       0x04u
#define SENSE_ILLEGAL_REQUEST      0x05u
#define ASC_INVALID_OPCODE         0x20u
#define ASC_LBA_OUT_OF_RANGE       0x21u
#define ASC_INVALID_FIELD          0x24u
#define ASC_INTERNAL_FAILURE       0x44u

/* NVMe opcodes and status, as the target in nvmeof_tcp.c implements them. */
#define NVME_OP_WRITE              0x01u
#define NVME_OP_READ               0x02u
#define NVME_OP_IDENTIFY           0x06u
#define NVME_NSID                  1u
#define NVME_CNS_NAMESPACE         0x00u
#define NVME_CNS_CONTROLLER        0x01u
#define NVME_SC_SUCCESS            0x00u

/* Codes returned by the PDU layer.  A socket read can mean three different things, and a
 * session that confuses "idle" with "closed" either spins or dies. */
#define IO_OK       0
#define IO_CLOSED  -1
#define IO_IDLE    -2
#define IO_ERROR   -3

/* ==================================================================================== */
/* Tiny helpers                                                                         */
/* ==================================================================================== */

/* Every multi-byte field on an iSCSI wire is network order.  Reading one little-endian
 * yields a plausible-looking number that fails far from its cause. */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t rd24(const uint8_t* p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
static uint32_t rd32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr24(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}
static void wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* The iSCSI data segment is padded to a multiple of four bytes and the padding is not
 * counted in DataSegmentLength.  One function, so the self-test can pin all four residues
 * rather than the one that happened to be seen. */
static uint32_t pad4(uint32_t len) { return (4u - (len & 3u)) & 3u; }

static unsigned nvmeChunkCount(uint32_t nblocks)
{
    return (unsigned)((nblocks + NVME_CHUNK_BLOCKS - 1u) / NVME_CHUNK_BLOCKS);
}

static int g_pass = 0;
static int g_fail = 0;

/* -trace: every PDU in and out, with the fields that matter when an initiator drops a
 * connection for a reason it does not write down anywhere.  Off by default because a
 * formatted disk generates thousands of PDUs. */
static int g_trace = 0;

/* printf with every line flushed: when stdout is a redirected file that only ever grows,
 * an unflushed line is a mystery, and a buffered log has cost this project real time. */
static void say(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void trace(const char* fmt, ...)
{
    va_list ap;
    if (!g_trace) { return; }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void ok(int cond, const char* what)
{
    if (cond) { g_pass++; printf("  [PASS] %s\n", what); }
    else      { g_fail++; printf("  [FAIL] %s\n", what); }
    fflush(stdout);
}

/* ==================================================================================== */
/* Sockets                                                                              */
/* ==================================================================================== */

static void setRecvTimeout(SOCKET s, int ms)
{
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
}
static void setSendTimeout(SOCKET s, int ms)
{
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
}

/* 1 MiB each way: 64 KiB data PDUs over the default 64 KiB window stall on a
 * zero window for the rest of every command (13 MB/s vs 63 MB/s measured). */
static void setSocketBuffers(SOCKET s)
{
    int v = 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof(v));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&v, sizeof(v));
}

/* 1 = all n bytes, 0 = peer closed, -1 = timed out before the first byte, -2 = error (or
 * timeout) in the middle of a PDU.  The distinction is the difference between a disk that is
 * merely idle and one whose initiator went away. */
static int recvAll(SOCKET s, uint8_t* b, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(s, (char*)b + got, n - got, 0);
        if (r == 0) { return got == 0 ? 0 : -2; }
        if (r < 0) {
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) { return got == 0 ? -1 : -2; }
            return got == 0 ? 0 : -2;
        }
        got += r;
    }
    return 1;
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

static SOCKET listenLoopback(uint16_t port, uint16_t* got)
{
    SOCKET l;
    struct sockaddr_in a;
    int one = 1, len;
    l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) { return INVALID_SOCKET; }
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(l, (struct sockaddr*)&a, sizeof(a)) != 0) { closesocket(l); return INVALID_SOCKET; }
    if (listen(l, 8) != 0) { closesocket(l); return INVALID_SOCKET; }
    len = (int)sizeof(a);
    if (getsockname(l, (struct sockaddr*)&a, &len) == 0 && got != NULL) { *got = ntohs(a.sin_port); }
    return l;
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
    setRecvTimeout(s, 15000);
    setSendTimeout(s, 15000);
    return s;
}

static uint16_t freePort(void)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    int len = (int)sizeof(a);
    uint16_t p = 0;
    if (s == INVALID_SOCKET) { return 0; }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) == 0 &&
        getsockname(s, (struct sockaddr*)&a, &len) == 0) {
        p = ntohs(a.sin_port);
    }
    closesocket(s);
    return p;
}

/* ==================================================================================== */
/* iSCSI PDU framing: send and receive, padding included                                */
/* ==================================================================================== */

/* Write one PDU: 48-byte BHS, then the data segment, then its padding.  AHS is never
 * emitted (every builder here leaves TotalAHSLength 0). */
static int iscsiSendPdu(SOCKET sock, const uint8_t* bhs, const void* data, uint32_t len)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    uint32_t pad = pad4(len);
    if (sendAll(sock, bhs, 48) != 48) { return -1; }
    if (len != 0) {
        if (sendAll(sock, (const uint8_t*)data, (int)len) != (int)len) { return -1; }
    }
    if (pad != 0) {
        if (sendAll(sock, zero, (int)pad) != (int)pad) { return -1; }
    }
    return 0;
}

/* Read one PDU.  `*data` is a reusable buffer that grows on demand, so the caller keeps it
 * alive for the connection.  AHS bytes are read and dropped: nothing here sends AHS, and a
 * PDU that has one only needs its length honoured so the stream stays aligned. */
static int iscsiRecvPdu(SOCKET sock, uint8_t* bhs, uint8_t** data, uint32_t* cap, uint32_t* len)
{
    int r = recvAll(sock, bhs, 48);
    uint32_t dsl, ahs, pad;

    if (r == 0) { return IO_CLOSED; }
    if (r == -1) { return IO_IDLE; }
    if (r != 1) { return IO_ERROR; }

    dsl = rd24(bhs + 5);
    ahs = (uint32_t)bhs[4] * 4u;
    pad = pad4(dsl);

    if (dsl > MAX_XFER) {
        say("  [pdu] refusing a data segment of %u bytes\n", dsl);
        return IO_ERROR;
    }
    if (ahs != 0) {
        uint8_t scratch[256];
        uint32_t left = ahs;
        while (left != 0) {
            uint32_t take = left < sizeof(scratch) ? left : (uint32_t)sizeof(scratch);
            if (recvAll(sock, scratch, (int)take) != 1) { return IO_ERROR; }
            left -= take;
        }
    }
    if (dsl != 0) {
        if (*data == NULL || *cap < dsl) {
            uint8_t* nb = (uint8_t*)realloc(*data, dsl);
            if (nb == NULL) { return IO_ERROR; }
            *data = nb;
            *cap = dsl;
        }
        if (recvAll(sock, *data, (int)dsl) != 1) { return IO_ERROR; }
    }
    if (pad != 0) {
        uint8_t skip[4];
        if (recvAll(sock, skip, (int)pad) != 1) { return IO_ERROR; }
    }
    *len = dsl;
    return IO_OK;
}

/* ----------------------------------------------------------------------------------- */
/* BHS accessors.  Named after the field, not the offset, because the offsets are the     */
/* thing that goes wrong.                                                                */
/* ----------------------------------------------------------------------------------- */

static uint8_t  bhsOpcode(const uint8_t* p)    { return (uint8_t)(p[0] & 0x3Fu); }
static int      bhsFinal(const uint8_t* p)     { return (p[1] & 0x80u) != 0; }
static uint32_t bhsDataLen(const uint8_t* p)   { return rd24(p + 5); }
static uint32_t bhsITT(const uint8_t* p)       { return rd32(p + 16); }
static uint32_t bhsEDTL(const uint8_t* p)      { return rd32(p + 20); }
static uint32_t bhsTTT(const uint8_t* p)       { return rd32(p + 20); }
static uint32_t bhsCmdSN(const uint8_t* p)     { return rd32(p + 24); }
static uint32_t bhsStatSN(const uint8_t* p)    { return rd32(p + 24); }
static uint32_t bhsExpStatSN(const uint8_t* p) { return rd32(p + 28); }
static uint32_t bhsExpCmdSN(const uint8_t* p)  { return rd32(p + 28); }
static uint32_t bhsMaxCmdSN(const uint8_t* p)  { return rd32(p + 32); }
static uint32_t bhsDataSN(const uint8_t* p)    { return rd32(p + 36); }   /* Data-In  */
static uint32_t bhsBufOff(const uint8_t* p)    { return rd32(p + 40); }   /* Data-In  */
static uint32_t bhsDoDataSN(const uint8_t* p)  { return rd32(p + 32); }   /* Data-Out */
static uint32_t bhsDoBufOff(const uint8_t* p)  { return rd32(p + 36); }   /* Data-Out */
static int      bhsLoginT(const uint8_t* p)    { return (p[1] & 0x80u) != 0; }
static uint8_t  bhsLoginCsg(const uint8_t* p)  { return (uint8_t)((p[1] >> 2) & 0x03u); }
static uint8_t  bhsLoginNsg(const uint8_t* p)  { return (uint8_t)(p[1] & 0x03u); }

/* ----------------------------------------------------------------------------------- */
/* BHS builders.  Each zeroes the header first: every reserved byte on the wire is 0, and */
/* a leftover byte from a previous PDU is the kind of defect that looks like a peer bug.  */
/* ----------------------------------------------------------------------------------- */

static void bhsInit(uint8_t* p, uint8_t opcode, uint8_t flags, uint32_t dsl)
{
    memset(p, 0, 48);
    p[0] = opcode;
    p[1] = flags;
    wr24(p + 5, dsl);              /* TotalAHSLength (byte 4) stays 0 */
}

static void buildLoginRsp(uint8_t* p, const uint8_t* req, uint16_t tsih, uint32_t itt,
                          uint32_t statSn, uint32_t expCmdSn, uint32_t maxCmdSn,
                          uint32_t keyLen)
{
    bhsInit(p, ISCSI_OP_LOGIN_RSP, 0, keyLen);
    /* T / CSG / NSG are mirrored from the request: this target accepts the stage progression
     * the initiator asks for, which is legal and removes all guesswork about which stage
     * Windows expects to land in.  Bits 6..4 are not part of the response's transition. */
    p[1] = (uint8_t)(req[1] & 0x8Fu);
    p[2] = req[2];                 /* version-max, mirrored   */
    p[3] = req[3];                 /* version-active, mirrored */
    memcpy(p + 8, req + 8, 6);     /* ISID echoed: bytes 8-13 are ISID, not a LUN */
    wr16(p + 14, tsih);
    wr32(p + 16, itt);
    wr32(p + 24, statSn);
    wr32(p + 28, expCmdSn);
    wr32(p + 32, maxCmdSn);
    /* bytes 36/37 (status class / detail) stay 0 = success */
}

static void buildTextRsp(uint8_t* p, uint32_t itt, int final, uint32_t statSn,
                         uint32_t expCmdSn, uint32_t maxCmdSn, uint32_t keyLen)
{
    bhsInit(p, ISCSI_OP_TEXT_RSP, (uint8_t)(final ? 0x80u : 0u), keyLen);
    wr32(p + 16, itt);
    wr32(p + 20, 0xFFFFFFFFu);     /* TTT: unsolicited */
    wr32(p + 24, statSn);
    wr32(p + 28, expCmdSn);
    wr32(p + 32, maxCmdSn);
}

static void buildDataIn(uint8_t* p, uint32_t itt, uint32_t statSn, uint32_t expCmdSn,
                        uint32_t maxCmdSn, uint32_t dataSn, uint32_t offset, uint32_t len,
                        int final)
{
    bhsInit(p, ISCSI_OP_DATA_IN, (uint8_t)(final ? 0x80u : 0u), len);
    wr32(p + 16, itt);
    wr32(p + 20, 0xFFFFFFFFu);     /* TTT: unsolicited data-in */
    wr32(p + 24, statSn);          /* StatSN of the SCSI Response that will follow */
    wr32(p + 28, expCmdSn);
    wr32(p + 32, maxCmdSn);
    wr32(p + 36, dataSn);
    wr32(p + 40, offset);
}

static void buildScsiRsp(uint8_t* p, uint32_t itt, uint8_t status, int underflow,
                         uint32_t statSn, uint32_t expCmdSn, uint32_t maxCmdSn,
                         uint32_t expDataSN, uint32_t residual, uint32_t senseLen)
{
    bhsInit(p, ISCSI_OP_SCSI_RSP, (uint8_t)(0x80u | (underflow ? 0x40u : 0u)),
            (uint32_t)(senseLen ? senseLen + 2u : 0u));
    p[2] = 0x00;                   /* response code: command completed at target */
    p[3] = status;                 /* status is byte 3, not byte 2 */
    wr32(p + 16, itt);
    wr32(p + 20, 0xFFFFFFFFu);     /* TTT: unsolicited */
    wr32(p + 24, statSn);
    wr32(p + 28, expCmdSn);
    wr32(p + 32, maxCmdSn);
    wr32(p + 36, expDataSN);
    wr32(p + 40, 0);               /* reserved */
    wr32(p + 44, residual);
}

static void buildNopIn(uint8_t* p, uint32_t itt, uint32_t ttt, uint32_t statSn,
                       uint32_t expCmdSn, uint32_t maxCmdSn)
{
    bhsInit(p, ISCSI_OP_NOP_IN, 0, 0);
    wr32(p + 16, itt);
    wr32(p + 20, ttt);
    wr32(p + 24, statSn);
    wr32(p + 28, expCmdSn);
    wr32(p + 32, maxCmdSn);
}

static void buildLogoutRsp(uint8_t* p, uint32_t itt, uint32_t statSn, uint32_t expCmdSn,
                           uint32_t maxCmdSn)
{
    bhsInit(p, ISCSI_OP_LOGOUT_RSP, 0x80, 0);
    p[2] = 0x00;                   /* response: connection closed successfully */
    wr32(p + 16, itt);
    wr32(p + 24, statSn);
    wr32(p + 28, expCmdSn);
    wr32(p + 32, maxCmdSn);
}

static void buildTaskMgmtRsp(uint8_t* p, uint32_t itt, uint8_t response, uint32_t statSn,
                             uint32_t expCmdSn, uint32_t maxCmdSn)
{
    bhsInit(p, ISCSI_OP_TASK_MGMT_RSP, 0x80, 0);
    p[2] = response;
    wr32(p + 16, itt);
    wr32(p + 24, statSn);
    wr32(p + 28, expCmdSn);
    wr32(p + 32, maxCmdSn);
}

static void buildR2T(uint8_t* p, uint32_t itt, uint32_t ttt, uint32_t statSn,
                     uint32_t expCmdSn, uint32_t maxCmdSn, uint32_t r2tSn,
                     uint32_t offset, uint32_t len)
{
    bhsInit(p, ISCSI_OP_R2T, 0x80, 0);
    wr32(p + 16, itt);
    wr32(p + 20, ttt);
    wr32(p + 24, statSn);
    wr32(p + 28, expCmdSn);
    wr32(p + 32, maxCmdSn);
    wr32(p + 36, r2tSn);
    wr32(p + 40, offset);
    wr32(p + 44, len);
}

/* The 18-byte fixed-format sense block (SPC-4): response code 0x70, sense key in byte 2,
 * additional sense length 10 in byte 7, ASC/ASCQ at 12/13. */
static uint32_t buildSense(uint8_t* s, uint8_t key, uint8_t asc, uint8_t ascq)
{
    memset(s, 0, 18);
    s[0] = 0x70;
    s[2] = key;
    s[7] = 10;
    s[12] = asc;
    s[13] = ascq;
    return 18;
}

/* ----------------------------------------------------------------------------------- */
/* Text parameters (RFC 7143 section 6.1): NUL-terminated "Key=Value" entries.           */
/* ----------------------------------------------------------------------------------- */

#define TP_MAX 24
typedef struct {
    char key[48];
    char val[256];
} TPARAM;

static int tpParse(const uint8_t* d, uint32_t len, TPARAM* out, int cap)
{
    uint32_t i = 0;
    int n = 0;
    while (i < len && n < cap) {
        uint32_t j = i;
        while (j < len && d[j] != 0) { j++; }
        if (j > i) {
            const uint8_t* e = (const uint8_t*)memchr(d + i, '=', (size_t)(j - i));
            if (e != NULL) {
                uint32_t klen = (uint32_t)(e - (d + i));
                uint32_t vlen = (uint32_t)(j - i) - klen - 1u;
                if (klen >= sizeof(out[n].key)) { klen = (uint32_t)sizeof(out[n].key) - 1u; }
                if (vlen >= sizeof(out[n].val)) { vlen = (uint32_t)sizeof(out[n].val) - 1u; }
                memcpy(out[n].key, d + i, klen);
                out[n].key[klen] = 0;
                memcpy(out[n].val, d + i + klen + 1u, vlen);
                out[n].val[vlen] = 0;
                n++;
            }
            /* A key with no '=' is a key sent alone; nothing here needs its value. */
        }
        i = j + 1;
    }
    return n;
}

static const char* tpGet(const TPARAM* kv, int n, const char* key)
{
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(kv[i].key, key) == 0) { return kv[i].val; }
    }
    return NULL;
}
static int tpHas(const TPARAM* kv, int n, const char* key) { return tpGet(kv, n, key) != NULL; }

/* Append "Key=Value\0" to a login/text answer.  A responder may only answer keys the
 * initiator offered (RFC 7143 section 6.2); answering an unoffered key gets "Irrelevant"
 * back and fails the negotiation, so every call site checks tpHas() first, except for the
 * target-only keys in the security stage. */
static void tpAdd(uint8_t* buf, uint32_t cap, uint32_t* len, const char* key, const char* val)
{
    size_t k = strlen(key), v = strlen(val);
    if (*len + k + v + 2u > cap) { return; }
    memcpy(buf + *len, key, k);        *len += (uint32_t)k;
    buf[(*len)++] = '=';
    memcpy(buf + *len, val, v);        *len += (uint32_t)v;
    buf[(*len)++] = 0;
}

/* ==================================================================================== */
/* NVMe/TCP initiator (the client side of nvmeof_tcp.h)                                  */
/*                                                                                       */
/* Adapted, deliberately, from the same ~200 lines that already appear in                */
/* nvmeof_tcp.c -initiator and tcp_identify_selftest.c: the proven path stays proven, and */
/* a third copy in a different program is cheaper than refactoring a working file.       */
/* ==================================================================================== */

typedef struct {
    SOCKET   sock;
    uint16_t nextCid;
    int      open;
} NVM;

/* SQE: +0 opcode, +2 CID, +4 NSID, +40 CDW10 (LBA / CNS), +48 CDW12 (NLB-1).
 * CQE: +12 DW3 = CID | P<<16 | SC<<17. */
static void nvmSqeBuild(uint8_t* q, uint8_t opcode, uint32_t nsid, uint16_t cid,
                        uint32_t cdw10, uint32_t cdw12)
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
    nvmeofTcpPut32(q, 44, 0);
    nvmeofTcpPut32(q, 48, cdw12);
}
static uint16_t nvmCqeCid(const uint8_t* c)    { return (uint16_t)(nvmeofTcpGet32(c, 12) & 0xFFFFu); }
static uint8_t  nvmCqeStatus(const uint8_t* c) { return (uint8_t)((nvmeofTcpGet32(c, 12) >> 17) & 0xFFu); }

/* Open one NVMe/TCP connection: TCP connect, then ICReq/ICResp.  One connection per iSCSI
 * session is enough - the NVMe/TCP transport makes each queue a connection, and this bridge
 * has exactly one queue. */
static int nvmOpen(NVM* h, const char* ip, uint16_t port)
{
    struct sockaddr_in a;
    uint8_t buf[NVMEOF_TCP_ICREQ_LEN];
    NVMEOF_TCP_IC ic;
    NVMEOF_TCP_HDR hdr;

    memset(h, 0, sizeof(*h));
    h->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (h->sock == INVALID_SOCKET) { return -1; }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { return -2; }
    if (connect(h->sock, (struct sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(h->sock);
        h->sock = INVALID_SOCKET;
        return -3;
    }
    setRecvTimeout(h->sock, 30000);
    setSendTimeout(h->sock, 30000);
    setSocketBuffers(h->sock);
    h->nextCid = 1;

    memset(&ic, 0, sizeof(ic));
    ic.cpda = 4;                             /* capsule data alignment */
    nvmeofTcpBuildIc(buf, NVMEOF_TCP_ICREQ, &ic);
    if (sendAll(h->sock, buf, (int)NVMEOF_TCP_ICREQ_LEN) != (int)NVMEOF_TCP_ICREQ_LEN) { return -4; }
    if (recvAll(h->sock, buf, 8) != 1) { return -5; }
    nvmeofTcpReadHdr(buf, &hdr);
    if (hdr.type != NVMEOF_TCP_ICRESP) { return -6; }
    if (recvAll(h->sock, buf + 8, (int)NVMEOF_TCP_IC_BODY_LEN) != 1) { return -7; }
    h->open = 1;
    return 0;
}

static void nvmClose(NVM* h)
{
    if (h->open && h->sock != INVALID_SOCKET) { closesocket(h->sock); }
    h->open = 0;
    h->sock = INVALID_SOCKET;
}

/* One command, response read back.  Up to 4096 bytes of data ride inside the capsule
 * (that is one capsule payload); anything larger is moved as C2HData (Read) or H2CData
 * (Write) PDUs, which the target in nvmeof_tcp.c implements.  The target advertises
 * maxr2t = 0, so write data is host-driven and no R2T is expected. */
static int nvmSendDataPdus(NVM* h, uint16_t cid, const uint8_t* in, unsigned len)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    unsigned off = 0;

    while (off < len) {
        NVMEOF_TCP_DATA d;
        uint8_t dh[NVMEOF_TCP_DATA_HDR_LEN];
        unsigned chunk = len - off;
        unsigned pad;

        if (chunk > NVME_PDU_DATA_MAX) { chunk = NVME_PDU_DATA_MAX; }
        memset(&d, 0, sizeof(d));
        d.cccid = cid;
        d.ttag = 0;
        d.datao = off;
        d.datal = chunk;
        nvmeofTcpBuildData(dh, NVMEOF_TCP_H2CDATA,
                           (off + chunk == len) ? NVMEOF_TCP_F_LAST : 0, &d);
        if (sendAll(h->sock, dh, (int)sizeof(dh)) != (int)sizeof(dh)) { return -1; }
        if (sendAll(h->sock, in + off, (int)chunk) != (int)chunk) { return -1; }
        pad = (unsigned)((NVMEOF_TCP_PDU_ALIGN - (chunk % NVMEOF_TCP_PDU_ALIGN)) % NVMEOF_TCP_PDU_ALIGN);
        if (pad != 0 && sendAll(h->sock, zero, (int)pad) != (int)pad) { return -1; }
        off += chunk;
    }
    return 0;
}

static int nvmReadDataPdus(NVM* h, uint16_t cid, uint8_t* out, unsigned want, unsigned have0)
{
    uint8_t zero[4];
    unsigned have = have0;

    while (have < want) {
        NVMEOF_TCP_HDR hh;
        NVMEOF_TCP_DATA d;
        uint8_t dh[NVMEOF_TCP_DATA_HDR_LEN];
        unsigned pad;

        if (recvAll(h->sock, dh, (int)sizeof(dh)) != 1) { return -1; }
        nvmeofTcpReadHdr(dh, &hh);
        if (hh.type != NVMEOF_TCP_C2HDATA || hh.hlen != NVMEOF_TCP_DATA_HDR_LEN) { return -1; }
        nvmeofTcpReadData(dh, &d);
        if (d.cccid != cid || d.datal == 0) { return -1; }
        if ((uint64_t)d.datao + (uint64_t)d.datal > (uint64_t)want) { return -1; }
        if (recvAll(h->sock, out + d.datao, (int)d.datal) != 1) { return -1; }
        pad = (unsigned)((NVMEOF_TCP_PDU_ALIGN - (d.datal % NVMEOF_TCP_PDU_ALIGN)) % NVMEOF_TCP_PDU_ALIGN);
        if (pad != 0 && recvAll(h->sock, zero, (int)pad) != 1) { return -1; }
        have += d.datal;
        if ((hh.flags & NVMEOF_TCP_F_LAST) != 0) { break; }
    }
    return have == want ? 0 : -1;
}

static int nvmCmd(NVM* h, uint8_t opcode, uint32_t nsid, uint32_t cdw10, uint32_t cdw12,
                  const uint8_t* inData, unsigned inLen, uint8_t* outData, unsigned outCap,
                  unsigned* outLen, uint8_t* statusOut)
{
    uint8_t sqe[64];
    uint8_t cap[NVMEOF_TCP_CCMD_HDR_LEN];
    uint8_t hdr[8];
    uint8_t cqe[16];
    uint8_t* big;
    unsigned n, total;
    NVMEOF_TCP_HDR hh;
    uint16_t cid = h->nextCid++;
    int bigWrite = (inLen > NVME_CAPSULE_DATA_MAX);
    int bigRead  = (outData != NULL && outCap > NVME_CAPSULE_DATA_MAX);

    nvmSqeBuild(sqe, opcode, nsid, cid, cdw10, cdw12);
    n = nvmeofTcpBuildCapsuleCmd(cap, sqe, bigWrite ? 0u : inLen, NVMEOF_TCP_CCMD_HDR_LEN);
    total = n + (bigWrite ? 0u : inLen);
    big = (uint8_t*)malloc(total);
    if (big == NULL) { return -1; }
    memcpy(big, cap, n);
    if (!bigWrite && inLen != 0) {
        if (inData == NULL) { free(big); return -1; }
        memcpy(big + n, inData, inLen);
    }
    if (sendAll(h->sock, big, (int)total) != (int)total) { free(big); return -2; }
    free(big);
    if (bigWrite) {
        if (inData == NULL) { return -1; }
        if (nvmSendDataPdus(h, cid, inData, inLen) != 0) { return -2; }
    }

    {
        uint8_t zero[4] = { 0, 0, 0, 0 };
        unsigned wire = 0;

        /* Data PDUs may arrive before the completion (spec order, and what the target
         * does since 2026-10-18) or after it (what it used to do).  Accept both: read
         * until the CapsuleResp is in hand, then finish any remaining data PDUs. */
        for (;;) {
            if (recvAll(h->sock, hdr, 8) != 1) { return -3; }
            nvmeofTcpReadHdr(hdr, &hh);
            if (hh.type == NVMEOF_TCP_C2HDATA) {
                NVMEOF_TCP_DATA d;
                uint8_t dh[NVMEOF_TCP_DATA_HDR_LEN];
                unsigned pad;

                if (hh.hlen != NVMEOF_TCP_DATA_HDR_LEN) { say("  [nvme] bad C2HData hlen=%u flags=0x%02X\n", (unsigned)hh.hlen, (unsigned)hh.flags); return -4; }
                memcpy(dh, hdr, 8);
                if (recvAll(h->sock, dh + 8, (int)(hh.hlen - 8)) != 1) { return -5; }
                nvmeofTcpReadData(dh, &d);
                if (d.cccid != cid || d.datal == 0) { say("  [nvme] bad C2HData cccid=%u cid=%u datal=%u\n", (unsigned)d.cccid, (unsigned)cid, (unsigned)d.datal); return -4; }
                if (outData == NULL || (uint64_t)d.datao + (uint64_t)d.datal > (uint64_t)outCap) { say("  [nvme] C2HData range %u+%u > %u\n", (unsigned)d.datao, (unsigned)d.datal, outCap);
                    return -4;
                }
                if (recvAll(h->sock, outData + d.datao, (int)d.datal) != 1) { return -5; }
                pad = (unsigned)((NVMEOF_TCP_PDU_ALIGN - (d.datal % NVMEOF_TCP_PDU_ALIGN)) % NVMEOF_TCP_PDU_ALIGN);
                if (pad != 0 && recvAll(h->sock, zero, (int)pad) != 1) { return -5; }
                wire += d.datal;
                if (outLen != NULL) { *outLen = wire; }
                continue;
            }
            if (hh.type != NVMEOF_TCP_CAPSULERESP) { say("  [nvme] unexpected PDU type 0x%02X hlen=%u plen=%u\n", (unsigned)hh.type, (unsigned)hh.hlen, (unsigned)hh.plen); return -4; }
            break;
        }
        if (recvAll(h->sock, cqe, 16) != 1) { return -5; }
        if (hh.plen > outCap) { return -6; }
        if (hh.plen != 0) {
            if (outData == NULL) { return -7; }
            if (recvAll(h->sock, outData, (int)hh.plen) != 1) { return -8; }
        }
        if (outLen != NULL) { *outLen = (wire > hh.plen) ? wire : hh.plen; }
        if (bigRead && hh.plen == 0 && (hh.flags & NVMEOF_TCP_F_SUCCESS) != 0 && wire < outCap) {
            /* The completion came first and the tail is still on the wire. */
            if (nvmReadDataPdus(h, cid, outData, outCap, wire) != 0) { return -9; }
            if (outLen != NULL) { *outLen = outCap; }
        }
    }
    if (statusOut != NULL) {
        /* The SUCCESS flag in the capsule header is authoritative: the target in nvmeof_tcp.c
         * sets that flag from its own status but leaves the 16-byte completion it places
         * inside the capsule uninitialised, so the status and command id in the completion
         * are stack garbage.  (That file's own -initiator mode reads the field too and gets
         * away with it because the stack happens to be zero.)  The completion is therefore
         * used only to tell one kind of failure from another. */
        if ((hh.flags & NVMEOF_TCP_F_SUCCESS) != 0) {
            *statusOut = 0;
        } else {
            uint8_t st = nvmCqeStatus(cqe);
            *statusOut = (st != 0) ? st : 0xFFu;
        }
    }
    if (nvmCqeCid(cqe) != cid) {
        static int noted = 0;
        if (!noted) {
            noted = 1;
            say("  [nvme] note: the response completion carries CID %u for our %u - the target\n"
                "         in nvmeof_tcp.c never fills the completion in, so this client trusts\n"
                "         the capsule SUCCESS flag instead\n", (unsigned)nvmCqeCid(cqe), (unsigned)cid);
        }
    }
    return 0;
}

/* ==================================================================================== */
/* Bridge configuration and per-session state                                            */
/* ==================================================================================== */

typedef struct {
    char     listenIp[64];
    uint16_t listenPort;
    char     targetIp[64];
    uint16_t targetPort;
    uint32_t lbaBase;      /* NVMe LBA that SCSI LBA 0 maps onto */
    uint32_t nblocks;      /* how many 512-byte blocks this LUN exposes */
} BRIDGE;

typedef struct {
    uint8_t  bhs[48];
    uint8_t* data;
    uint32_t len;
} DEFER;

typedef struct SESS {
    SOCKET   sock;
    unsigned id;
    BRIDGE*  br;

    int      discovery;
    int      loggedIn;
    int      fullFeature;
    uint16_t tsih;
    uint32_t statSn;
    uint32_t expCmdSn;
    uint32_t maxCmdSn;

    uint32_t maxRecvDsl;   /* peer's MaxRecvDataSegmentLength: caps one Data-In PDU     */
    uint32_t firstBurst;   /* negotiated FirstBurstLength: unsolicited write data       */
    uint32_t maxBurst;     /* negotiated MaxBurstLength (also one R2T's worth)          */
    int      unsolicited;  /* InitialR2T negotiated to No                               */

    NVM      nvm;

    uint8_t* rx;
    uint32_t rxCap;

    DEFER    defer[MAX_DEFER];
    int      deferN;

    /* per-command bookkeeping for the response */
    uint32_t edtlThis;
    uint32_t sentThis;
    uint32_t dataSnSent;
    uint8_t  lastSense[18];
    uint32_t lastSenseLen;

    unsigned long long cmdCount;
    unsigned long long nvmeReads;
    unsigned long long nvmeWrites;
    unsigned long long bytesRead;
    unsigned long long bytesWritten;
} SESS;

/* ==================================================================================== */
/* The backend: SCSI LBA range -> NVMe Read/Write, in 4096-byte steps                    */
/* ==================================================================================== */

static int nvmEnsure(SESS* s)
{
    int rc;
    if (s->nvm.open) { return 0; }
    rc = nvmOpen(&s->nvm, s->br->targetIp, s->br->targetPort);
    if (rc != 0) {
        say("  [nvme] cannot open %s:%u (rc=%d)\n", s->br->targetIp, s->br->targetPort, rc);
        return rc;
    }
    say("  [nvme] connected to %s:%u (ICReq/ICResp exchanged)\n",
        s->br->targetIp, s->br->targetPort);
    return 0;
}

/* One NVMe Read of up to NVME_CHUNK_BLOCKS (64 KiB) blocks; data over 4096 bytes comes back as C2HData PDUs.  On a transport error the connection is
 * dropped so the next command reopens it: the NVMe/TCP target closes an idle connection
 * after its own receive timeout, and a bridge that kept a dead socket would answer every
 * later SCSI command with an I/O error. */
static int nvmReadChunk(SESS* s, uint32_t lba, uint32_t nblocks, uint8_t* out)
{
    unsigned len = 0;
    uint8_t sc = 0xFF;
    int rc = nvmCmd(&s->nvm, NVME_OP_READ, NVME_NSID, lba, nblocks - 1u,
                    NULL, 0, out, nblocks * BLOCK_SIZE, &len, &sc);
    if (rc != 0 || sc != NVME_SC_SUCCESS || len != nblocks * BLOCK_SIZE) {
        say("  [nvme] Read lba=%u nlb=%u failed (rc=%d sc=0x%02X len=%u)\n",
            lba, nblocks, rc, (unsigned)sc, len);
        nvmClose(&s->nvm);
        return -1;
    }
    s->nvmeReads++;
    return 0;
}

static int nvmWriteChunk(SESS* s, uint32_t lba, uint32_t nblocks, const uint8_t* in)
{
    unsigned len = 0;
    uint8_t sc = 0xFF;
    int rc = nvmCmd(&s->nvm, NVME_OP_WRITE, NVME_NSID, lba, nblocks - 1u,
                    in, nblocks * BLOCK_SIZE, NULL, 0, &len, &sc);
    if (rc != 0 || sc != NVME_SC_SUCCESS) {
        say("  [nvme] Write lba=%u nlb=%u failed (rc=%d sc=0x%02X)\n",
            lba, nblocks, rc, (unsigned)sc);
        nvmClose(&s->nvm);
        return -1;
    }
    s->nvmeWrites++;
    return 0;
}

/* A whole SCSI transfer, split into NVME_CHUNK_BLOCKS (64 KiB) NVMe commands and
 * reassembled.  One command per iSCSI Data-In/Data-Out burst instead of sixteen. */
static int nvmReadRange(SESS* s, uint64_t lba, uint32_t nblocks, uint8_t* out)
{
    uint32_t done = 0;
    if (nvmEnsure(s) != 0) { return -1; }
    while (done < nblocks) {
        uint32_t n = nblocks - done;
        if (n > NVME_CHUNK_BLOCKS) { n = NVME_CHUNK_BLOCKS; }
        if (nvmReadChunk(s, (uint32_t)(lba + done), n, out + (size_t)done * BLOCK_SIZE) != 0) {
            /* One reopen-and-retry: a connection the NVMe target closed while idle is the
             * common case and must not surface as a disk error. */
            if (nvmEnsure(s) != 0) { return -1; }
            if (nvmReadChunk(s, (uint32_t)(lba + done), n, out + (size_t)done * BLOCK_SIZE) != 0) {
                return -1;
            }
        }
        done += n;
    }
    return 0;
}

static int nvmWriteRange(SESS* s, uint64_t lba, uint32_t nblocks, const uint8_t* in)
{
    uint32_t done = 0;
    if (nvmEnsure(s) != 0) { return -1; }
    while (done < nblocks) {
        uint32_t n = nblocks - done;
        if (n > NVME_CHUNK_BLOCKS) { n = NVME_CHUNK_BLOCKS; }
        if (nvmWriteChunk(s, (uint32_t)(lba + done), n, in + (size_t)done * BLOCK_SIZE) != 0) {
            if (nvmEnsure(s) != 0) { return -1; }
            if (nvmWriteChunk(s, (uint32_t)(lba + done), n, in + (size_t)done * BLOCK_SIZE) != 0) {
                return -1;
            }
        }
        done += n;
    }
    return 0;
}

/* ==================================================================================== */
/* iSCSI responses                                                                      */
/* ==================================================================================== */

static int sessSendPdu(SESS* s, const uint8_t* bhs, const void* data, uint32_t len)
{
    trace("      -> op=0x%02X f=0x%02X dsl=%u itt=0x%08X statsn=%u expcmdsn=%u maxcmdsn=%u\n",
          (unsigned)bhs[0], (unsigned)bhs[1], rd24(bhs + 5), rd32(bhs + 16), rd32(bhs + 24),
          rd32(bhs + 28), rd32(bhs + 32));
    if (iscsiSendPdu(s->sock, bhs, data, len) != 0) {
        say("  [conn %u] send failed\n", s->id);
        return -1;
    }
    return 0;
}

/* One Data-In PDU, or as many as the peer's MaxRecvDataSegmentLength requires.  The StatSN of
 * a Data-In PDU is the StatSN the following SCSI Response will carry, so it is not
 * incremented here.  The data segment is padded by iscsiSendPdu; a 7-byte VPD list with no
 * padding puts the next PDU one byte out of phase and the initiator drops the session.
 *
 * TWO THINGS ARE COMPUTED AGAINST THE WHOLE COMMAND, not against this call, and both were a
 * real bug once:
 *   - The Buffer Offset starts at s->sentThis, so a 128 KiB read that arrives here as two
 *     64 KiB calls continues at 65536 instead of restarting at 0.
 *   - The F bit is set only on the last PDU of the LAST call (`lastCall`).  Marking the end of
 *     each call final tells the initiator the data ended 64 KiB early; the next Data-In PDU
 *     is then a protocol violation, and Windows resets the connection.  That is exactly how
 *     this bridge behaved until it was caught with a 128 KiB READ(10). */
static int sessDataInEx(SESS* s, uint32_t itt, const uint8_t* data, uint32_t len, int lastCall)
{
    uint32_t off = 0;
    if (len == 0) { return 0; }
    while (off < len) {
        uint8_t bhs[48];
        uint32_t chunk = len - off;
        uint32_t absOff = s->sentThis + off;
        int final;
        if (chunk > s->maxRecvDsl) { chunk = s->maxRecvDsl; }
        final = lastCall && (off + chunk == len);
        buildDataIn(bhs, itt, s->statSn, s->expCmdSn, s->maxCmdSn, s->dataSnSent, absOff,
                    chunk, final);
        trace("      -> Data-In dataSN=%u offset=%u len=%u%s\n", s->dataSnSent, absOff, chunk,
              final ? " (F)" : "");
        if (sessSendPdu(s, bhs, data + off, chunk) != 0) { return -1; }
        s->dataSnSent++;
        off += chunk;
    }
    s->sentThis += len;
    return 0;
}

/* Every response except the streaming read sends all of its data in one call. */
static int sessDataIn(SESS* s, uint32_t itt, const uint8_t* data, uint32_t len)
{
    return sessDataInEx(s, itt, data, len, 1);
}

/* The final SCSI Response.  residual = EDTL - what actually moved, with the underflow bit
 * set, because a target that returns 8 of the 96 bytes asked for and then reports "no
 * residual" tells the initiator two contradictory things. */
static int sessScsiRsp(SESS* s, uint32_t itt, uint8_t status, const uint8_t* sense,
                       uint32_t senseLen, uint32_t residual, int underflow)
{
    uint8_t bhs[48];
    uint8_t seg[2 + 18];
    uint32_t segLen = 0;

    if (status == SCSI_STATUS_GOOD && !underflow && residual == 0 && s->sentThis < s->edtlThis) {
        residual = s->edtlThis - s->sentThis;
        underflow = 1;
    }
    if (senseLen != 0) {
        wr16(seg, (uint16_t)senseLen);
        memcpy(seg + 2, sense, senseLen);
        segLen = 2 + senseLen;
    }
    buildScsiRsp(bhs, itt, status, underflow, s->statSn, s->expCmdSn, s->maxCmdSn,
                 s->dataSnSent, residual, senseLen);
    s->statSn++;
    return sessSendPdu(s, bhs, seg, segLen);
}

/* ==================================================================================== */
/* SCSI command set                                                                     */
/* ==================================================================================== */

static void sessLogScsi(SESS* s, const char* name, const uint8_t* cdb, uint32_t edtl)
{
    say("  [conn %u] scsi %-20s cdb=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
        "edtl=%u\n",
        s->id, name, cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8],
        cdb[9], edtl);
}

/* INQUIRY.  The standard response is 36 bytes; Windows asks for 96 and the whole allocation
 * length is handed back zero-filled, so the residual is always 0.  VPD pages 0x00 (supported
 * list), 0x80 (serial) and 0x83 (device identification) are the three the Windows storage
 * stack reads; without them enumeration is unreliable. */
static int sessInquiry(SESS* s, uint32_t itt, const uint8_t* cdb)
{
    uint8_t evpd = (uint8_t)(cdb[1] & 0x01u);
    uint8_t page = cdb[2];
    uint32_t alloc = rd16(cdb + 3);
    uint8_t out[256];
    uint32_t n = 0;

    memset(out, 0, sizeof(out));
    if (!evpd) {
        out[0] = 0x00;                       /* peripheral qualifier 0, direct access */
        out[1] = 0x00;                       /* not removable */
        out[2] = 0x05;                       /* SPC-3 */
        out[3] = 0x02;                       /* response data format */
        out[4] = 31;                         /* additional length */
        memcpy(out + 8, "NVMEOFT ", 8);      /* vendor identification */
        memcpy(out + 16, "iSCSI-NVMe/TCP  ", 16);
        memcpy(out + 32, "0001", 4);
        n = 36;
        if (alloc > n) { n = (alloc < sizeof(out)) ? alloc : (uint32_t)sizeof(out); }
    } else if (page == 0x00) {
        static const uint8_t pages[3] = { 0x00, 0x80, 0x83 };
        out[3] = 3;
        memcpy(out + 4, pages, 3);
        n = 7;
        if (alloc > n) { n = (alloc < sizeof(out)) ? alloc : (uint32_t)sizeof(out); }
    } else if (page == 0x80) {
        static const char serial[] = "NVMEOF-TCP-BRIDGE-0001";
        out[1] = 0x80;
        wr16(out + 2, (uint16_t)(sizeof(serial) - 1));
        memcpy(out + 4, serial, sizeof(serial) - 1);
        n = 4 + (uint32_t)(sizeof(serial) - 1);
        if (alloc > n) { n = (alloc < sizeof(out)) ? alloc : (uint32_t)sizeof(out); }
    } else if (page == 0x83) {
        static const char ident[] = "NVMEOFT1";
        out[1] = 0x83;
        wr16(out + 2, 12);                   /* page length: 4-byte descriptor + 8 */
        out[4] = 0x02;                       /* code set: ASCII */
        out[5] = 0x03;                       /* designator: vendor ID (type 3) */
        out[6] = 0x00;                       /* association: LUN */
        out[7] = (uint8_t)(sizeof(ident) - 1);
        memcpy(out + 8, ident, sizeof(ident) - 1);
        n = 16;
        if (alloc > n) { n = (alloc < sizeof(out)) ? alloc : (uint32_t)sizeof(out); }
    } else {
        uint8_t sense[18];
        uint32_t sl = buildSense(sense, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        say("  [conn %u] INQUIRY VPD page 0x%02X not supported -> CHECK CONDITION\n",
            s->id, (unsigned)page);
        return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, s->edtlThis, 1);
    }
    if (sessDataIn(s, itt, out, n) != 0) { return -1; }
    return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);
}

/* MODE SENSE(6)/(10): a header with no mode pages.  Windows reads pages 0x08 (caching) and
 * 0x3F (all); answering with just the header is legal and means "no mode pages". */
static int sessModeSense(SESS* s, uint32_t itt, const uint8_t* cdb, uint8_t op)
{
    uint8_t out[256];
    uint32_t alloc, n;
    memset(out, 0, sizeof(out));
    if (op == 0x1A) {
        out[0] = 3;                          /* mode data length */
        out[1] = 0x00;                       /* medium type */
        out[2] = 0x00;                       /* device specific: not write protected */
        out[3] = 0x00;                       /* block descriptor length */
        n = 4;
        alloc = cdb[4];
    } else {
        wr16(out + 0, 6);                    /* mode data length */
        out[2] = 0x00;
        wr16(out + 6, 0);                    /* block descriptor length */
        n = 8;
        alloc = rd16(cdb + 7);
    }
    if (alloc > n) { n = (alloc < sizeof(out)) ? alloc : (uint32_t)sizeof(out); }
    if (sessDataIn(s, itt, out, n) != 0) { return -1; }
    return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);
}

/* READ CAPACITY(10): last LBA and block size, both 32-bit big-endian.
 *
 * The CDB's allocation length does NOT decide this.  Measured off the real Windows
 * initiator: it sends READ CAPACITY(10) with an allocation length of 0 and an
 * ExpectedDataTransferLength of 8, and it expects the 8-byte answer.  A target that
 * honours the zero allocation length returns nothing, Windows concludes the disk has no
 * capacity, and the session is torn down and retried for ever - which is exactly the
 * failure this function used to produce.  The parameter data for READ CAPACITY(10) is a
 * fixed 8 bytes; every target returns it. */
static int sessReadCapacity(SESS* s, uint32_t itt)
{
    uint8_t out[8];
    uint32_t nb = s->br->nblocks;
    wr32(out, nb ? nb - 1u : 0u);
    wr32(out + 4, BLOCK_SIZE);
    if (sessDataIn(s, itt, out, sizeof(out)) != 0) { return -1; }
    return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);
}

/* REQUEST SENSE: "no sense" (all zeros behind a valid header) after a good command, and the
 * sense of the last refusal after one. */
static int sessRequestSense(SESS* s, uint32_t itt, uint32_t alloc)
{
    uint8_t out[18];
    uint32_t n = (uint32_t)sizeof(out);
    if (s->lastSenseLen != 0) {
        memcpy(out, s->lastSense, s->lastSenseLen);
    } else {
        memset(out, 0, sizeof(out));
        out[0] = 0x70;
        out[7] = 10;
    }
    if (alloc < n) { n = alloc; }
    if (n != 0) {
        if (sessDataIn(s, itt, out, n) != 0) { return -1; }
    }
    s->lastSenseLen = 0;
    return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);
}

/* READ(10): split into NVMe Reads of at most 4096 bytes and stream them back out as Data-In
 * PDUs.  A 64 KiB READ(10) is sixteen NVMe commands over one TCP connection - the documented
 * consequence of the in-capsule 4096-byte limit. */
static int sessRead10(SESS* s, uint32_t itt, uint32_t edtl, uint64_t lba, uint32_t nblocks)
{
    uint8_t* buf;
    uint32_t chunkBlocks;
    uint32_t done = 0;
    uint64_t nvmeLba = (uint64_t)s->br->lbaBase + lba;
    DWORD t0 = GetTickCount();

    if (nvmEnsure(s) != 0) {
        uint8_t sense[18];
        uint32_t sl = buildSense(sense, SENSE_HARDWARE_ERROR, ASC_INTERNAL_FAILURE, 0);
        return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, edtl, 1);
    }
    buf = (uint8_t*)malloc(ISCSI_MAX_DSL);
    if (buf == NULL) { return -1; }

    /* Stream in units of at most one Data-In PDU: the buffer is 64 KiB, not the transfer
     * size, so a 1 MiB read works without a 1 MiB allocation. */
    chunkBlocks = s->maxRecvDsl / BLOCK_SIZE;
    if (chunkBlocks > nblocks) { chunkBlocks = nblocks; }
    if (chunkBlocks == 0) { chunkBlocks = 1; }

    while (done < nblocks) {
        uint32_t n = nblocks - done;
        if (n > chunkBlocks) { n = chunkBlocks; }
        if (nvmReadRange(s, nvmeLba + done, n, buf) != 0) {
            uint8_t sense[18];
            uint32_t sl = buildSense(sense, SENSE_MEDIUM_ERROR, ASC_INTERNAL_FAILURE, 0);
            memcpy(s->lastSense, sense, sl);
            s->lastSenseLen = sl;
            free(buf);
            return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl,
                               edtl - done * BLOCK_SIZE, 1);
        }
        if (sessDataInEx(s, itt, buf, n * BLOCK_SIZE, (done + n == nblocks)) != 0) {
            free(buf);
            return -1;
        }
        done += n;
    }
    free(buf);
    s->bytesRead += (unsigned long long)nblocks * BLOCK_SIZE;
    say("  [scsi] READ(10) lba=%llu n=%u (%u B) -> %u NVMe Read cmd(s) of <=%u KiB, %lu ms\n",
        (unsigned long long)lba, nblocks, nblocks * BLOCK_SIZE,
        nvmeChunkCount(nblocks), (unsigned)(NVME_CHUNK_BYTES / 1024u), (unsigned long)(GetTickCount() - t0));
    return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);
}

/* WRITE(10): the data arrives as Data-Out PDUs after the command PDU, so the burst is
 * drained first and only then handed to NVMe.  Draining before deciding anything is not a
 * style choice: with InitialR2T=No the initiator pushes its first burst the moment it has
 * sent the command and does not wait to hear whether the target likes it, so a refusal that
 * answered without reading would leave those bytes in the socket and the next read would take
 * a payload for a BHS. */
static int sessWrite10(SESS* s, uint32_t itt, uint32_t edtl, uint64_t lba, uint32_t nblocks,
                       const uint8_t* imm, uint32_t immLen)
{
    uint8_t* buf;
    uint32_t got = 0;
    uint32_t r2tSn = 0;
    int r2tOutstanding = 0;
    uint64_t nvmeLba = (uint64_t)s->br->lbaBase + lba;
    DWORD t0 = GetTickCount();

    if (edtl > MAX_XFER) {
        uint8_t sense[18];
        uint32_t sl = buildSense(sense, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        say("  [scsi] WRITE(10) edtl=%u is larger than this bridge accepts (%u)\n",
            edtl, MAX_XFER);
        return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, edtl, 1);
    }
    buf = (uint8_t*)calloc(1, edtl ? edtl : 1u);
    if (buf == NULL) { return -1; }

    /* Immediate data exists only if ImmediateData was negotiated to Yes, which this bridge
     * answers No to.  Handled anyway because it is five lines and the alternative is
     * misreading the initiator's first bytes as a PDU. */
    if (immLen != 0 && edtl != 0) {
        uint32_t take = (immLen < edtl) ? immLen : edtl;
        memcpy(buf, imm, take);
        got = take;
        say("  [scsi] WRITE(10): %u byte(s) of immediate data arrived in the command PDU\n",
            take);
    }

    /* InitialR2T=Yes in the offer would mean the initiator waits for an R2T before sending
     * anything; ask for the whole transfer up front in that case. */
    if (!s->unsolicited && got < edtl) {
        uint8_t p[48];
        uint32_t want = edtl - got;
        if (want > s->maxBurst) { want = s->maxBurst; }
        buildR2T(p, itt, 0xFFFFFFFFu, s->statSn, s->expCmdSn, s->maxCmdSn, r2tSn++, got, want);
        if (sessSendPdu(s, p, NULL, 0) != 0) { free(buf); return -1; }
        r2tOutstanding = 1;
        say("  [scsi] WRITE(10): InitialR2T=Yes -> R2T offset=%u len=%u\n", got, want);
    }

    while (got < edtl) {
        uint8_t bhs[48];
        uint8_t* data = NULL;
        uint32_t cap = 0, dlen = 0;
        int r = iscsiRecvPdu(s->sock, bhs, &data, &cap, &dlen);
        if (r != IO_OK) {
            say("  [scsi] WRITE(10): initiator stopped after %u of %u bytes (r=%d)\n",
                got, edtl, r);
            free(data);
            free(buf);
            return -1;
        }
        if (bhsOpcode(bhs) == ISCSI_OP_DATA_OUT) {
            uint32_t off = bhsDoBufOff(bhs);
            if (r2tOutstanding) { r2tOutstanding = 0; }
            if (off != got) {
                /* Windows restarts the Buffer Offset at 0 for the burst that answers each R2T
                 * (measured: 1 MiB writes arrive as sixteen 64 KiB bursts, every one of them
                 * at offset 0), so the field is a cross-check and not the placement rule.
                 * DataPDUInOrder=Yes is negotiated, and the running total is what the
                 * initiator's own view of this transfer is. */
                trace("  [scsi] WRITE(10): Data-Out offset %u, expected %u (appending)\n",
                      off, got);
            }
            if (dlen > edtl - got) {
                say("  [scsi] WRITE(10): Data-Out of %u bytes overruns the transfer by %u\n",
                    dlen, dlen - (edtl - got));
                free(data);
                free(buf);
                return -1;
            }
            memcpy(buf + got, data, dlen);
            got += dlen;
            free(data);
            if (bhsFinal(bhs) && got < edtl) {
                /* The burst ended and the transfer is not complete: ask for the rest.  With
                 * MaxBurstLength = 64 KiB and Windows' 64 KiB write CDBs this never fires;
                 * it exists so that a larger CDB stalls nothing. */
                uint8_t p[48];
                uint32_t want = edtl - got;
                if (want > s->maxBurst) { want = s->maxBurst; }
                buildR2T(p, itt, 0xFFFFFFFFu, s->statSn, s->expCmdSn, s->maxCmdSn,
                         r2tSn++, got, want);
                trace("  [scsi] WRITE(10): burst ended at %u/%u -> R2T offset=%u len=%u\n",
                      got, edtl, got, want);
                if (sessSendPdu(s, p, NULL, 0) != 0) { free(buf); return -1; }
            }
        } else if (bhsOpcode(bhs) == ISCSI_OP_NOP_OUT) {
            uint8_t p[48];
            buildNopIn(p, bhsITT(bhs), bhsTTT(bhs), s->statSn, s->expCmdSn, s->maxCmdSn);
            s->statSn++;
            free(data);
            if (sessSendPdu(s, p, NULL, 0) != 0) { free(buf); return -1; }
        } else {
            /* Something that is not part of this write arrived mid-burst: hold it for the
             * main loop rather than answering it out of order. */
            if (s->deferN >= MAX_DEFER) {
                say("  [scsi] WRITE(10): more than %d deferred PDUs, giving up\n", MAX_DEFER);
                free(data);
                free(buf);
                return -1;
            }
            memcpy(s->defer[s->deferN].bhs, bhs, 48);
            s->defer[s->deferN].data = data;      /* ownership moves to the queue */
            s->defer[s->deferN].len = dlen;
            s->deferN++;
            say("  [scsi] WRITE(10): opcode 0x%02X arrived mid-burst, deferred\n",
                (unsigned)bhsOpcode(bhs));
        }
    }

    if (nvmWriteRange(s, nvmeLba, nblocks, buf) != 0) {
        uint8_t sense[18];
        uint32_t sl = buildSense(sense, SENSE_MEDIUM_ERROR, ASC_INTERNAL_FAILURE, 0);
        memcpy(s->lastSense, sense, sl);
        s->lastSenseLen = sl;
        free(buf);
        return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, 0, 0);
    }
    free(buf);
    s->bytesWritten += (unsigned long long)nblocks * BLOCK_SIZE;
    say("  [scsi] WRITE(10) lba=%llu n=%u (%u B) -> %u NVMe Write cmd(s) of <=%u KiB, %lu ms\n",
        (unsigned long long)lba, nblocks, nblocks * BLOCK_SIZE,
        nvmeChunkCount(nblocks), (unsigned)(NVME_CHUNK_BYTES / 1024u), (unsigned long)(GetTickCount() - t0));
    return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);
}

/* Read out and discard a write burst that belongs to a command being refused, so the bytes
 * do not land in the stream where the next BHS is expected. */
static int sessDrainBurst(SESS* s, uint32_t edtl)
{
    uint32_t left = edtl;
    while (left != 0) {
        uint8_t bhs[48];
        uint8_t* data = NULL;
        uint32_t cap = 0, dlen = 0;
        int r = iscsiRecvPdu(s->sock, bhs, &data, &cap, &dlen);
        if (r != IO_OK) { free(data); return -1; }
        if (bhsOpcode(bhs) != ISCSI_OP_DATA_OUT) {
            if (s->deferN < MAX_DEFER) {
                memcpy(s->defer[s->deferN].bhs, bhs, 48);
                s->defer[s->deferN].data = data;
                s->defer[s->deferN].len = dlen;
                s->deferN++;
            } else {
                free(data);
            }
            continue;
        }
        left = (dlen > left) ? 0u : left - dlen;
        free(data);
    }
    return 0;
}

/* One SCSI Command PDU.  `cmdData` is the command PDU's data segment (immediate write data),
 * normally empty because ImmediateData is negotiated to No. */
static int sessScsi(SESS* s, const uint8_t* bhs, const uint8_t* cmdData, uint32_t cmdDataLen)
{
    uint32_t itt = bhsITT(bhs);
    uint32_t edtl = bhsEDTL(bhs);
    uint32_t cmdSn = bhsCmdSN(bhs);
    const uint8_t* cdb = bhs + 32;
    uint8_t op = cdb[0];
    uint64_t lba;
    uint32_t nblocks;
    uint8_t sense[18];
    uint32_t sl;

    /* The command window has to be reopened for every command: MaxCmdSN starts at 1 (the
     * value the login response carried) and an initiator may not send a CmdSN above it.
     * Left alone the initiator gets exactly one command through and then silently has no
     * legal CmdSN - the session stays open and nothing ever happens again. */
    s->expCmdSn = cmdSn + 1u;
    s->maxCmdSn = s->expCmdSn + CMD_WINDOW - 1u;
    s->edtlThis = edtl;
    s->sentThis = 0;
    s->dataSnSent = 0;
    s->cmdCount++;

    switch (op) {
    case 0x00:                                     /* TEST UNIT READY */
        sessLogScsi(s, "TEST UNIT READY", cdb, edtl);
        return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);

    case 0x03:                                     /* REQUEST SENSE */
        sessLogScsi(s, "REQUEST SENSE", cdb, edtl);
        return sessRequestSense(s, itt, cdb[4]);

    case 0x12:                                     /* INQUIRY */
        sessLogScsi(s, "INQUIRY", cdb, edtl);
        return sessInquiry(s, itt, cdb);

    case 0x1A:                                     /* MODE SENSE(6) */
        sessLogScsi(s, "MODE SENSE(6)", cdb, edtl);
        return sessModeSense(s, itt, cdb, op);

    case 0x5A:                                     /* MODE SENSE(10) */
        sessLogScsi(s, "MODE SENSE(10)", cdb, edtl);
        return sessModeSense(s, itt, cdb, op);

    case 0x25:                                     /* READ CAPACITY(10) */
        sessLogScsi(s, "READ CAPACITY(10)", cdb, edtl);
        return sessReadCapacity(s, itt);

    case 0xA0: {                                   /* REPORT LUNS */
        uint8_t out[16];
        uint32_t alloc = rd32(cdb + 6);
        uint32_t n = 16;
        sessLogScsi(s, "REPORT LUNS", cdb, edtl);
        memset(out, 0, sizeof(out));
        wr32(out, 8);                              /* LUN list length: one LUN */
        if (alloc < n) { n = alloc; }
        if (n != 0 && sessDataIn(s, itt, out, n) != 0) { return -1; }
        return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);
    }

    case 0x1B:                                     /* START STOP UNIT */
        sessLogScsi(s, "START STOP UNIT", cdb, edtl);
        return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);

    case 0x35:                                     /* SYNCHRONIZE CACHE(10) */
        /* A no-op, and said out loud: every NVMe write is acknowledged by the backend before
         * the SCSI command that carried it completes, so there is nothing left to flush. */
        sessLogScsi(s, "SYNCHRONIZE CACHE(10)", cdb, edtl);
        return sessScsiRsp(s, itt, SCSI_STATUS_GOOD, NULL, 0, 0, 0);

    case 0x41:                                     /* WRITE SAME(10) */
        sessLogScsi(s, "WRITE SAME(10) REFUSED", cdb, edtl);
        say("  [scsi] WRITE SAME(10) was asked for and is refused (not implemented)\n");
        if (edtl != 0 && edtl <= MAX_XFER) { if (sessDrainBurst(s, edtl) != 0) { return -1; } }
        sl = buildSense(sense, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0);
        memcpy(s->lastSense, sense, sl);
        s->lastSenseLen = sl;
        return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, edtl, 1);

    case 0x28:                                     /* READ(10) */
        lba = rd32(cdb + 2);
        nblocks = rd16(cdb + 7);
        sessLogScsi(s, "READ(10)", cdb, edtl);
        if (nblocks == 0 || lba + nblocks > s->br->nblocks ||
            (uint64_t)nblocks * BLOCK_SIZE > edtl) {
            sl = buildSense(sense, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            memcpy(s->lastSense, sense, sl);
            s->lastSenseLen = sl;
            say("  [scsi] READ(10) out of range: lba=%llu n=%u (the LUN has %u blocks)\n",
                (unsigned long long)lba, nblocks, s->br->nblocks);
            return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, edtl, 1);
        }
        if ((uint64_t)nblocks * BLOCK_SIZE > MAX_XFER) {
            sl = buildSense(sense, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
            say("  [scsi] READ(10) of %u blocks exceeds the %u-byte limit\n", nblocks, MAX_XFER);
            return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, edtl, 1);
        }
        return sessRead10(s, itt, edtl, lba, nblocks);

    case 0x2A:                                     /* WRITE(10) */
        lba = rd32(cdb + 2);
        nblocks = rd16(cdb + 7);
        sessLogScsi(s, "WRITE(10)", cdb, edtl);
        if (nblocks == 0 || lba + nblocks > s->br->nblocks ||
            (uint64_t)nblocks * BLOCK_SIZE > edtl) {
            sl = buildSense(sense, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            memcpy(s->lastSense, sense, sl);
            s->lastSenseLen = sl;
            say("  [scsi] WRITE(10) out of range: lba=%llu n=%u (the LUN has %u blocks)\n",
                (unsigned long long)lba, nblocks, s->br->nblocks);
            /* The initiator is already pushing its first burst; read it before answering. */
            if (edtl != 0 && edtl <= MAX_XFER) { if (sessDrainBurst(s, edtl) != 0) { return -1; } }
            return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, edtl, 1);
        }
        return sessWrite10(s, itt, edtl, lba, nblocks, cmdData, cmdDataLen);

    default:
        sessLogScsi(s, "UNSUPPORTED", cdb, edtl);
        say("  [scsi] CDB 0x%02X is not implemented -> CHECK CONDITION / ILLEGAL REQUEST "
            "(ASC 0x20)\n", (unsigned)op);
        sl = buildSense(sense, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0);
        memcpy(s->lastSense, sense, sl);
        s->lastSenseLen = sl;
        return sessScsiRsp(s, itt, SCSI_STATUS_CHECK_COND, sense, sl, edtl, 1);
    }
}

/* ==================================================================================== */
/* Login, text, and the session loop                                                     */
/* ==================================================================================== */

/* A number key may only be lowered by the responder; a boolean OR/AND key has to follow the
 * initiator's offer.  These helpers apply the rule instead of assuming it. */
static uint32_t numMin(const char* offered, uint32_t ours)
{
    unsigned long v;
    if (offered == NULL) { return ours; }
    v = strtoul(offered, NULL, 10);
    if (v == 0) { return ours; }
    return ((uint32_t)v < ours) ? (uint32_t)v : ours;
}
static int boolOrYes(const char* offered)
{
    return offered != NULL && _stricmp(offered, "Yes") == 0;
}

static int sessLogin(SESS* s, const uint8_t* bhs, const uint8_t* data, uint32_t dlen)
{
    TPARAM kv[TP_MAX];
    int n = tpParse(data, dlen, kv, TP_MAX);
    uint8_t keys[1024];
    uint32_t klen = 0;
    uint8_t rsp[48];
    uint8_t csg = bhsLoginCsg(bhs);
    const char* v;
    int i;

    v = tpGet(kv, n, "SessionType");
    if (v != NULL && _stricmp(v, "Discovery") == 0) { s->discovery = 1; }

    say("  [conn %u] login: T=%d CSG=%u NSG=%u dsl=%u keys=%d isid=%02X%02X%02X%02X%02X%02X "
        "tsih(req)=%u cid=%u\n",
        s->id, bhsLoginT(bhs) ? 1 : 0, (unsigned)csg,
        (unsigned)bhsLoginNsg(bhs), dlen, n,
        bhs[8], bhs[9], bhs[10], bhs[11], bhs[12], bhs[13],
        (unsigned)rd16(bhs + 14), (unsigned)rd16(bhs + 20));
    for (i = 0; i < n; i++) {
        say("    offered %s=%s\n", kv[i].key, kv[i].val);
    }

    if (csg == ISCSI_STAGE_SECURITY) {
        /* Target-only keys: these are sent unasked because the initiator has no opinion
         * about them.  TargetPortalGroupTag is the one that matters - without it Windows
         * answers the security stage of a normal session and then waits for its login timer
         * to expire, while the discovery session (which needs no portal group) goes through.
         * AuthMethod=None is the whole of the authentication negotiation: there is no method
         * to offer, so there is nothing to complete. */
        tpAdd(keys, sizeof(keys), &klen, "AuthMethod", "None");
        tpAdd(keys, sizeof(keys), &klen, "TargetAlias", TARGET_ALIAS);
        tpAdd(keys, sizeof(keys), &klen, "TargetPortalGroupTag", TARGET_PORTAL_TAG);
    } else {
        /* Answer only what was offered, and lower where the rule says lower. */
        char num[16];
        if (tpHas(kv, n, "HeaderDigest")) { tpAdd(keys, sizeof(keys), &klen, "HeaderDigest", "None"); }
        if (tpHas(kv, n, "DataDigest")) { tpAdd(keys, sizeof(keys), &klen, "DataDigest", "None"); }
        if (tpHas(kv, n, "MaxConnections")) { tpAdd(keys, sizeof(keys), &klen, "MaxConnections", "1"); }

        /* InitialR2T is OR-negotiated (LIO: TYPERANGE_BOOL_OR), so Yes wins if either side
         * wants it.  Answering No against an initiator that offered No removes R2T from the
         * write path entirely for anything that fits in one first burst, which is what
         * Windows' 64 KiB write CDBs are. */
        if (tpHas(kv, n, "InitialR2T")) {
            if (boolOrYes(tpGet(kv, n, "InitialR2T"))) {
                tpAdd(keys, sizeof(keys), &klen, "InitialR2T", "Yes");
                s->unsolicited = 0;
            } else {
                tpAdd(keys, sizeof(keys), &klen, "InitialR2T", "No");
                s->unsolicited = 1;
            }
        }
        /* ImmediateData is AND-negotiated: Yes only if both sides say Yes.  No keeps the
         * first burst out of the command PDU and in separate Data-Out PDUs, one shape to
         * receive instead of two. */
        if (tpHas(kv, n, "ImmediateData")) {
            tpAdd(keys, sizeof(keys), &klen, "ImmediateData", "No");
        }
        if (tpHas(kv, n, "FirstBurstLength")) {
            s->firstBurst = numMin(tpGet(kv, n, "FirstBurstLength"), ISCSI_MAX_DSL);
            if (s->firstBurst < BLOCK_SIZE) { s->firstBurst = BLOCK_SIZE; }
            snprintf(num, sizeof(num), "%u", s->firstBurst);
            tpAdd(keys, sizeof(keys), &klen, "FirstBurstLength", num);
        }
        if (tpHas(kv, n, "MaxBurstLength")) {
            s->maxBurst = numMin(tpGet(kv, n, "MaxBurstLength"), ISCSI_MAX_DSL);
            if (s->maxBurst < s->firstBurst) { s->maxBurst = s->firstBurst; }
            snprintf(num, sizeof(num), "%u", s->maxBurst);
            tpAdd(keys, sizeof(keys), &klen, "MaxBurstLength", num);
        }
        if (tpHas(kv, n, "MaxOutstandingR2T")) { tpAdd(keys, sizeof(keys), &klen, "MaxOutstandingR2T", "1"); }
        if (tpHas(kv, n, "DataPDUInOrder")) { tpAdd(keys, sizeof(keys), &klen, "DataPDUInOrder", "Yes"); }
        if (tpHas(kv, n, "DataSequenceInOrder")) { tpAdd(keys, sizeof(keys), &klen, "DataSequenceInOrder", "Yes"); }
        if (tpHas(kv, n, "ErrorRecoveryLevel")) { tpAdd(keys, sizeof(keys), &klen, "ErrorRecoveryLevel", "0"); }
        if (tpHas(kv, n, "MaxRecvDataSegmentLength")) {
            uint32_t m = numMin(tpGet(kv, n, "MaxRecvDataSegmentLength"), ISCSI_MAX_DSL);
            m &= ~3u;
            if (m < BLOCK_SIZE) { m = ISCSI_MAX_DSL; }
            s->maxRecvDsl = m;
            snprintf(num, sizeof(num), "%u", m);
            tpAdd(keys, sizeof(keys), &klen, "MaxRecvDataSegmentLength", num);
        }
        /* DefaultTime2Wait / DefaultTime2Retain stay unanswered on purpose: they are the
         * initiator's own reinstatement timers, and answering them produced "=NotUnderstood"
         * entries in the capture this file was built from. */
    }

    /* The TSIH is 0 until the operational stage, and only then the session handle.  Returning
     * a non-zero TSIH to the first login makes the response look like the target believes
     * this is a continuation of an existing session, and Windows stops right there.
     *
     * A non-zero TSIH IN THE REQUEST is the opposite case: the initiator is putting this
     * connection into a session it already has (RFC 7143 section 6.3.1, connection
     * reinstatement), and the response has to echo that handle.  Answering 0 instead tells it
     * the session is gone, so it tears the connection down and tries again - which looks
     * exactly like a target that keeps dropping the disk. */
    {
        uint16_t reqTsih = rd16(bhs + 14);
        uint16_t tsihOut;
        if (reqTsih != 0) {
            tsihOut = reqTsih;
            s->tsih = reqTsih;
        } else if (csg == ISCSI_STAGE_SECURITY) {
            tsihOut = 0;
        } else {
            tsihOut = s->tsih;
        }
        buildLoginRsp(rsp, bhs, tsihOut, bhsITT(bhs), s->statSn, s->expCmdSn, s->maxCmdSn,
                      klen);
    }
    s->statSn++;
    s->loggedIn = 1;
    if (bhsLoginT(bhs) && bhsLoginNsg(bhs) == ISCSI_STAGE_FULL_FEATURE) {
        s->fullFeature = 1;
        say("  [conn %u] full feature phase (%s session)\n", s->id,
            s->discovery ? "a DISCOVERY" : "a NORMAL");
    }
    if (klen != 0) {
        say("  [conn %u] login response: %u bytes of keys\n", s->id, klen);
        if (g_trace) {
            uint32_t k = 0;
            while (k < klen) {
                const char* kv = (const char*)keys + k;
                trace("      -> key %s\n", kv);
                k += (uint32_t)strlen(kv) + 1u;
            }
        }
    }
    return sessSendPdu(s, rsp, keys, klen);
}

static int sessText(SESS* s, const uint8_t* bhs, const uint8_t* data, uint32_t dlen)
{
    TPARAM kv[TP_MAX];
    int n = tpParse(data, dlen, kv, TP_MAX);
    uint8_t keys[1024];
    uint32_t klen = 0;
    uint8_t rsp[48];
    const char* st = tpGet(kv, n, "SendTargets");
    char addr[128];
    int i;

    for (i = 0; i < n; i++) {
        say("  [conn %u] text: %s=%s\n", s->id, kv[i].key, kv[i].val);
    }
    if (st == NULL || _stricmp(st, "All") == 0) {
        /* Two NUL-separated keys, and the address carries ",<TPGT>".  The comma-joined
         * one-line form makes Windows answer "The iSCSI name specified contains invalid
         * characters or is too long" - it takes everything after "TargetName=" up to the NUL
         * as the name - and the two-key form without ",1" is rejected as an invalid
         * SendTargets response. */
        snprintf(addr, sizeof(addr), "%s:%u,1", s->br->listenIp, (unsigned)s->br->listenPort);
        tpAdd(keys, sizeof(keys), &klen, "TargetName", TARGET_IQN);
        tpAdd(keys, sizeof(keys), &klen, "TargetAddress", addr);
        say("  [conn %u] SendTargets -> %s @ %s\n", s->id, TARGET_IQN, addr);
    }
    buildTextRsp(rsp, bhsITT(bhs), 1, s->statSn, s->expCmdSn, s->maxCmdSn, klen);
    s->statSn++;
    return sessSendPdu(s, rsp, keys, klen);
}

/* Reject carries the BHS of the PDU it refuses as its data segment (RFC 7143 section 11.17). */
static int sessReject(SESS* s, const uint8_t* bhs, uint8_t reason)
{
    uint8_t p[48];
    bhsInit(p, ISCSI_OP_REJECT, 0x80, 48);
    p[2] = reason;
    wr32(p + 16, 0xFFFFFFFFu);
    return sessSendPdu(s, p, bhs, 48);
}

/* The session loop.  Returns 0 when the initiator logged out cleanly, -1 when the connection
 * broke.  Deferred PDUs (held while a write burst was being drained) are processed first so
 * that the order the initiator sent them in is the order they are answered in. */
static int sessRun(SESS* s)
{
    for (;;) {
        uint8_t bhs[48];
        uint8_t* data = NULL;
        uint32_t dlen = 0;
        int owned = 0;
        int r;
        uint8_t op;

        if (s->deferN > 0) {
            memcpy(bhs, s->defer[0].bhs, 48);
            data = s->defer[0].data;
            dlen = s->defer[0].len;
            if (s->deferN > 1) {
                memmove(&s->defer[0], &s->defer[1],
                        sizeof(DEFER) * (size_t)(s->deferN - 1));
            }
            s->deferN--;
            owned = 1;
            r = IO_OK;
        } else {
            r = iscsiRecvPdu(s->sock, bhs, &s->rx, &s->rxCap, &dlen);
            data = s->rx;
        }
        if (r == IO_IDLE) { continue; }
        if (r != IO_OK) {
            if (owned) { free(data); }
            say("  [conn %u] initiator closed the connection (r=%d)\n", s->id, r);
            return -1;
        }

        op = bhsOpcode(bhs);
        trace("    <- op=0x%02X f=0x%02X itt=0x%08X cmdsn=%u expstatsn=%u dsl=%u%s\n",
              (unsigned)op, (unsigned)bhs[1], bhsITT(bhs), bhsCmdSN(bhs), bhsExpStatSN(bhs),
              dlen, owned ? " (deferred)" : "");
        switch (op) {
        case ISCSI_OP_LOGIN_REQ:
            if (sessLogin(s, bhs, data, dlen) != 0) { if (owned) { free(data); } return -1; }
            break;

        case ISCSI_OP_TEXT_REQ:
            if (sessText(s, bhs, data, dlen) != 0) { if (owned) { free(data); } return -1; }
            break;

        case ISCSI_OP_NOP_OUT: {
            uint8_t p[48];
            buildNopIn(p, bhsITT(bhs), bhsTTT(bhs), s->statSn, s->expCmdSn, s->maxCmdSn);
            s->statSn++;
            if (sessSendPdu(s, p, NULL, 0) != 0) { if (owned) { free(data); } return -1; }
            break;
        }

        case ISCSI_OP_SCSI_CMD:
            if (!s->loggedIn) {
                if (sessReject(s, bhs, 0x02) != 0) { if (owned) { free(data); } return -1; }
                break;
            }
            if (sessScsi(s, bhs, data, dlen) != 0) { if (owned) { free(data); } return -1; }
            break;

        case ISCSI_OP_TASK_MGMT: {
            uint8_t p[48];
            buildTaskMgmtRsp(p, bhsITT(bhs), 0x00, s->statSn, s->expCmdSn, s->maxCmdSn);
            s->statSn++;
            if (sessSendPdu(s, p, NULL, 0) != 0) { if (owned) { free(data); } return -1; }
            break;
        }

        case ISCSI_OP_LOGOUT_REQ: {
            uint8_t p[48];
            buildLogoutRsp(p, bhsITT(bhs), s->statSn, s->expCmdSn, s->maxCmdSn);
            s->statSn++;
            say("  [conn %u] logout answered\n", s->id);
            if (sessSendPdu(s, p, NULL, 0) != 0) { if (owned) { free(data); } return 0; }
            if (owned) { free(data); }
            return 0;
        }

        case ISCSI_OP_DATA_OUT:
            say("  [conn %u] unsolicited Data-Out outside any write command, ignored\n", s->id);
            break;

        default:
            say("  [conn %u] unsupported opcode 0x%02X -> Reject\n", s->id, (unsigned)op);
            if (sessReject(s, bhs, 0x05) != 0) { if (owned) { free(data); } return -1; }
            break;
        }
        if (owned) { free(data); }
    }
}

/* ==================================================================================== */
/* The listener                                                                         */
/* ==================================================================================== */

static volatile LONG g_connSeed = 1;

typedef struct {
    SOCKET  sock;
    unsigned id;
    BRIDGE* br;
} CONNARG;

static DWORD WINAPI connThread(LPVOID param)
{
    CONNARG* a = (CONNARG*)param;
    SESS* s = (SESS*)calloc(1, sizeof(SESS));
    if (s == NULL) { closesocket(a->sock); free(a); return 1; }

    s->sock = a->sock;
    s->id = a->id;
    s->br = a->br;
    /* StatSN must never be 0 on the wire: 0 is the "not set" value. */
    s->statSn = ((uint32_t)InterlockedIncrement(&g_connSeed) * 131u) | 1u;
    s->expCmdSn = 1;
    s->maxCmdSn = 1;
    s->maxRecvDsl = ISCSI_MAX_DSL;
    s->firstBurst = ISCSI_MAX_DSL;
    s->maxBurst = ISCSI_MAX_DSL;
    s->unsolicited = 1;
    s->tsih = 1;
    setRecvTimeout(s->sock, 30000);
    setSendTimeout(s->sock, 30000);

    say("[conn %u] accepted\n", s->id);
    (void)sessRun(s);
    say("[conn %u] closed after %llu command(s): %llu NVMe Read, %llu NVMe Write, "
        "%llu B read, %llu B written\n",
        s->id, s->cmdCount, s->nvmeReads, s->nvmeWrites, s->bytesRead, s->bytesWritten);

    nvmClose(&s->nvm);
    closesocket(s->sock);
    {
        int i;
        for (i = 0; i < s->deferN; i++) { free(s->defer[i].data); }
    }
    free(s->rx);
    free(s);
    free(a);
    return 0;
}

/* One probe of the backend before the first iSCSI connection: if the NVMe/TCP target is not
 * there, saying so now is better than answering Windows' first READ with an I/O error. */
static int backendProbe(BRIDGE* br)
{
    NVM h;
    uint8_t buf[4096];
    unsigned len = 0;
    uint8_t sc = 0xFF;

    if (nvmOpen(&h, br->targetIp, br->targetPort) != 0) {
        say("  [nvme] cannot reach %s:%u - start the NVMe/TCP target first\n",
            br->targetIp, (unsigned)br->targetPort);
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    if (nvmCmd(&h, NVME_OP_IDENTIFY, 0, NVME_CNS_CONTROLLER, 0, NULL, 0, buf, sizeof(buf),
               &len, &sc) == 0 && sc == NVME_SC_SUCCESS && len == 4096) {
        char sn[21], mn[41];
        memcpy(sn, buf + 4, 20);  sn[20] = 0;
        memcpy(mn, buf + 24, 40); mn[40] = 0;
        say("  [nvme] Identify Controller: SN=%s MN=%s\n", sn, mn);
    } else {
        say("  [nvme] Identify Controller failed (sc=0x%02X len=%u)\n", (unsigned)sc, len);
    }
    memset(buf, 0, sizeof(buf));
    len = 0;
    if (nvmCmd(&h, NVME_OP_IDENTIFY, NVME_NSID, NVME_CNS_NAMESPACE, 0, NULL, 0, buf,
               sizeof(buf), &len, &sc) == 0 && sc == NVME_SC_SUCCESS && len == 4096) {
        uint64_t nsze = 0;
        memcpy(&nsze, buf, 8);
        say("  [nvme] Identify Namespace 1: nsze=%llu blocks (%llu KiB)\n",
            (unsigned long long)nsze, (unsigned long long)(nsze * BLOCK_SIZE / 1024));
        if (br->lbaBase + br->nblocks > nsze) {
            say("  [nvme] WARNING: the LUN window %u+%u runs past the namespace (%llu blocks)\n",
                br->lbaBase, br->nblocks, (unsigned long long)nsze);
        }
    } else {
        say("  [nvme] Identify Namespace failed (sc=0x%02X len=%u)\n", (unsigned)sc, len);
    }
    nvmClose(&h);
    return 0;
}

static int runBridge(BRIDGE* br)
{
    SOCKET l;
    uint16_t bound = 0;
    unsigned served = 0;

    say("=== iSCSI -> NVMe/TCP bridge\n");
    say("    listening on %s:%u, backend %s:%u\n",
        br->listenIp, (unsigned)br->listenPort, br->targetIp, (unsigned)br->targetPort);
    say("    LUN 0 = %u blocks x %u = %u KiB, mapped to NVMe LBA %u\n",
        br->nblocks, BLOCK_SIZE, br->nblocks * BLOCK_SIZE / 1024, br->lbaBase);
    (void)backendProbe(br);

    l = listenLoopback(br->listenPort, &bound);
    if (l == INVALID_SOCKET) {
        say("bind/listen on %s:%u failed (%d)\n", br->listenIp, (unsigned)br->listenPort,
            WSAGetLastError());
        return 1;
    }
    say("[listener armed] %s:%u is accepting connections\n", br->listenIp, (unsigned)bound);

    for (;;) {
        SOCKET c = accept(l, NULL, NULL);
        CONNARG* a;
        HANDLE t;
        if (c == INVALID_SOCKET) { break; }
        setSocketBuffers(c);
        a = (CONNARG*)calloc(1, sizeof(*a));
        if (a == NULL) { closesocket(c); continue; }
        a->sock = c;
        a->id = served++;
        a->br = br;
        t = CreateThread(NULL, 0, connThread, a, 0, NULL);
        if (t != NULL) { CloseHandle(t); }
        else { closesocket(c); free(a); }
    }
    closesocket(l);
    return 0;
}

/* ==================================================================================== */
/* Child processes (self-test only)                                                      */
/* ==================================================================================== */

typedef struct {
    PROCESS_INFORMATION pi;
    HANDLE log;
    char   logPath[MAX_PATH];
    int    started;
} CHILD;

static void exeSibling(const char* name, char* out, size_t cap)
{
    char self[MAX_PATH];
    char* slash;
    DWORD n = GetModuleFileNameA(NULL, self, (DWORD)sizeof(self));
    if (n == 0 || n >= sizeof(self)) { snprintf(out, cap, "%s", name); return; }
    slash = strrchr(self, '\\');
    if (slash != NULL) { *(slash + 1) = 0; }
    snprintf(out, cap, "%s%s", self, name);
}

/* Start a child with its stdout and stderr redirected to a FILE, never a pipe.  A child that
 * survives holds an inherited pipe open and hangs the wrapper that waits on it; a file has
 * no such failure mode, and it can be read while the child runs. */
static int childStart(const char* exe, const char* args, const char* logPath, CHILD* c)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    char cmd[1024];
    HANDLE log;

    memset(c, 0, sizeof(*c));
    snprintf(c->logPath, sizeof(c->logPath), "%s", logPath);
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    log = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) {
        say("  [child] cannot create %s\n", logPath);
        return -1;
    }
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = INVALID_HANDLE_VALUE;
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &c->pi)) {
        say("  [child] CreateProcess failed (%lu) for: %s\n", GetLastError(), cmd);
        CloseHandle(log);
        return -2;
    }
    c->log = log;
    c->started = 1;
    return 0;
}

static void childStop(CHILD* c)
{
    if (!c->started) { return; }
    TerminateProcess(c->pi.hProcess, 0);
    WaitForSingleObject(c->pi.hProcess, 3000);
    CloseHandle(c->pi.hProcess);
    CloseHandle(c->pi.hThread);
    if (c->log != INVALID_HANDLE_VALUE) { CloseHandle(c->log); }
    c->started = 0;
}

/* Wait until the port accepts a connection: a listener prints "[listener armed]" only after
 * bind+listen, so a successful connect is the same fact observed from outside. */
static int waitPort(uint16_t port, int ms)
{
    int waited = 0;
    while (waited < ms) {
        SOCKET s = connectLoopback(port);
        if (s != INVALID_SOCKET) { closesocket(s); return 0; }
        Sleep(100);
        waited += 100;
    }
    return -1;
}

/* ==================================================================================== */
/* Self-test 1: PDU framing, padding included                                           */
/* ==================================================================================== */

static void testFraming(void)
{
    uint16_t port = 0;
    SOCKET ls, a, b;
    uint8_t bhs[48];
    uint8_t cdb[16];
    uint8_t payload[13];
    uint8_t small[7];
    uint8_t rsp[48];
    uint8_t* data = NULL;
    uint32_t cap = 0, dlen = 0;
    int i, r;

    printf("\n== 1. iSCSI PDU framing: 48-byte BHS, data segment, 4-byte padding ==\n");

    ok(pad4(0) == 0 && pad4(1) == 3 && pad4(2) == 2 && pad4(3) == 1 && pad4(4) == 0 &&
       pad4(5) == 3 && pad4(7) == 1,
       "pad4 gives 0,3,2,1,0,3,1 for lengths 0,1,2,3,4,5,7");

    ls = listenLoopback(0, &port);
    ok(ls != INVALID_SOCKET && port != 0, "a loopback listener is up for the framing test");
    if (ls == INVALID_SOCKET) { return; }
    b = connectLoopback(port);
    ok(b != INVALID_SOCKET, "the receiving end connects");
    a = accept(ls, NULL, NULL);
    ok(a != INVALID_SOCKET, "the sending end accepts");
    if (a == INVALID_SOCKET || b == INVALID_SOCKET) { closesocket(ls); return; }
    setRecvTimeout(b, 2000);
    setRecvTimeout(a, 2000);

    /* PDU A: a SCSI Command whose data segment is 13 bytes - deliberately not a multiple of
     * four, because that is the length that decides whether the padding rule was applied. */
    for (i = 0; i < 13; i++) { payload[i] = (uint8_t)(0xA0 + i); }
    memset(bhs, 0, sizeof(bhs));
    bhs[0] = 0x01;                    /* SCSI Command, F and R clear so they can be set below */
    bhs[1] = (uint8_t)(0x80 | 0x40 | 0x02);  /* F=1, R=1, attr=2 (ordered) */
    wr24(bhs + 5, 13);                /* DataSegmentLength: the point of the test */
    /* LUN 0 in the 8 bytes at +8 (left zero) */
    wr32(bhs + 16, 0x11223344u);      /* InitiatorTaskTag */
    wr32(bhs + 20, 0x00020000u);      /* ExpectedDataTransferLength = 128 KiB */
    wr32(bhs + 24, 7u);               /* CmdSN */
    wr32(bhs + 28, 9u);               /* ExpStatSN */
    cdb[0] = 0x28; cdb[1] = 0x00; cdb[2] = 0x00; cdb[3] = 0x00;
    cdb[4] = 0x00; cdb[5] = 0x00; cdb[6] = 0x00; cdb[7] = 0x80;
    cdb[8] = 0; cdb[9] = 0; cdb[10] = 0; cdb[11] = 0; cdb[12] = 0; cdb[13] = 0;
    cdb[14] = 0; cdb[15] = 0;
    memcpy(bhs + 32, cdb, 16);

    ok(iscsiSendPdu(a, bhs, payload, 13) == 0, "a 13-byte SCSI Command PDU is sent");
    /* PDU B: a Text Request with a 7-byte data segment, queued behind it.  If the 3 bytes of
     * padding after PDU A were not written, this PDU starts 3 bytes early. */
    memset(small, 0, sizeof(small));
    memcpy(small, "St=Al", 5);
    memset(rsp, 0, sizeof(rsp));
    rsp[0] = 0x44;                    /* Text Request, immediate */
    rsp[1] = 0x80;                    /* F */
    wr24(rsp + 5, 7);
    wr32(rsp + 16, 0x55667788u);      /* InitiatorTaskTag */
    wr32(rsp + 20, 0xFFFFFFFFu);      /* TargetTransferTag */
    wr32(rsp + 24, 8u);               /* CmdSN */
    wr32(rsp + 28, 10u);              /* ExpStatSN */
    ok(iscsiSendPdu(a, rsp, small, 7) == 0, "a 7-byte Text Request is sent behind it");
    /* PDU C: a NOP-In with no data segment. */
    memset(rsp, 0, sizeof(rsp));
    rsp[0] = 0x20;
    wr32(rsp + 16, 0xDEADBEEFu);
    ok(iscsiSendPdu(a, rsp, NULL, 0) == 0, "a NOP-In with no data segment is sent last");

    /* ---- parse it all back with the production receive path ---- */
    r = iscsiRecvPdu(b, bhs, &data, &cap, &dlen);
    ok(r == IO_OK && dlen == 13, "the first PDU arrives with a 13-byte data segment");
    ok(bhsOpcode(bhs) == ISCSI_OP_SCSI_CMD, "opcode is SCSI Command (0x01)");
    ok(bhs[1] == 0xC2, "byte 1 carries F=1, R=1 and attribute 2");
    ok(bhs[4] == 0, "TotalAHSLength is 0 (no additional header)");
    ok(rd32(bhs + 16) == 0x11223344u, "InitiatorTaskTag survives at +16");
    ok(rd32(bhs + 20) == 0x00020000u, "ExpectedDataTransferLength survives at +20");
    ok(rd32(bhs + 24) == 7u, "CmdSN survives at +24");
    ok(rd32(bhs + 28) == 9u, "ExpStatSN survives at +28");
    ok(rd32(bhs + 8) == 0 && rd32(bhs + 12) == 0, "LUN is 0 at +8");
    ok(memcmp(bhs + 32, cdb, 16) == 0, "the 16-byte CDB comes back byte for byte");
    ok(data != NULL && memcmp(data, payload, 13) == 0,
       "the 13-byte data segment comes back byte for byte");

    /* The real proof of the padding rule: the next PDU must start where it should. */
    r = iscsiRecvPdu(b, bhs, &data, &cap, &dlen);
    ok(r == IO_OK && bhsOpcode(bhs) == ISCSI_OP_TEXT_REQ,
       "the PDU behind it parses as Text Request (so the 3 bytes of padding were consumed)");
    ok(dlen == 7 && memcmp(data, small, 7) == 0, "its 7-byte data segment is intact");
    ok(rd32(bhs + 16) == 0x55667788u, "its InitiatorTaskTag is intact");
    r = iscsiRecvPdu(b, bhs, &data, &cap, &dlen);
    ok(r == IO_OK && bhsOpcode(bhs) == ISCSI_OP_NOP_IN,
       "the third PDU parses as NOP-In (so the 1 byte of padding after 7 was consumed)");
    ok(rd32(bhs + 16) == 0xDEADBEEFu, "its InitiatorTaskTag is intact");
    /* Nothing may be left over: exactly 3 and 1 bytes of padding were written. */
    r = iscsiRecvPdu(b, bhs, &data, &cap, &dlen);
    ok(r == IO_IDLE, "nothing else is on the wire (padding was exactly 3 and 1 bytes)");

    /* And a response built by the server side, so both directions are pinned. */
    buildScsiRsp(bhs, 0x11223344u, SCSI_STATUS_CHECK_COND, 0, 77u, 8u, 16u, 0u, 0u, 18u);
    {
        uint8_t sense[18];
        uint8_t seg[2 + 18];
        uint32_t sl = buildSense(sense, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0);
        wr16(seg, (uint16_t)sl);
        memcpy(seg + 2, sense, sl);
        ok(iscsiSendPdu(a, bhs, seg, 2 + sl) == 0, "a SCSI Response with sense is sent");
        r = iscsiRecvPdu(b, bhs, &data, &cap, &dlen);
        ok(r == IO_OK && bhsOpcode(bhs) == ISCSI_OP_SCSI_RSP, "it parses as SCSI Response");
        ok(bhs[3] == SCSI_STATUS_CHECK_COND, "the status is in byte 3");
        ok(bhs[2] == 0x00, "the response code is in byte 2");
        ok(bhs[1] == 0x80, "the F bit is set");
        ok(rd32(bhs + 44) == 0u, "the residual field at +44 is 0");
        ok(dlen == 20, "the data segment is SenseLength + 18");
        ok(rd16(data) == 18, "SenseLength says 18");
        ok(data[2 + 2] == SENSE_ILLEGAL_REQUEST, "the sense key rides in the data segment");
    }

    closesocket(a);
    closesocket(b);
    closesocket(ls);
    free(data);
}

/* ==================================================================================== */
/* Self-test 2: the NVMe/TCP backend                                                    */
/* ==================================================================================== */

static void testBackend(void)
{
    char nsPath[MAX_PATH], logPath[MAX_PATH];
    char exe[MAX_PATH];
    char args[1024];
    uint16_t port;
    CHILD child;
    SESS* s;
    BRIDGE br;
    uint8_t* pattern;
    uint8_t* back;
    uint8_t buf[4096];
    unsigned len = 0;
    uint8_t sc = 0xFF;
    uint64_t nsze = 0;
    int i;

    printf("\n== 2. backend: 4096 bytes written to LBA 2 over NVMe/TCP and read back ==\n");

    if (GetTempPathA((DWORD)sizeof(nsPath), nsPath) == 0) { snprintf(nsPath, sizeof(nsPath), ".\\"); }
    snprintf(logPath, sizeof(logPath), "%siscsi_bridge_nvme.log", nsPath);
    snprintf(nsPath + strlen(nsPath), sizeof(nsPath) - strlen(nsPath), "iscsi_bridge_ns.img");

    /* Prefer the side-by-side big-IO target build when it exists: the live
     * nvmeof_tcp.exe is locked by the process serving Z:, so the out-of-capsule
     * path is tested against nvmeof_tcp_bigio.exe until those binaries merge. */
    exeSibling("nvmeof_tcp_bigio.exe", exe, sizeof(exe));
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        exeSibling("nvmeof_tcp.exe", exe, sizeof(exe));
    }
    port = freePort();
    ok(port != 0, "a free TCP port was found for the NVMe/TCP target");
    if (port == 0) { return; }
    /* 16 MiB namespace, so the read-back cannot accidentally hit the end of the file. */
    snprintf(args, sizeof(args), "-target 127.0.0.1 %u -nsfile \"%s\" -blocks %u -serve",
             (unsigned)port, nsPath, 8192u);
    ok(childStart(exe, args, logPath, &child) == 0, "nvmeof_tcp.exe -target started as a child");
    if (!child.started) { return; }
    ok(waitPort(port, 15000) == 0, "the NVMe/TCP target is accepting connections");

    s = (SESS*)calloc(1, sizeof(SESS));
    pattern = (uint8_t*)malloc(4096);
    back = (uint8_t*)calloc(1, 4096);
    if (s == NULL || pattern == NULL || back == NULL) {
        ok(0, "memory for the backend test");
        childStop(&child);
        return;
    }
    memset(&br, 0, sizeof(br));
    snprintf(br.targetIp, sizeof(br.targetIp), "127.0.0.1");
    br.targetPort = port;
    br.nblocks = 8192;
    s->br = &br;
    s->id = 0;
    s->maxRecvDsl = ISCSI_MAX_DSL;

    ok(nvmEnsure(s) == 0, "the NVMe/TCP client connects and completes the ICReq/ICResp handshake");

    memset(buf, 0, sizeof(buf));
    ok(nvmCmd(&s->nvm, NVME_OP_IDENTIFY, 0, NVME_CNS_CONTROLLER, 0, NULL, 0, buf, sizeof(buf),
              &len, &sc) == 0 && sc == NVME_SC_SUCCESS && len == 4096,
       "Identify Controller returns 4096 bytes with status 0");
    memset(buf, 0, sizeof(buf));
    len = 0;
    sc = 0xFF;
    ok(nvmCmd(&s->nvm, NVME_OP_IDENTIFY, NVME_NSID, NVME_CNS_NAMESPACE, 0, NULL, 0, buf,
              sizeof(buf), &len, &sc) == 0 && sc == NVME_SC_SUCCESS && len == 4096,
       "Identify Namespace returns 4096 bytes with status 0");
    memcpy(&nsze, buf, 8);
    ok(nsze == 8192u, "the namespace reports the 8192 blocks it was given");

    for (i = 0; i < 4096; i++) { pattern[i] = (uint8_t)((i * 37 + 11) & 0xFF); }
    ok(nvmWriteRange(s, 2, 8, pattern) == 0,
       "eight 512-byte blocks written to LBA 2 as one 4096-byte NVMe Write");
    ok(s->nvmeWrites == 1, "it took exactly one NVMe Write command");
    ok(nvmReadRange(s, 2, 8, back) == 0, "the same range read back as one NVMe Read");
    ok(s->nvmeReads == 1, "it took exactly one NVMe Read command");
    ok(memcmp(pattern, back, 4096) == 0, "the 4096 bytes come back byte for byte");

    /* And a range that crosses the 4096-byte split: 12 blocks = 8 + 4. */
    for (i = 0; i < 4096; i++) { pattern[i] = (uint8_t)((i * 91 + 5) & 0xFF); }
    ok(nvmWriteRange(s, 16, 8, pattern) == 0, "a second 4096-byte pattern is written to LBA 16");
    {
        uint8_t* tail = (uint8_t*)malloc(2048);
        uint8_t* tailBack = (uint8_t*)calloc(1, 2048);
        if (tail != NULL && tailBack != NULL) {
            for (i = 0; i < 2048; i++) { tail[i] = (uint8_t)(0x5A ^ (i & 0xFF)); }
            ok(nvmWriteRange(s, 24, 4, tail) == 0, "2048 bytes are written to LBA 24");
            ok(nvmReadRange(s, 24, 4, tailBack) == 0, "and read back");
            ok(memcmp(tail, tailBack, 2048) == 0, "the 2048 bytes come back byte for byte");
        } else {
            ok(0, "memory for the second pattern");
        }
        free(tail);
        free(tailBack);
    }

    /* 64 KiB in one NVMe command.  The data no longer fits in a capsule, so it moves as
     * H2CData (write) and C2HData (read) PDUs - and it must take exactly one command
     * instead of the sixteen 4 KiB commands this backend used to issue. */
    {
        uint32_t bigBytes = NVME_CHUNK_BLOCKS * BLOCK_SIZE;
        uint8_t* bigPat = (uint8_t*)malloc(bigBytes);
        uint8_t* bigBack = (uint8_t*)calloc(1, bigBytes);
        unsigned long long beforeW = s->nvmeWrites;
        unsigned long long beforeR = s->nvmeReads;
        if (bigPat != NULL && bigBack != NULL) {
            for (i = 0; i < (int)bigBytes; i++) { bigPat[i] = (uint8_t)((i * 13 + 7) & 0xFF); }
            ok(nvmWriteRange(s, 128, NVME_CHUNK_BLOCKS, bigPat) == 0,
               "64 KiB written as one NVMe Write, data pushed as H2CData PDUs");
            ok(s->nvmeWrites - beforeW == 1ull, "it took exactly one NVMe Write command");
            ok(nvmReadRange(s, 128, NVME_CHUNK_BLOCKS, bigBack) == 0,
               "the same 64 KiB read back as one NVMe Read, data returned as C2HData PDUs");
            ok(s->nvmeReads - beforeR == 1ull, "it took exactly one NVMe Read command");
            ok(memcmp(bigPat, bigBack, bigBytes) == 0, "all 65536 bytes come back byte for byte");
        } else {
            ok(0, "memory for the 64 KiB pattern");
        }
        free(bigPat);
        free(bigBack);
    }

    /* An LBA the namespace does not have must fail cleanly, not silently succeed. */
    ok(nvmReadRange(s, 8190, 8, back) != 0, "a read past the end of the namespace is refused");

    nvmClose(&s->nvm);
    free(pattern);
    free(back);
    free(s);
    childStop(&child);
    DeleteFileA(nsPath);
    say("  (child log: %s)\n", logPath);
}

/* ==================================================================================== */
/* Self-test 3: a tiny iSCSI initiator against our own target                           */
/*                                                                                      */
/* This is the fallback the brief allows for, and the fastest way to find out whether the */
/* target works before Windows has an opinion: it drives the same code path Windows will, */
/* with login, SendTargets, READ CAPACITY, READ(10), WRITE(10) and a read-back.          */
/* ==================================================================================== */

typedef struct {
    SOCKET   sock;
    uint8_t* rx;
    uint32_t rxCap;
    uint32_t itt;
    uint32_t cmdSn;
    uint32_t statSn;      /* the StatSN the target should use next */
    uint16_t tsih;
} CLI;

static int cliConnect(CLI* c, uint16_t port)
{
    memset(c, 0, sizeof(*c));
    c->sock = connectLoopback(port);
    if (c->sock == INVALID_SOCKET) { return -1; }
    c->statSn = 1;
    return 0;
}

static void cliClose(CLI* c)
{
    if (c->sock != INVALID_SOCKET) { closesocket(c->sock); }
    c->sock = INVALID_SOCKET;
    free(c->rx);
    c->rx = NULL;
    c->rxCap = 0;
}

/* Read one PDU into the client's reusable buffer.  The payload is at c->rx, which the next
 * call reuses - so a caller that needs the bytes must consume them before reading again. */
static int cliRecv(CLI* c, uint8_t* bhs, uint32_t* dlen)
{
    return iscsiRecvPdu(c->sock, bhs, &c->rx, &c->rxCap, dlen) == IO_OK ? 0 : -1;
}

/* Build and send a Login Request, then read the response and report its transition. */
static int cliLogin(CLI* c, int t, int csg, int nsg, const char* text, uint8_t* cls,
                    uint8_t* detail, int* rspT, int* rspCsg, int* rspNsg)
{
    uint8_t p[48];
    uint32_t dlen = 0;
    uint32_t textLen = (uint32_t)strlen(text);

    memset(p, 0, sizeof(p));
    p[0] = (uint8_t)(ISCSI_OP_LOGIN_REQ | 0x40);           /* immediate */
    p[1] = (uint8_t)((t ? 0x80 : 0) | ((csg & 3) << 2) | (nsg & 3));
    wr24(p + 5, textLen);
    p[8] = 0x40; p[9] = 0x00; p[10] = 0x01; p[11] = 0x37;  /* ISID: an OUI-shaped value */
    p[12] = 0x00; p[13] = 0x01;
    wr16(p + 14, c->tsih);
    c->itt++;
    wr32(p + 16, c->itt);
    wr16(p + 20, 1);                                       /* CID */
    wr32(p + 24, ++c->cmdSn);
    wr32(p + 28, c->statSn);
    if (iscsiSendPdu(c->sock, p, text, textLen) != 0) { return -1; }
    if (cliRecv(c, p, &dlen) != 0) { return -2; }
    if (bhsOpcode(p) != ISCSI_OP_LOGIN_RSP) { return -3; }
    if (bhsITT(p) != c->itt) { return -4; }
    if (cls != NULL) { *cls = p[36]; }
    if (detail != NULL) { *detail = p[37]; }
    if (rspT != NULL) { *rspT = bhsLoginT(p) ? 1 : 0; }
    if (rspCsg != NULL) { *rspCsg = bhsLoginCsg(p); }
    if (rspNsg != NULL) { *rspNsg = bhsLoginNsg(p); }
    c->statSn = bhsStatSN(p) + 1u;
    c->tsih = rd16(p + 14);
    return 0;
}

/* A bounded substring search: the text answer is a list of NUL-terminated keys, so strstr()
 * would stop at the first NUL and never see the second key. */
static int textHas(const char* buf, uint32_t len, const char* needle)
{
    uint32_t i, n = (uint32_t)strlen(needle);
    if (n == 0) { return 1; }
    for (i = 0; i + n <= len; i++) {
        if (memcmp(buf + i, needle, n) == 0) { return 1; }
    }
    return 0;
}

static int cliText(CLI* c, const char* text, char* out, uint32_t outCap, uint32_t* outLen)
{
    uint8_t p[48];
    uint32_t dlen = 0;
    uint32_t textLen = (uint32_t)strlen(text);

    memset(p, 0, sizeof(p));
    p[0] = (uint8_t)(ISCSI_OP_TEXT_REQ | 0x40);
    p[1] = 0x80;                                           /* F: final */
    wr24(p + 5, textLen);
    c->itt++;
    wr32(p + 16, c->itt);
    wr32(p + 20, 0xFFFFFFFFu);
    wr32(p + 24, ++c->cmdSn);
    wr32(p + 28, c->statSn);
    if (iscsiSendPdu(c->sock, p, text, textLen) != 0) { return -1; }
    if (cliRecv(c, p, &dlen) != 0) { return -2; }
    if (bhsOpcode(p) != ISCSI_OP_TEXT_RSP) { return -3; }
    if (!bhsFinal(p)) { return -4; }
    if (out != NULL && outCap != 0) {
        uint32_t n = (dlen < outCap - 1u) ? dlen : outCap - 1u;
        if (n != 0) { memcpy(out, c->rx, n); }
        out[n] = 0;
        if (outLen != NULL) { *outLen = n; }
    }
    c->statSn = bhsStatSN(p) + 1u;
    return 0;
}

/* Facts about the last command's Data-In stream, so the self-test can check the framing rather
 * than just the bytes: the offset of every PDU must continue where the previous one ended, and
 * the F bit must appear exactly once, on the last PDU. */
static uint32_t g_cliDataInPdus = 0;
static int      g_cliDataInFramingOk = 1;

/* Send one SCSI Command, optionally followed by its data as Data-Out PDUs, and collect the
 * Data-In payload and the final status. */
static int cliScsi(CLI* c, const uint8_t* cdb, uint32_t edtl, const uint8_t* wr, uint32_t wrLen,
                   uint8_t* out, uint32_t outCap, uint32_t* outLen, uint8_t* status,
                   uint8_t* sense, uint32_t* senseLen)
{
    uint8_t p[48];
    uint32_t dlen = 0;
    uint32_t got = 0;
    uint32_t itt;
    uint32_t expectOff = 0;
    int sawFinal = 0;

    g_cliDataInPdus = 0;
    g_cliDataInFramingOk = 1;

    memset(p, 0, sizeof(p));
    p[0] = ISCSI_OP_SCSI_CMD;
    p[1] = (uint8_t)(0x80 | (wr == NULL ? 0x40 : 0x00) | 0x02);  /* F, R for reads, attr 2 */
    c->itt++;
    itt = c->itt;
    wr32(p + 16, itt);
    wr32(p + 20, edtl);
    wr32(p + 24, ++c->cmdSn);
    wr32(p + 28, c->statSn);
    memcpy(p + 32, cdb, 16);
    if (iscsiSendPdu(c->sock, p, NULL, 0) != 0) { return -1; }

    if (wr != NULL && wrLen != 0) {
        /* Two Data-Out PDUs, so the target's burst accumulation and its DataSN/offset
         * handling are exercised rather than assumed. */
        uint32_t half = wrLen / 2u;
        uint32_t off = 0;
        uint32_t dataSn = 0;
        while (off < wrLen) {
            uint32_t chunk = (off == 0 && half != 0) ? half : wrLen - off;
            int final = (off + chunk == wrLen);
            memset(p, 0, sizeof(p));
            p[0] = ISCSI_OP_DATA_OUT;
            p[1] = (uint8_t)(final ? 0x80 : 0x00);
            wr24(p + 5, chunk);
            wr32(p + 16, itt);
            wr32(p + 20, 0xFFFFFFFFu);           /* unsolicited */
            wr32(p + 24, c->statSn);             /* ExpStatSN */
            wr32(p + 32, dataSn);                /* DataSN */
            wr32(p + 36, off);                   /* Buffer Offset */
            if (iscsiSendPdu(c->sock, p, wr + off, chunk) != 0) { return -2; }
            off += chunk;
            dataSn++;
        }
    }

    if (outLen != NULL) { *outLen = 0; }
    if (senseLen != NULL) { *senseLen = 0; }
    for (;;) {
        if (cliRecv(c, p, &dlen) != 0) { return -3; }
        if (bhsOpcode(p) == ISCSI_OP_DATA_IN) {
            uint32_t off = bhsBufOff(p);
            if (off != expectOff) { g_cliDataInFramingOk = 0; }
            if (sawFinal) { g_cliDataInFramingOk = 0; }
            if (bhsFinal(p)) { sawFinal = 1; }
            g_cliDataInPdus++;
            expectOff = off + dlen;
            if (out != NULL && off + dlen <= outCap) { memcpy(out + off, c->rx, dlen); }
            got += dlen;
            if (outLen != NULL) { *outLen = got; }
            continue;
        }
        if (bhsOpcode(p) == ISCSI_OP_NOP_IN) { continue; }
        if (bhsOpcode(p) == ISCSI_OP_SCSI_RSP) { break; }
        return -4;
    }
    if (bhsITT(p) != itt) { return -5; }
    if (got != 0 && !sawFinal) { g_cliDataInFramingOk = 0; }
    if (status != NULL) { *status = p[3]; }
    if (sense != NULL && senseLen != NULL && bhsDataLen(p) >= 2) {
        uint32_t n = rd16(c->rx);
        if (n > bhsDataLen(p) - 2u) { n = bhsDataLen(p) - 2u; }
        memcpy(sense, c->rx + 2, n);
        *senseLen = n;
    }
    if (bhsStatSN(p) != c->statSn) {
        say("  (client) StatSN %u, expected %u\n", bhsStatSN(p), c->statSn);
    }
    c->statSn = bhsStatSN(p) + 1u;
    return 0;
}

static int cliLogout(CLI* c)
{
    uint8_t p[48];
    uint32_t dlen = 0;

    memset(p, 0, sizeof(p));
    p[0] = (uint8_t)(ISCSI_OP_LOGOUT_REQ | 0x40);
    p[1] = 0x80;                                           /* reason: close the session */
    c->itt++;
    wr32(p + 16, c->itt);
    wr32(p + 24, ++c->cmdSn);
    wr32(p + 28, c->statSn);
    if (iscsiSendPdu(c->sock, p, NULL, 0) != 0) { return -1; }
    if (cliRecv(c, p, &dlen) != 0) { return -2; }
    return bhsOpcode(p) == ISCSI_OP_LOGOUT_RSP ? 0 : -3;
}

static void testBridgeLoopback(void)
{
    char nsPath[MAX_PATH], logPath[MAX_PATH], bridgeLog[MAX_PATH];
    char exe[MAX_PATH], args[1024];
    uint16_t nvmePort, iscsiPort;
    CHILD nvme, bridge;
    CLI c;
    uint8_t cdb[16];
    uint8_t buf[4096];
    uint8_t pattern[4096];
    uint8_t readback[4096];
    uint8_t sense[18];
    uint32_t outLen = 0, senseLen = 0;
    uint32_t textLen = 0;
    uint8_t status = 0xFF;
    uint8_t cls = 0xFF, detail = 0xFF;
    int t = 0, csg = -1, nsg = -1;
    char text[512];
    int i, rc;
    FILE* f;

    printf("\n== 3. the whole chain without Windows: a tiny iSCSI initiator -> our target -> "
           "NVMe/TCP -> nvmeof_tcp.exe ==\n");

    if (GetTempPathA((DWORD)sizeof(nsPath), nsPath) == 0) { snprintf(nsPath, sizeof(nsPath), ".\\"); }
    snprintf(logPath, sizeof(logPath), "%siscsi_bridge_bt_nvme.log", nsPath);
    snprintf(bridgeLog, sizeof(bridgeLog), "%siscsi_bridge_bt_iscsi.log", nsPath);
    snprintf(nsPath + strlen(nsPath), sizeof(nsPath) - strlen(nsPath), "iscsi_bridge_bt.img");
    DeleteFileA(nsPath);

    /* Prefer the side-by-side big-IO target build when it exists: the live
     * nvmeof_tcp.exe is locked by the process serving Z:, so the out-of-capsule
     * path is tested against nvmeof_tcp_bigio.exe until those binaries merge. */
    exeSibling("nvmeof_tcp_bigio.exe", exe, sizeof(exe));
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        exeSibling("nvmeof_tcp.exe", exe, sizeof(exe));
    }
    nvmePort = freePort();
    iscsiPort = freePort();
    ok(nvmePort != 0 && iscsiPort != 0 && nvmePort != iscsiPort,
       "two free TCP ports were found");
    if (nvmePort == 0 || iscsiPort == 0) { return; }

    snprintf(args, sizeof(args), "-target 127.0.0.1 %u -nsfile \"%s\" -blocks %u -serve",
             (unsigned)nvmePort, nsPath, 8192u);
    ok(childStart(exe, args, logPath, &nvme) == 0, "the NVMe/TCP target child started");
    if (!nvme.started) { return; }
    ok(waitPort(nvmePort, 15000) == 0, "the NVMe/TCP target is accepting connections");

    /* Like the target above: use the side-by-side build when it exists, because the
     * live iscsi_tcp_bridge.exe is locked by the process serving 3260. */
    exeSibling("iscsi_tcp_bridge_new.exe", exe, sizeof(exe));
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        exeSibling("iscsi_tcp_bridge.exe", exe, sizeof(exe));
    }
    snprintf(args, sizeof(args),
             "-listen 127.0.0.1 %u -target 127.0.0.1 %u -lun 0 %u",
             (unsigned)iscsiPort, (unsigned)nvmePort, 8192u);
    ok(childStart(exe, args, bridgeLog, &bridge) == 0, "the bridge started as a child");
    if (!bridge.started) { childStop(&nvme); return; }
    ok(waitPort(iscsiPort, 15000) == 0, "the bridge is accepting iSCSI connections on its port");

    /* ---- a discovery session: login, SendTargets, logout ---- */
    ok(cliConnect(&c, iscsiPort) == 0, "the tiny initiator connects to the iSCSI portal");
    snprintf(text, sizeof(text),
             "InitiatorName=iqn.1991-05.com.microsoft:iscsi-bridge-selftest%c"
             "SessionType=Discovery%cAuthMethod=None%cHeaderDigest=None%cDataDigest=None%c",
             0, 0, 0, 0, 0);
    rc = cliLogin(&c, 1, ISCSI_STAGE_SECURITY, ISCSI_STAGE_OPERATIONAL, text, &cls, &detail,
                  &t, &csg, &nsg);
    ok(rc == 0 && cls == 0 && detail == 0,
       "stage 1 (SecurityNegotiation) answered with status class 0 / detail 0");
    ok(t == 1 && csg == ISCSI_STAGE_SECURITY && nsg == ISCSI_STAGE_OPERATIONAL,
       "the login response mirrors T=1, CSG=0, NSG=1 (transit accepted)");
    rc = cliLogin(&c, 1, ISCSI_STAGE_OPERATIONAL, ISCSI_STAGE_FULL_FEATURE,
                  "HeaderDigest=None\0DataDigest=None\0MaxRecvDataSegmentLength=65536\0", &cls,
                  &detail, &t, &csg, &nsg);
    ok(rc == 0 && cls == 0, "stage 2 (LoginOperationalNegotiation) answered with status 0");
    ok(t == 1 && csg == ISCSI_STAGE_OPERATIONAL && nsg == ISCSI_STAGE_FULL_FEATURE,
       "the login response mirrors the transit to FullFeaturePhase");
    memset(text, 0, sizeof(text));
    rc = cliText(&c, "SendTargets=All\0", text, sizeof(text), &textLen);
    ok(rc == 0, "SendTargets is answered");
    ok(textHas(text, textLen, "TargetName=" TARGET_IQN),
       "the text answer names " TARGET_IQN);
    ok(textHas(text, textLen, "TargetAddress=127.0.0.1:"),
       "the text answer carries a reachable TargetAddress");
    ok(textHas(text, textLen, ",1"), "the TargetAddress carries portal group tag 1");
    ok(cliLogout(&c) == 0, "logout is answered");
    cliClose(&c);

    /* ---- a normal session: the command set ---- */
    ok(cliConnect(&c, iscsiPort) == 0, "the tiny initiator opens a second connection");
    snprintf(text, sizeof(text),
             "InitiatorName=iqn.1991-05.com.microsoft:iscsi-bridge-selftest%c"
             "SessionType=Normal%cAuthMethod=None%cHeaderDigest=None%cDataDigest=None%c"
             "InitialR2T=No%cImmediateData=Yes%cMaxRecvDataSegmentLength=65536%c"
             "FirstBurstLength=65536%cMaxBurstLength=262144%cMaxConnections=32%c"
             "ErrorRecoveryLevel=2%cMaxOutstandingR2T=16%c", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
             0, 0);
    rc = cliLogin(&c, 1, ISCSI_STAGE_SECURITY, ISCSI_STAGE_OPERATIONAL, text, &cls, &detail,
                  &t, &csg, &nsg);
    ok(rc == 0 && cls == 0, "normal session: security stage answered with status 0");
    rc = cliLogin(&c, 1, ISCSI_STAGE_OPERATIONAL, ISCSI_STAGE_FULL_FEATURE,
                  "HeaderDigest=None\0DataDigest=None\0InitialR2T=No\0ImmediateData=Yes\0"
                  "MaxRecvDataSegmentLength=65536\0FirstBurstLength=65536\0"
                  "MaxBurstLength=262144\0MaxConnections=32\0ErrorRecoveryLevel=2\0"
                  "MaxOutstandingR2T=16\0DataPDUInOrder=Yes\0DataSequenceInOrder=Yes\0", &cls,
                  &detail, &t, &csg, &nsg);
    ok(rc == 0 && cls == 0, "normal session: operational stage answered with status 0");
    ok(t == 1 && nsg == ISCSI_STAGE_FULL_FEATURE, "the session is in FullFeaturePhase");

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x00;                                          /* TEST UNIT READY */
    status = 0xFF;
    ok(cliScsi(&c, cdb, 0, NULL, 0, NULL, 0, &outLen, &status, NULL, NULL) == 0 &&
       status == SCSI_STATUS_GOOD, "TEST UNIT READY -> GOOD");

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12; cdb[4] = 96;                             /* INQUIRY, allocation 96 */
    status = 0xFF;
    memset(buf, 0, sizeof(buf));
    ok(cliScsi(&c, cdb, 96, NULL, 0, buf, sizeof(buf), &outLen, &status, NULL, NULL) == 0 &&
       status == SCSI_STATUS_GOOD, "INQUIRY -> GOOD");
    ok(outLen == 96, "INQUIRY returned the whole 96-byte allocation length");
    ok(memcmp(buf + 8, "NVMEOFT ", 8) == 0, "the INQUIRY vendor identification is NVMEOFT");
    ok(buf[0] == 0x00 && buf[2] == 0x05, "INQUIRY says direct-access device, SPC-3");

    memset(cdb, 0, sizeof(cdb));
    /* READ CAPACITY(10) with a ZERO CDB allocation length - which is what the real Windows
     * initiator sends, with an EDTL of 8 - and the 8-byte answer must still come back.  A
     * target that returns nothing here sizes the disk at zero and Windows resets the
     * session in a loop; that was this bridge's worst bug, so it is pinned here. */
    cdb[0] = 0x25;
    status = 0xFF;
    memset(buf, 0, sizeof(buf));
    ok(cliScsi(&c, cdb, 8, NULL, 0, buf, sizeof(buf), &outLen, &status, NULL, NULL) == 0 &&
       status == SCSI_STATUS_GOOD, "READ CAPACITY(10) -> GOOD");
    ok(outLen == 8, "READ CAPACITY(10) returned 8 bytes");
    ok(rd32(buf) == 8191u, "it reports last LBA 8191 (8192 blocks)");
    ok(rd32(buf + 4) == 512u, "it reports 512-byte blocks");

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28; cdb[7] = 0; cdb[8] = 8;                  /* READ(10) LBA 0, 8 blocks */
    status = 0xFF;
    memset(buf, 0, sizeof(buf));
    ok(cliScsi(&c, cdb, 4096, NULL, 0, buf, sizeof(buf), &outLen, &status, NULL, NULL) == 0 &&
       status == SCSI_STATUS_GOOD, "READ(10) of LBA 0 -> GOOD");
    ok(outLen == 4096, "it returned 4096 bytes");

    memset(pattern, 0, sizeof(pattern));
    for (i = 0; i < 4096; i++) { pattern[i] = (uint8_t)((i * 131 + 7) & 0xFF); }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x2A; cdb[2] = 0; cdb[3] = 0; cdb[4] = 0; cdb[5] = 3;   /* WRITE(10) LBA 3 */
    cdb[7] = 0; cdb[8] = 8;                                  /* 8 blocks */
    status = 0xFF;
    ok(cliScsi(&c, cdb, 4096, pattern, 4096, NULL, 0, &outLen, &status, NULL, NULL) == 0 &&
       status == SCSI_STATUS_GOOD, "WRITE(10) of LBA 3 -> GOOD (4096 bytes as Data-Out PDUs)");

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28; cdb[5] = 3; cdb[8] = 8;                   /* READ(10) LBA 3, 8 blocks */
    status = 0xFF;
    memset(readback, 0, sizeof(readback));
    ok(cliScsi(&c, cdb, 4096, NULL, 0, readback, sizeof(readback), &outLen, &status, NULL,
               NULL) == 0 && status == SCSI_STATUS_GOOD, "READ(10) of LBA 3 -> GOOD");
    ok(memcmp(pattern, readback, 4096) == 0,
       "the 4096 bytes read back are byte-identical to what was written");

    /* A 128 KiB transfer: larger than one Data-In PDU (64 KiB), so the target has to send two
     * and mark only the second one final.  Marking the first one final - which this bridge
     * used to do - tells the initiator the data ended 64 KiB early, the next PDU is then a
     * protocol violation, and Windows resets the connection.  The window initiator sends
     * 128 KiB and 1 MiB transfers routinely, so this is the shape that matters. */
    {
        uint32_t bigLen = 256u * BLOCK_SIZE;                  /* 131072 bytes */
        uint8_t* bigWr = (uint8_t*)malloc(bigLen);
        uint8_t* bigRd = (uint8_t*)calloc(1, bigLen);
        if (bigWr != NULL && bigRd != NULL) {
            uint32_t i2;
            for (i2 = 0; i2 < bigLen; i2++) { bigWr[i2] = (uint8_t)((i2 * 29 + 3) & 0xFF); }
            memset(cdb, 0, sizeof(cdb));
            cdb[0] = 0x2A; cdb[4] = 0x04;                     /* WRITE(10) LBA 1024 */
            cdb[7] = 0x01;                                   /* 256 blocks = 0x0100 */
            status = 0xFF;
            ok(cliScsi(&c, cdb, bigLen, bigWr, bigLen, NULL, 0, &outLen, &status, NULL,
                       NULL) == 0 && status == SCSI_STATUS_GOOD,
               "WRITE(10) of 128 KiB (256 blocks) -> GOOD");
            memset(cdb, 0, sizeof(cdb));
            cdb[0] = 0x28; cdb[4] = 0x04;                     /* READ(10) LBA 1024 */
            cdb[7] = 0x01;                                   /* 256 blocks = 0x0100 */
            status = 0xFF;
            ok(cliScsi(&c, cdb, bigLen, NULL, 0, bigRd, bigLen, &outLen, &status, NULL,
                       NULL) == 0 && status == SCSI_STATUS_GOOD,
               "READ(10) of 128 KiB (256 blocks) -> GOOD");
            ok(outLen == bigLen, "it returned all 131072 bytes");
            ok(memcmp(bigWr, bigRd, bigLen) == 0, "the 128 KiB read back byte for byte");
            ok(g_cliDataInPdus == 2, "the Data-In arrived as two 64 KiB PDUs");
            ok(g_cliDataInFramingOk == 1,
               "only the last Data-In PDU is marked final and the offsets are contiguous");
        } else {
            ok(0, "memory for the 128 KiB transfer");
        }
        free(bigWr);
        free(bigRd);
    }

    /* A refused command must not take the session with it: this is the check that a sense
     * block actually reaches the initiator. */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xC7; cdb[8] = 0;                               /* an opcode nothing implements */
    status = 0xFF;
    memset(sense, 0, sizeof(sense));
    ok(cliScsi(&c, cdb, 0, NULL, 0, NULL, 0, &outLen, &status, sense, &senseLen) == 0,
       "an unsupported CDB is answered rather than ignored");
    ok(status == SCSI_STATUS_CHECK_COND, "it comes back CHECK CONDITION");
    ok(senseLen >= 14 && sense[2] == SENSE_ILLEGAL_REQUEST, "its sense key is ILLEGAL REQUEST");
    ok(senseLen >= 14 && sense[12] == ASC_INVALID_OPCODE,
       "its ASC is 0x20 (invalid command operation code)");

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x41;                                           /* WRITE SAME(10) */
    status = 0xFF;
    memset(sense, 0, sizeof(sense));
    ok(cliScsi(&c, cdb, 0, NULL, 0, NULL, 0, &outLen, &status, sense, &senseLen) == 0 &&
       status == SCSI_STATUS_CHECK_COND, "WRITE SAME(10) is refused with CHECK CONDITION");

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35;                                           /* SYNCHRONIZE CACHE(10) */
    status = 0xFF;
    ok(cliScsi(&c, cdb, 0, NULL, 0, NULL, 0, &outLen, &status, NULL, NULL) == 0 &&
       status == SCSI_STATUS_GOOD, "SYNCHRONIZE CACHE(10) is accepted as a no-op");

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x00;                                           /* TEST UNIT READY again */
    status = 0xFF;
    ok(cliScsi(&c, cdb, 0, NULL, 0, NULL, 0, &outLen, &status, NULL, NULL) == 0 &&
       status == SCSI_STATUS_GOOD, "the session is still alive after all of that");

    ok(cliLogout(&c) == 0, "the normal session logs out cleanly");
    cliClose(&c);

    /* The bytes must be in the namespace file, not just in a buffer somewhere: this is the
     * check that separates "the SCSI read-back was served from somewhere in this process"
     * from "the bytes really went through NVMe/TCP into the backend's namespace image". */
    f = fopen(nsPath, "rb");
    ok(f != NULL, "the namespace file the NVMe/TCP target created can be read");
    if (f != NULL) {
        uint8_t fileBlock[4096];
        memset(fileBlock, 0, sizeof(fileBlock));
        if (_fseeki64(f, (__int64)3 * BLOCK_SIZE, SEEK_SET) == 0 &&
            fread(fileBlock, 1, sizeof(fileBlock), f) == sizeof(fileBlock)) {
            if (memcmp(fileBlock, pattern, 4096) != 0) {
                uint32_t k;
                for (k = 0; k < 4096; k++) { if (fileBlock[k] != pattern[k]) { break; } }
                say("  (file check) first difference at byte %u: file=%02X expected=%02X\n"
                    "  (file check) file starts %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    k, k < 4096 ? fileBlock[k] : 0, k < 4096 ? pattern[k] : 0,
                    fileBlock[0], fileBlock[1], fileBlock[2], fileBlock[3], fileBlock[4],
                    fileBlock[5], fileBlock[6], fileBlock[7]);
            }
            ok(memcmp(fileBlock, pattern, 4096) == 0,
               "the 4096 bytes are in the namespace file at offset 3 * 512");
        } else {
            ok(0, "the 4096 bytes could be read back from the namespace file");
        }
        fclose(f);
    }

    childStop(&bridge);
    childStop(&nvme);
    say("  (child logs: %s , %s)\n", bridgeLog, logPath);
    DeleteFileA(nsPath);
}

/* ==================================================================================== */
/* main                                                                                 */
/* ==================================================================================== */

static void usage(const char* prog)
{
    printf("usage:\n");
    printf("  %s -listen <ip> <port> -target <ip> <port> -lun <lba0> <nblocks>\n", prog);
    printf("  %s -selftest\n", prog);
    printf("\n");
    printf("  -listen   the iSCSI portal Windows connects to (e.g. 127.0.0.1 3260)\n");
    printf("  -target   the NVMe/TCP target (nvmeof_tcp.exe -target <ip> <port> -serve)\n");
    printf("  -lun      maps SCSI LBA 0..nblocks-1 onto NVMe LBA lba0..\n");
    printf("  -trace    log every iSCSI PDU in and out (verbose)\n");
    printf("  -selftest PDU framing, the NVMe/TCP backend, and a tiny iSCSI initiator\n");
}

int main(int argc, char** argv)
{
    WSADATA wsa;
    BRIDGE br;
    int i;
    int selftest = 0;
    int haveListen = 0, haveTarget = 0, haveLun = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup failed\n"); return 2; }
    memset(&br, 0, sizeof(br));

    if (argc >= 2 && _stricmp(argv[1], "-selftest") == 0) { selftest = 1; }

    if (!selftest) {
        for (i = 1; i < argc; i++) {
            if (_stricmp(argv[i], "-trace") == 0) {
                g_trace = 1;
            } else if (_stricmp(argv[i], "-listen") == 0 && i + 2 < argc) {
                snprintf(br.listenIp, sizeof(br.listenIp), "%s", argv[i + 1]);
                br.listenPort = (uint16_t)atoi(argv[i + 2]);
                i += 2;
                haveListen = 1;
            } else if (_stricmp(argv[i], "-target") == 0 && i + 2 < argc) {
                snprintf(br.targetIp, sizeof(br.targetIp), "%s", argv[i + 1]);
                br.targetPort = (uint16_t)atoi(argv[i + 2]);
                i += 2;
                haveTarget = 1;
            } else if (_stricmp(argv[i], "-lun") == 0 && i + 2 < argc) {
                br.lbaBase = (uint32_t)strtoul(argv[i + 1], NULL, 0);
                br.nblocks = (uint32_t)strtoul(argv[i + 2], NULL, 0);
                i += 2;
                haveLun = 1;
            } else {
                printf("unknown argument: %s\n", argv[i]);
                usage(argv[0]);
                return 2;
            }
        }
        if (!haveListen || !haveTarget || !haveLun || br.listenPort == 0 || br.targetPort == 0 ||
            br.nblocks == 0) {
            usage(argv[0]);
            return 2;
        }
    }

    if (selftest) {
        int rc;
        printf("iSCSI <-> NVMe/TCP bridge self-test\n");
        printf("===================================\n");
        testFraming();
        testBackend();
        testBridgeLoopback();
        printf("\n===================================\n");
        printf("pass %d  fail %d\n", g_pass, g_fail);
        rc = (g_fail == 0) ? 0 : 1;
        WSACleanup();
        return rc;
    }

    {
        int rc = runBridge(&br);
        WSACleanup();
        return rc;
    }
}










