// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// ===========================================================================
//  F5 - interoperability with an independent NVMe-oF/RDMA peer
//
//  Every other suite in this project runs our initiator against our target.
//  That is the weakest possible arrangement: a mistake made on both sides is
//  invisible, and this project has already shipped four constants that both
//  sides agreed on and no other implementation uses (DESIGN 8.29).
//
//  F5 is the suite that points our code at somebody else's.  It has two roles:
//
//    -initiator  our host against ANY fabrics target - Linux's nvmet, or ours.
//                The target's NQN is a parameter, and nothing about the target
//                is assumed: every value the host needs (capsule sizes, queue
//                count, namespace geometry) is read from Identify.
//
//    -target     our target, for a Linux host (`nvme-cli connect`) to drive.
//                It is deliberately tolerant and loud: it logs every admin
//                command, every I/O op and every connection, because what a
//                foreign host does first is the finding.
//
//  Usage:
//    f5_interop -initiator <serverIp> <port> <localIp> [-subnqn <nqn>]
//                [-hostnqn <nqn>] [-queue <qid>] [-blocks <n>]
//    f5_interop -target    <ip> <port> [-queues <n>]
//
//  Linux side: see linux/nvmet_setup.sh and INTEROP_F5.md.
// ===========================================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <io.h>        // _dup2 / _fileno / _open_osfhandle, for the service log redirect
#include <fcntl.h>     // _O_APPEND / _O_TEXT
#include <share.h>     // _SH_DENYNO

#include "nvmeof_wire.h"
#include "nvmeof_rdma.h"
#include "nvmeof_auth.h"
#include "nvmeof_dhchap.h"
// The iSCSI target: what lets a REAL disk (any size, no local copy) appear in
// Windows' Disk Management, because Windows' own kernel-mode iSCSI initiator can be
// the block-layer end that Windows' missing NVMe-oF initiator would have been.
#include "nvmeof_iscsi.h"

using namespace nvmeof;

// ---------------------------------------------------------------------------
//  Region layout
// ---------------------------------------------------------------------------
static const size_t kRegionBytes  = 64u * 1024 * 1024;
// Every buffer here is inside the one registered region, and the admin side is a
// RING rather than a single slot.
//
// The ring is not decoration.  A target that keeps exactly one Receive armed can
// be overtaken by a host that sends two admin commands without waiting for the
// first answer, and the capsule that arrives while no Receive is posted is not
// retried - it is simply lost (DESIGN 8.10(4), re-measured by this file's Async
// Event test, which sent five capsules back to back and had two arrive).  A real
// target posts a whole receive ring: nvmet's admin queue posts hsqsize + 1 = 32.
// Eight is what this implementation needs to survive a pipelining host, and the
// count it reports in the connect reply is the real one.
// The ring depth must be at least the depth we ADVERTISE, or a host that uses the
// whole window loses capsules: this target used to advertise crqsize = 32 while
// posting 8 Receives, so capsules 9..32 of a busy queue would arrive with nowhere
// to land - and a capsule that arrives with no Receive posted is dropped, not
// retried (DESIGN 8.10(4)).  The assert below ties the two numbers together so
// they cannot drift apart again.
static const size_t kCapSlots     = 32;
static const size_t kAdminCapOff  = 0;                              // 8 x 64 B capsules
static const size_t kAdminRespOff = kAdminCapOff + kCapSlots * 64;  // 8 x 16 B responses
// How many I/O queue pairs this target can serve.  A real host asks for one queue
// per CPU and then creates exactly as many as the answer to "Set Features Number of
// Queues" allows, so this number is the contract: it is both what the target
// answers AND how many queue pairs, capsule rings and slot tables it really owns.
// PROMISING MORE THAN YOU HAVE is how this target used to fail a real host on the
// second I/O queue (DESIGN 8.47) - which is why the answer is derived from this
// constant rather than written in the reply by hand.
static const uint16_t kMaxIoQueues = 8;

// Every I/O queue gets its OWN capsule ring and response ring.  Queue q's capsules
// start at kIoCapOff + q * kIoCapStride, and the context id of a receive encodes
// (queue, slot), so a completion can be routed back to the queue that posted it -
// two queues sharing one ring would silently overwrite each other's capsules.
static const size_t kIoCapOff     = kAdminRespOff + kCapSlots * 16;
static const size_t kIoCapStride  = kCapSlots * 64;
static const size_t kIoRespOff    = kIoCapOff + (size_t)kMaxIoQueues * kIoCapStride;
static const size_t kIoRespStride = kCapSlots * 16;
static const size_t kRespInSlots  = 32;                              // host side, one per in-flight command
static const size_t kRespInOff    = kIoRespOff + (size_t)kMaxIoQueues * kIoRespStride;
static const size_t kConnectOff   = kRespInOff + kRespInSlots * 16;
static const size_t kIdOff        = kConnectOff + NVMEOF_CONNECT_DATA_SIZE;
static const size_t kXferOff      = kIdOff + 2 * NVMEOF_IDENTIFY_SIZE;
static const size_t kNsOff        = kXferOff + 1024u * 1024;
// The namespace is the one thing a real host looks at, and until now it was a memory
// buffer this process filled with a pattern.  That is enough to prove a data path and
// useless as a demonstration: a host can connect, but it cannot keep anything.  With
// -nsfile the same region is the cache of a FILE, the geometry is the file's size,
// and FLUSH writes it back - so a Linux host can mkfs, mount, write and come back
// after a reconnect to find its files.
//
// The size is the maximum; the actual namespace is min(file size, this).
static const size_t kNsBytes      = 48u * 1024 * 1024;
static const uint32_t kBlockSize  = 512;
static const uint32_t kNsBlocks   = (uint32_t)(kNsBytes / kBlockSize);
// A page-aligned scratch buffer for buffers a PEER writes into us.  nvmet's RDMA
// Write of the identify data does not land at kIdOff (no response, buffer
// untouched, and the connection dies), so alignment/offset is one of the two
// variables worth separating: this one is 2 MiB-aligned and nothing else changes.
//
// It must ALSO sit outside the namespace.  It used to be a fixed 2 MiB, which is
// INSIDE the namespace area (which starts around 1 MiB), so the last Identify
// payload and the namespace shared memory: a host writing blocks ~2176..2303
// would have overwritten the very buffer an Identify had just been written into,
// and an Identify would have corrupted those blocks.  Nothing failed only because
// the Linux host happened to read elsewhere.  Rounding up from the end of the
// namespace keeps both properties (2 MiB alignment, no overlap).
static const size_t kScratchAlign = 2u * 1024 * 1024;
static const size_t kOutOff       = (kNsOff + kNsBytes + (kScratchAlign - 1)) & ~(kScratchAlign - 1);
static const size_t kOutBytes     = 64u * 1024;
static_assert(kOutOff >= kNsOff + kNsBytes,
              "the peer-write scratch must not overlap the namespace");
static_assert(kOutOff % kScratchAlign == 0,
              "the scratch must stay 2 MiB aligned: that is what made a peer's "
              "RDMA Write land at all");

// DH-HMAC-CHAP payload landing zone, one per direction: an Auth Send's payload is
// RDMA-Read into it, an Auth Receive's answer is built in it and RDMA-Written back.
// It is 4096 bytes because that is CHAP_BUF_SIZE - the Linux host posts every Auth
// Receive with al = 4096 and expects all 4096 back, zero padded - and it is a
// region of its own rather than a reuse of kIdOff/kConnectOff so that an auth
// exchange can never be the thing that corrupts an Identify the host is about to
// read.  (That is not hypothetical: kOutOff itself used to sit inside the
// namespace, DESIGN 8.50.)
static const size_t kAuthOff      = kOutOff + kOutBytes;
static const size_t kAuthBytes    = nvmeof_dhchap::kMaxBuf;
static_assert(kAuthOff + kAuthBytes <= kRegionBytes,
              "the auth payload buffer must be inside the registered region, or the "
              "RDMA Read/Write that fills it fails at run time, not at compile time");

// DSMs range list landing zone: the target RDMA-Reads the host's list of deallocated
// ranges into it.  A region of its own for the same reason kAuthOff is one - it is
// host-controlled data of a host-controlled length, and giving it its own bytes means
// a length that got past the checks cannot land on an Identify or on the namespace.
static const size_t kDsmBytes     = NVMEOF_DSM_MAX_RANGES * NVMEOF_DSM_RANGE_SIZE;
static const size_t kDsmOff       = kAuthOff + kAuthBytes;

// The iSCSI bridge's chunk buffer: SCSI READ/WRITE payloads land here before they
// are handed to the NVMe queue (and vice versa).  It has to be inside the registered
// region - the SGL names this address - and 64 KiB of it is what the R2T loop asks
// for at a time.  256 KiB leaves room for the text/sense payloads that share it.
// IT WAS 256 KiB, AND THAT WAS THE CEILING ON READ THROUGHPUT.  Measured with
// -iscsitime through a 4 MiB Windows read: 16 NVMe reads and 16 Data-In PDUs of
// 256 KiB, 29.1 ms total, of which the socket writes were 26.2 ms and the NVMe side
// only 2.6 ms.  The initiator had offered MaxRecvDataSegmentLength=1048576 and the
// bridge had negotiated it (-iscsichunk 1048576), but a 1 MiB PDU cannot exist while
// the staging unit is 256 KiB: PDU size is min(unit, chunk), so the buffer silently
// overrode the knob.  One Windows transfer (MaxTransferLength) is 4 MiB, so that is
// the size that makes a 4 MiB read ONE NVMe read plus four 1 MiB Data-In PDUs.
// DESIGN 8.70 has the before/after.
static const size_t kIscsiBytes   = 4u * 1024 * 1024;
static const size_t kIscsiOff     = (kDsmOff + kDsmBytes + 511) & ~(size_t)511;
static_assert(kIscsiOff + kIscsiBytes <= kRegionBytes,
              "the iSCSI chunk buffer must be inside the registered region, or every "
              "RDMA transfer that names it fails at run time");
static_assert(kDsmOff + kDsmBytes <= kRegionBytes,
              "the DSM range list must be inside the registered region");

static const uint16_t kSqSize     = 31;      // 0-based: 32 entries
static const uint16_t kSqDepth    = kSqSize + 1;
// What the target advertises as its receive queue size, and what it must therefore
// be able to hold.
static_assert(kCapSlots >= kSqDepth,
              "the capsule ring must cover the crqsize the target advertises");

#define CTX_ADMIN_CAP_BASE ((uintptr_t)0x300)   // + slot index, 0..kCapSlots-1
// I/O completions carry (queue, slot), not just slot: with eight queue pairs, a
// bare slot number would send queue 3's completion to queue 0's ring.  The ranges
// do not overlap: 0x400..0x4ff capsules, 0x500..0x5ff responses, 0x600..0x6ff data.
#define CTX_IO_CAP_BASE ((uintptr_t)0x400)
#define CTX_IO_CAP(q, slot)  ((void*)(CTX_IO_CAP_BASE + (uintptr_t)(q) * kCapSlots + (uintptr_t)(slot)))
#define CTX_IO_RESP_BASE ((uintptr_t)0x500)
#define CTX_IO_RESP(q, slot) ((void*)(CTX_IO_RESP_BASE + (uintptr_t)(q) * kCapSlots + (uintptr_t)(slot)))
#define CTX_CAP       ((void*)(uintptr_t)0xC3)  // host: capsule send completion
#define CTX_RESP      ((void*)(uintptr_t)0xC4)  // host: response received
#define CTX_HOST_RESP_BASE ((uintptr_t)0x700)  // + host slot index (pipelined test)
#define CTX_DATA_BASE ((uintptr_t)0x600)
// Note the name: CTX_DATA (below) is the admin-side data-transfer context, so the
// per-queue one must not be a function-like macro with that name - the two would
// collide and the compiler would read `CTX_DATA` as 0xD1 followed by a call.
#define CTX_IO_DATA(q, slot) ((void*)(CTX_DATA_BASE + (uintptr_t)(q) * kCapSlots + (uintptr_t)(slot)))
#define CTX_DATA      ((void*)(uintptr_t)0xD1)  // admin-side data transfers
#define CTX_CONN      ((void*)(uintptr_t)0xD2)

static int g_failures = 0;
// The DH-HMAC-CHAP host key this target was told to require, or null.  A file-scope
// global because the one place that needs it (the Connect handler, deep inside the
// admin path) has no other reason to see the command line, and threading it through
// every call would put a copy of the same string in five signatures.
static const char* g_targetAuthKey = nullptr;
static const char* g_targetCtrlKey = nullptr;
static uint8_t     g_targetDhGroup = nvmeof_auth::DHGROUP_2048;
// How long a target keeps its listener open for the NEXT controller after one goes
// away.  20 s is the value every existing suite was written against (F7 asserts that
// a target whose host disappears exits on its own); a real controller reset needs
// longer, so the interop battery raises it with -reconnectwait.
static uint32_t    g_reconnectWaitMs = 20000;
// ---- file-backed namespace (-nsfile) ----
static const char* g_nsFile = nullptr;                 // the backing file, or null
// -initiator only: move the entire namespace to/from these local files, over the
// wire.  Set by -dump <path> / -push <path>; see the loop that uses them.
static const char* g_dumpFile = nullptr;
static const char* g_pushFile = nullptr;
// -initiator only: -survey reads the GPT and the partitions' boot sectors and prints
// what the namespace is.  Read-only by construction (see surveyDisk).
static bool g_survey = false;
// -initiator only: -iscsi <tcpPort> puts an iSCSI target in front of the NVMe-oF
// namespace, so Windows' own kernel-mode iSCSI initiator can attach it as a live
// disk of any size.  See nvmeof_iscsi.h for why that is the only route to a drive
// letter without writing a kernel driver.
static int  g_iscsiPort = 0;
static bool g_iscsiReadWrite = false;      // default is READ-ONLY; -iscsirw opts in

// ---------------------------------------------------------------------------
//  Payload state and Windows service state.
//
//  These live at file scope because they are written by the argument parser in
//  main() and read again by the service thread: under the SCM the payload runs
//  inside ServiceMain, not in the scope that parsed the command line.  Keeping
//  one parser and one set of values is deliberate - a service that interprets
//  its arguments differently from the console path is a bug waiting for a
//  reboot.
// ---------------------------------------------------------------------------
struct Payload {
    const char* subnqn = "nqn.2024-01.local.rdma:windows-nd";
    const char* hostnqn = "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001";
    int         wantQueues = 1;
    uint32_t    blocks = 8;
    bool        discover = false;
    int         serveControllers = 1;      // 1 keeps every test's behaviour (see main)
    const char* initiatorAuthKey = nullptr;
    const char* initiatorCtrlKey = nullptr;
    bool        authSkip = false;
};
static Payload g_pl;

// Set when the NVMe-oF backend behind the bridge is gone for good (see
// NvmeIscsiBackend::lost).  It reaches main so the bridge can EXIT instead of
// holding a mounted disk that fails every command: running as a service, a
// non-zero exit is what makes the SCM restart it, and the -backendretry loop
// then reconnects to a backend that has come back.
static bool g_backendLost = false;

static bool          g_serviceMode = false;
static std::string   g_svcName = "nvmeofNdBridge";
static std::string   g_svcLog;
static int           g_svcArgc = 0;
static char**        g_svcArgv = nullptr;
static int           g_svcRc = 0;
static SERVICE_STATUS_HANDLE g_ssh = nullptr;
static SERVICE_STATUS        g_ss = {};
static IscsiTarget*  g_svcBridge = nullptr;
// -backendretry <seconds>: keep trying to reach the NVMe-oF peer instead of exiting.
//
// Why: installed as a service, the bridge is supposed to be there when the disk is
// wanted, and the peer (the Linux box, or a second -target run) may well come up after
// this machine does.  Without this the process exits rc=1 on a failed connect and the
// SCM restarts it every 5 s - which works, but logs a failure every 5 s and means the
// disk appears only by luck of timing.  With it the service stays Running and connects
// when the peer arrives.
static uint32_t      g_backendRetryMs = 0;
// Set by the service control handler so a stop during the retry wait is honoured:
// there is no IscsiTarget to stop() yet at that point.
static volatile LONG g_svcStopRequested = 0;
// Where the iSCSI listener binds, and what SendTargets advertises.  127.0.0.1 is the
// safe default; a real local address is for the case where the initiator will not
// complete a session with a target it considers local.  ONE address, never 0.0.0.0:
// nvmet has no read-only namespace attribute, so a LAN-wide listener would hand the
// namespace behind it to the whole network.
static const char* g_iscsiBind = "127.0.0.1";
// The foreign controller's identity, captured at Identify, reported to Windows as
// the SCSI unit serial so the disk can be traced back to the physical drive.
static char g_foreignSerial[24] = "unknown";
static char g_foreignModel[48] = "NVMe-oF namespace";
static HANDLE      g_nsHandle = INVALID_HANDLE_VALUE;
static uint32_t    g_nsBlocks = (uint32_t)(kNsBytes / kBlockSize);
static uint64_t    g_nsFlushes = 0;

// Open (or create) the backing file and fill the namespace region from it.
//
// The namespace is a SUPERSET of the file: the region is kNsBytes and the file may be
// smaller, in which case only the file's blocks are exposed - the host must see the
// geometry the backend really has, or it will write past the end of the file.
static bool nsFileOpen(Device& dev, const char* path) {
    // FILE_SHARE_READ *and* FILE_SHARE_WRITE, deliberately: the namespace file is the
    // thing a person wants to look at while the target is still serving it - "the host
    // wrote those bytes, here they are" is only checkable from outside, and a handle
    // that locks the file makes the check impossible without stopping the target.
    // Nothing in this process depends on being the only writer.
    g_nsHandle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_nsHandle == INVALID_HANDLE_VALUE) {
        printf("  nsfile: cannot open '%s' (error %lu)\n", path, GetLastError());
        return false;
    }
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(g_nsHandle, &sz)) {
        printf("  nsfile: cannot size '%s' (error %lu)\n", path, GetLastError());
        return false;
    }
    if (sz.QuadPart == 0) {
        // A new file: make it the full size so the host has somewhere to format.
        LARGE_INTEGER want = {}; want.QuadPart = (LONGLONG)kNsBytes;
        if (!SetFilePointerEx(g_nsHandle, want, nullptr, FILE_BEGIN) || !SetEndOfFile(g_nsHandle)) {
            printf("  nsfile: cannot size a new '%s' to %u bytes (error %lu)\n",
                   path, (unsigned)kNsBytes, GetLastError());
            return false;
        }
        sz.QuadPart = (LONGLONG)kNsBytes;
        printf("  nsfile: created '%s' at %u bytes\n", path, (unsigned)kNsBytes);
    }
    if (sz.QuadPart > (LONGLONG)kNsBytes) {
        printf("  nsfile: '%s' is %llu bytes, larger than the %u-byte namespace region - "
               "refusing rather than exposing a truncated disk\n",
               path, (unsigned long long)sz.QuadPart, (unsigned)kNsBytes);
        return false;
    }
    uint32_t blocks = (uint32_t)(sz.QuadPart / kBlockSize);
    if (blocks == 0) {
        printf("  nsfile: '%s' is smaller than one %u-byte block\n", path, (unsigned)kBlockSize);
        return false;
    }
    // The file is the medium, the region is the cache: read it in whole.
    //
    // Seek to 0 first: creating the file left the pointer at its END (SetEndOfFile is
    // preceded by a seek to the wanted length), so the read below would return 0
    // bytes with GetLastError() == 0 - a failure that reports success and says nothing.
    uint8_t* ns = dev.region(kNsOff);
    memset(ns, 0, kNsBytes);
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(g_nsHandle, zero, nullptr, FILE_BEGIN)) {
        printf("  nsfile: cannot rewind '%s' (error %lu)\n", path, GetLastError());
        return false;
    }
    DWORD got = 0;
    if (!ReadFile(g_nsHandle, ns, blocks * kBlockSize, &got, nullptr) || got != blocks * kBlockSize) {
        printf("  nsfile: read %lu of %u bytes from '%s' (error %lu)\n",
               (unsigned long)got, (unsigned)(blocks * kBlockSize), path, GetLastError());
        return false;
    }
    g_nsBlocks = blocks;
    printf("  nsfile: '%s' loaded, %u blocks x %u B = %u MiB (namespace nsze=%u)\n",
           path, (unsigned)g_nsBlocks, (unsigned)kBlockSize,
           (unsigned)((blocks * kBlockSize) >> 20), (unsigned)g_nsBlocks);
    return true;
}

// Write the namespace region back to the medium.  Called on every FLUSH and once at
// the end of a target run.
static bool nsFileFlush(Device& dev, bool report) {
    if (g_nsHandle == INVALID_HANDLE_VALUE) return true;
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(g_nsHandle, zero, nullptr, FILE_BEGIN)) return false;
    DWORD want = g_nsBlocks * kBlockSize, wrote = 0;
    if (!WriteFile(g_nsHandle, dev.region(kNsOff), want, &wrote, nullptr) || wrote != want) {
        printf("  nsfile: wrote %lu of %u bytes (error %lu)\n",
               (unsigned long)wrote, (unsigned)want, GetLastError());
        return false;
    }
    if (!FlushFileBuffers(g_nsHandle)) return false;
    g_nsFlushes++;
    if (report) {
        printf("  nsfile: '%s' persisted (%u blocks, %llu flush(es) this run)\n",
               g_nsFile, (unsigned)g_nsBlocks, (unsigned long long)g_nsFlushes);
    }
    return true;
}
static void Report(const char* what, int ok, const char* detail) {
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           (detail && *detail) ? " - " : "", (detail && *detail) ? detail : "");
    if (!ok) g_failures++;
}

// ---------------------------------------------------------------------------
//  SGL parsing and completion building, shared by both roles
// ---------------------------------------------------------------------------
struct Sgl { bool present; uint8_t type; uint64_t addr; uint32_t key; uint32_t len;
             bool invalidate; };

static Sgl parseSgl(const uint8_t* cap) {
    Sgl s = {};
    const uint8_t ts = cap[24 + 15];
    s.type = nvmeof_sgl_type_of(ts);
    s.addr = nvmeof_rd64(cap + 24);
    s.len  = nvmeof_rd24(cap + 24 + 8);
    s.key  = nvmeof_rd32(cap + 24 + 11);
    s.present = nvmeof_sgl_is_keyed(ts) != 0;
    s.invalidate = nvmeof_sgl_wants_invalidate(ts) != 0;
    return s;
}

static void buildCompletion(uint8_t* cqe, uint16_t cid, uint16_t sqHead,
                            uint64_t result, uint8_t sct, uint8_t sc) {
    memset(cqe, 0, 16);
    nvmeof_wr64(cqe + 0, result);
    nvmeof_wr16(cqe + 8, sqHead);
    nvmeof_wr16(cqe + 12, cid);
    nvmeof_wr16(cqe + 14, NVMEOF_STATUS_CQE(sct, sc));
}

// The same, with Do Not Retry.  nvmet sets DNR on the errors where retrying the
// same command cannot possibly help - a bad SQE flags byte is one of them, and a
// test target that answers without DNR would not reproduce what a real host sees.
static void buildCompletionDnr(uint8_t* cqe, uint16_t cid, uint16_t sqHead,
                               uint64_t result, uint8_t sct, uint8_t sc) {
    buildCompletion(cqe, cid, sqHead, result, sct, sc);
    nvmeof_wr16(cqe + 14, (uint16_t)(nvmeof_rd16(cqe + 14) | NVMEOF_STATUS_DNR));
}

// ---------------------------------------------------------------------------
//  NOTE: the RETH rkey encoding quirk lives in nvmeof_rdma.h (ndWireKey), not here
// ---------------------------------------------------------------------------
//
// Every rkey that crosses the wire is byte-swapped by this driver's provider, on
// both the read it issues and the read it serves.  That is invisible between two
// peers sharing the driver, which is why no self-test in this project ever caught
// it, and fatal against Linux: nvmet's RDMA READ of our connect data came back
// "remote access error (10)" and our read of a Linux host's connect data came back
// ND_ACCESS_VIOLATION, until the tokens were swapped.  Device::open publishes
// ndWireKey(token) and Queue::read/write un-swap the peer's key, so call sites here
// use dev.rkey and the parsed SGL key directly.
// ---------------------------------------------------------------------------
//  Small serial command helper: one capsule out, one completion back
// ---------------------------------------------------------------------------
struct Completion { uint16_t cid; uint16_t status; uint64_t result; };

// -nvmetiming: split the serial round trip into "capsule send completed" and
// "target answered".  It exists because the serial path measures ~10 ms per
// command while the fabric's own 64 KiB read latency is 28 us - a 400x gap that
// cannot be attributed by looking at the total.  Inert without the switch.
static bool g_nvmeTiming = false;

// Per-I/O-command trace on the target, OFF by default.  See the note at its single
// use site: printing per command into an unbuffered stdout cost 52x on a serial
// read, and that cost was mistaken for RDMA completion latency (DESIGN 8.68).
static bool g_ioTrace = false;

// Machine-wide microseconds, from QueryPerformanceCounter.
//
// NOT std::chrono::steady_clock: MSVC gives that clock a per-process epoch, so two
// processes' timestamps cannot be subtracted - the first attempt at this produced
// hop times of 1.8 million microseconds and negative reply legs.  QPC counts share
// one epoch across the machine, which is exactly what a two-process hop trace needs.
static long long nowUs() {
    static const double invFreq = [] {
        LARGE_INTEGER f = {};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? (1000000.0 / (double)f.QuadPart) : 1.0;
    }();
    LARGE_INTEGER c = {};
    QueryPerformanceCounter(&c);
    return (long long)((double)c.QuadPart * invFreq);
}

static bool submitCommand(Queue& q, Device& d, const uint8_t* capsule64,
                          Completion* out, DWORD timeoutMs) {
    auto tStart = std::chrono::steady_clock::now();
    memcpy(d.region(kAdminCapOff), capsule64, 64);
    memset(d.region(kRespInOff), 0xFF, 16);
    if (!q.postReceive(d.region(kRespInOff), 16, CTX_RESP)) {
        printf("    postReceive(response) failed\n"); return false;
    }
    if (!q.send(d.region(kAdminCapOff), 64, CTX_CAP)) {
        printf("    send(capsule) failed\n"); return false;
    }
    ND2_RESULT r = {};
    if (!ndOk(q.reap(CTX_CAP, &r, kWaitMs))) {
        char b[64]; printf("    capsule send %s\n", ndStr(r.Status, b, sizeof(b))); return false;
    }
    auto tSent = std::chrono::steady_clock::now();
    long long nowUs0 = nowUs();
    HRESULT st = q.reap(CTX_RESP, &r, timeoutMs);
    auto tDone = std::chrono::steady_clock::now();
    if (g_nvmeTiming) {
        printf("    [nvme] init send-done qpc=%lld, reply qpc=%lld (send %lld us, answer %lld us)\n",
               nowUs0,
               nowUs(),
               (long long)std::chrono::duration_cast<std::chrono::microseconds>(tSent - tStart).count(),
               (long long)std::chrono::duration_cast<std::chrono::microseconds>(tDone - tSent).count());
    }
    if (!ndOk(st)) {
        char b[64];
        printf("    no completion after %u ms (%s) - the target did not answer\n",
               (unsigned)timeoutMs, ndStr(st, b, sizeof(b)));
        return false;
    }
    if (r.BytesTransferred != 16) {
        printf("    completion was %u bytes, expected 16\n", r.BytesTransferred);
        return false;
    }
    const uint8_t* cqe = d.region(kRespInOff);
    out->cid = nvmeof_rd16(cqe + 12);
    out->status = nvmeof_rd16(cqe + 14);
    out->result = nvmeof_rd64(cqe + 0);
    return true;
}

static bool statusOk(uint16_t st) {
    // The phase bit (bit 0 of the 16-bit status word) is NOT part of success for a
    // fabrics completion: Linux reads the status as `le16_to_cpu(status) >> 1` and
    // never looks at bit 0 (nvme_end_request).  nvmet therefore answers a perfectly
    // successful Connect with a status word whose bit 0 is clear - and this host
    // used to call that a failure, because our own target happens to set the bit.
    // That is the "both of our ends agree with each other" failure mode again, in
    // the one place where it costs a whole working connection.
    (void)NVMEOF_STATUS_P_MASK;
    return nvmeof_status_sct(st) == NVMEOF_SCT_GENERIC &&
           nvmeof_status_sc(st) == NVMEOF_SC_SUCCESS;
}

static void fabricHeader(uint8_t* cap, uint8_t opcode, uint16_t cid, uint8_t fctype) {
    memset(cap, 0, 64);
    cap[0] = opcode;
    // Byte 1 is not decoration: nvmet checks PSDT (and the FUSE bits) on EVERY
    // command before parsing it, and answers Invalid Field to anything that is not
    // exactly 0x40.  The Linux host sets it on every RDMA command it sends.
    cap[1] = (uint8_t)NVMEOF_CMD_FLAGS_METABUF;
    nvmeof_wr16(cap + 2, cid);
    cap[4] = fctype;
    // A command with no data still needs a keyed descriptor, not a zeroed field:
    // nvmet's RDMA transport switches on this byte for every command and answers
    // "invalid SGL subtype: 0x0" (Invalid Field) to a zeroed dptr.  Commands that do
    // carry data overwrite this with nvmeof_sgl_set_keyed.
    nvmeof_sgl_set_null((nvmeof_sgl*)(cap + 24));
}

static void dumpStatus(const char* what, const Completion& c) {
    printf("    %s: sct=%u sc=0x%02X%s\n", what, nvmeof_status_sct(c.status),
           nvmeof_status_sc(c.status),
           (c.status & NVMEOF_STATUS_DNR) ? " DNR" : "");
}

// ===========================================================================
//  -survey: describe the foreign namespace WITHOUT touching it.
//
//  Written for the case where the namespace is somebody's real disk: a 1 TB NVMe
//  drive exported from Linux over nvmet.  Two things have to be true of anything
//  that runs against a disk like that, and they are the whole design of this
//  function:
//
//    1. IT ONLY READS.  Not one WRITE, WRITE ZEROES, DSM or FLUSH is issued - the
//       self-tests that normally run later in this path write patterns at LBA 0,
//       which on a real disk is the partition table.
//    2. IT SAYS WHICH DISK IT IS.  Serial and model from Identify, then the GPT and
//       each partition's boot sector: a description you can check against the
//       machine the disk is plugged into, instead of "the namespace at that address".
//
//  Windows has no NVMe-oF initiator of its own, so "mount the Linux disk on Windows"
//  cannot mean a live block device.  What it can mean is this: read the real medium
//  over our own RDMA stack and say exactly what is on it.
// ===========================================================================
struct GptEntry {
    uint8_t  type[16];
    uint64_t firstLba;
    uint64_t lastLba;
    char     name[80];
};

static void guidToStr(const uint8_t* g, char* out, size_t n) {
    // GPT stores the first three fields little-endian and the remaining eight as-is.
    sprintf_s(out, n, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
              nvmeof_rd32(g), nvmeof_rd16(g + 4), nvmeof_rd16(g + 6),
              g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static void utf16ToAscii(const uint8_t* in, size_t bytes, char* out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; i + 1 < bytes && j + 1 < n; i += 2) {
        uint16_t ch = nvmeof_rd16(in + i);
        if (ch == 0) break;
        out[j++] = (ch < 0x80) ? (char)ch : '?';
    }
    out[j] = 0;
}

// Read `blocks` logical blocks at `lba` into `buf`.  The only I/O this file does
// outside the normal read/write paths, and it is a READ.
static bool surveyRead(Queue& q, Device& dev, uint32_t nsid, uint32_t blockSize,
                       uint8_t* cap, uint16_t& cid, Completion& c,
                       uint64_t lba, uint32_t blocks, uint8_t* buf) {
    fabricHeader(cap, NVMEOF_OPC_READ, ++cid, 0);
    nvmeof_wr32(cap + 4, nsid);
    nvmeof_wr64(cap + 40, lba);
    nvmeof_wr32(cap + 48, blocks - 1);
    nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)buf,
                         blocks * blockSize, dev.rkey);
    if (!submitCommand(q, dev, cap, &c, kWaitMs) || !statusOk(c.status)) {
        char b[64];
        printf("    READ lba=%llu blocks=%u -> sct=%u sc=0x%02X (%s)\n",
               (unsigned long long)lba, blocks, nvmeof_status_sct(c.status),
               nvmeof_status_sc(c.status), ndStr(c.status, b, sizeof(b)));
        return false;
    }
    return true;
}

static int surveyDisk(Queue& q, Device& dev, uint32_t nsid, uint64_t nsze,
                      uint32_t blockSize, uint8_t* cap, uint16_t& cid, Completion& c) {
    printf("\n-- SURVEY (read-only: no WRITE, no WRITE ZEROES, no DSM, no FLUSH)\n");
    printf("    namespace: %llu blocks x %u B = %.2f GiB\n", (unsigned long long)nsze,
           blockSize, (double)nsze * blockSize / 1073741824.0);
    if (blockSize != 512) {
        printf("    this survey understands 512-byte logical blocks only\n");
        return 0;
    }

    uint8_t* buf = dev.region(kXferOff);
    memset(buf, 0, 64 * 1024);
    if (!surveyRead(q, dev, nsid, blockSize, cap, cid, c, 0, 1, buf)) return 1;

    bool isGpt = false;
    if (nvmeof_rd16(buf + 510) == 0xAA55) {
        uint8_t type = buf[0x1BE + 4];
        uint32_t firstLba = nvmeof_rd32(buf + 0x1BE + 8);
        printf("    LBA 0: MBR, partition type 0x%02X, first LBA %u\n", type, firstLba);
        if (type == 0xEE) isGpt = true;          // 0xEE = GPT protective
    } else {
        printf("    LBA 0: no 0xAA55 signature - a bare filesystem, or not a disk?\n");
    }

    if (!isGpt) {
        if (memcmp(buf + 3, "NTFS    ", 8) == 0)
            printf("    ...and it says NTFS: this namespace IS a filesystem, not a disk\n");
        else
            printf("    ...and it is not NTFS either; nothing more this tool can say\n");
        return 0;
    }

    memset(buf, 0, 64 * 1024);
    if (!surveyRead(q, dev, nsid, blockSize, cap, cid, c, 1, 1, buf)) return 1;
    if (memcmp(buf, "EFI PART", 8) != 0) {
        printf("    LBA 1: not a GPT header ('%.8s')\n", (const char*)buf);
        return 0;
    }
    uint64_t myLba       = nvmeof_rd64(buf + 24);
    uint64_t altLba      = nvmeof_rd64(buf + 32);
    uint64_t firstUsable = nvmeof_rd64(buf + 40);
    uint64_t lastUsable  = nvmeof_rd64(buf + 48);
    uint32_t numEntries  = nvmeof_rd32(buf + 80);
    uint32_t entrySize   = nvmeof_rd32(buf + 84);
    char g[40];
    guidToStr(buf + 56, g, sizeof(g));
    printf("    GPT: header at LBA %llu (backup at %llu), disk GUID %s\n",
           (unsigned long long)myLba, (unsigned long long)altLba, g);
    printf("         usable LBA %llu..%llu, %u entries x %u B\n",
           (unsigned long long)firstUsable, (unsigned long long)lastUsable,
           numEntries, entrySize);
    if (numEntries > 128 || entrySize < 128 || entrySize > 512) {
        printf("         (unusual entry count/size - stopping rather than guessing)\n");
        return 0;
    }

    const uint32_t perSector = 512 / entrySize;
    const uint32_t sectors = (numEntries + perSector - 1) / perSector;
    memset(buf, 0, 64 * 1024);
    if (!surveyRead(q, dev, nsid, blockSize, cap, cid, c, 2, sectors, buf)) return 1;

    printf("\n    %-3s %-38s %-13s %-13s %s\n", "#", "type GUID", "first LBA",
           "last LBA", "name");
    struct Found { uint64_t first; uint64_t count; };
    Found found[8] = {}; int nFound = 0;
    for (uint32_t i = 0; i < numEntries; i++) {
        const uint8_t* e = buf + (size_t)i * entrySize;
        bool empty = true;
        for (int k = 0; k < 16; k++) if (e[k]) { empty = false; break; }
        if (empty) continue;
        uint64_t firstLba = nvmeof_rd64(e + 32);
        uint64_t lastLba  = nvmeof_rd64(e + 40);
        char name[80];
        utf16ToAscii(e + 56, 72, name, sizeof(name));
        guidToStr(e, g, sizeof(g));
        printf("    %-3u %-38s %-13llu %-13llu %s\n", i, g,
               (unsigned long long)firstLba, (unsigned long long)lastLba,
               name[0] ? name : "(no name)");
        if (nFound < 8) {
            found[nFound].first = firstLba;
            found[nFound].count = lastLba - firstLba + 1;
            nFound++;
        }
    }

    // ---- each partition's first sector: NTFS?  and how big does IT think it is? ----
    for (int i = 0; i < nFound; i++) {
        if (found[i].first == 0) continue;
        memset(buf, 0, 64 * 1024);
        if (!surveyRead(q, dev, nsid, blockSize, cap, cid, c, found[i].first, 1, buf)) return 1;
        printf("\n    partition %d (LBA %llu, %.1f GiB):\n", i,
               (unsigned long long)found[i].first,
               (double)found[i].count * 512 / 1073741824.0);
        if (memcmp(buf + 3, "NTFS    ", 8) != 0) {
            printf("      first sector is not NTFS ('%.8s' at offset 3) - not described\n",
                   (const char*)(buf + 3));
            continue;
        }
        uint16_t bytesPerSector = nvmeof_rd16(buf + 11);
        uint8_t  secPerCluster  = buf[13];
        uint64_t totalSectors   = nvmeof_rd64(buf + 40);
        uint64_t mftCluster     = nvmeof_rd64(buf + 48);
        uint64_t mftMirrCluster = nvmeof_rd64(buf + 56);
        int8_t   clustersPerRec = (int8_t)buf[64];
        uint64_t volSerial      = nvmeof_rd64(buf + 72);
        uint32_t clusterBytes   = (uint32_t)bytesPerSector * (secPerCluster ? secPerCluster : 1);
        uint32_t recordBytes = (clustersPerRec > 0)
                                   ? (uint32_t)clustersPerRec * clusterBytes
                                   : (1u << (uint32_t)(-clustersPerRec));
        printf("      NTFS: %u B/sector, %u B/cluster, volume says 0x%llX sectors = %.2f GiB\n",
               bytesPerSector, clusterBytes, (unsigned long long)totalSectors,
               (double)totalSectors * bytesPerSector / 1073741824.0);
        printf("            serial 0x%016llX, MFT cluster %llu, MFTMirr cluster %llu, "
               "%u B/record\n", (unsigned long long)volSerial,
               (unsigned long long)mftCluster, (unsigned long long)mftMirrCluster,
               recordBytes);
        if (bytesPerSector != 512 || clusterBytes == 0 || recordBytes < 512 ||
            recordBytes > 4096) {
            printf("            (geometry this survey does not parse)\n");
            continue;
        }

        // The volume label lives in MFT record 3 ($Volume), attribute 0x60
        // ($VOLUME_NAME).  Reading it is the cheapest proof that this tool is looking
        // at the FILESYSTEM and not merely at a boot sector that says "NTFS".
        uint64_t mftByteOff = found[i].first * 512 + mftCluster * clusterBytes;
        uint64_t rec3Off    = mftByteOff + 3ull * recordBytes;
        uint8_t* rec = dev.region(kXferOff + 64 * 1024);
        memset(rec, 0, recordBytes);
        if (!surveyRead(q, dev, nsid, blockSize, cap, cid, c, rec3Off / 512,
                        (recordBytes + 511) / 512, rec))
            return 1;
        if (memcmp(rec, "FILE", 4) != 0) {
            printf("            MFT record 3 does not start with FILE - not parsed\n");
            continue;
        }
        uint32_t attrOff = nvmeof_rd16(rec + 20);
        bool got = false;
        while (attrOff + 8 < recordBytes && nvmeof_rd32(rec + attrOff) != 0xFFFFFFFFu) {
            uint32_t type = nvmeof_rd32(rec + attrOff);
            uint32_t len  = nvmeof_rd32(rec + attrOff + 4);
            if (len < 8 || attrOff + len > recordBytes) break;
            if (type == 0x60) {                                  // $VOLUME_NAME
                uint32_t vlen = nvmeof_rd32(rec + attrOff + 16);
                if (vlen > 0 && vlen <= 512) {
                    char label[260];
                    utf16ToAscii(rec + attrOff + 24, vlen, label, sizeof(label));
                    printf("            volume label: '%s'\n", label);
                    got = true;
                }
            }
            attrOff += len;
        }
        if (!got) printf("            volume label: (none set)\n");
    }
    printf("\n-- end of survey\n");
    return 0;
}

// ===========================================================================
//  The iSCSI bridge's backend: one SCSI command in, one NVMe-oF command out.
//
//  It lives here rather than in nvmeof_iscsi.h because it needs this file's
//  submitCommand/statusOk/fabricHeader, and because that is the whole point of the
//  design: the SCSI command is translated into an NVMe command on the SAME queue
//  pair the rest of this program uses, in the same process, with no second
//  connection and no IPC.
// ===========================================================================
class NvmeIscsiBackend : public IscsiBackend {
public:
    NvmeIscsiBackend(Queue& q_, Queue& adm_, Device& d_, uint32_t nsid_, uint64_t nsze_,
                     uint32_t bs_, uint8_t* cap_, uint16_t* cid_, Completion* c_,
                     uint8_t* buf_, bool rw_, int katoMs_)
        : q(q_), adm(adm_), d(d_), nsid(nsid_), nsze(nsze_), bs(bs_), cap(cap_), cid(cid_),
          c(c_), buf(buf_), rw(rw_), katoMs(katoMs_), lastKa(GetTickCount64()) {
        snprintf(ser, sizeof(ser), "%s", g_foreignSerial);
        for (char* p = ser; *p; p++) if (*p == ' ') *p = '_';
    }
    uint64_t blocks() const override { return nsze; }
    uint32_t blockSize() const override { return bs; }
    const char* serial() const override { return ser; }
    const char* model() const override { return g_foreignModel; }
    bool writable() const override { return rw; }
    // Three consecutive failures with no success in between is the line between "this
    // command failed" and "this queue pair is dead".  Measured, after the backend
    // process exited: every later submit returned ND_CANCELED, so the FIRST failure
    // here is already permanent in practice - the threshold is only there so that a
    // transient, retryable error cannot tear a healthy session down.
    static const int kLostAfter = 3;
    bool lost() const override { return consecFail >= kLostAfter; }
    void noteOk() { consecFail = 0; }
    void noteFail(const char* what) {
        if (++consecFail == kLostAfter) {
            printf("\n  [iscsi] BACKEND LOST: %d consecutive NVMe failures (last: %s)\n"
                   "          this namespace can no longer be served; ending the session so\n"
                   "          the process exits and the service restarts into -backendretry\n",
                   consecFail, what);
            g_backendLost = true;
        }
    }
    // The staging region is what the NVMe READ is sized against (see kIscsiOff).
    uint32_t maxTransfer() const override { return (uint32_t)kIscsiBytes; }

    bool read(uint64_t lba, uint32_t nblocks, void* out) override {
        return xfer(NVMEOF_OPC_READ, lba, nblocks, out);
    }
    bool write(uint64_t lba, uint32_t nblocks, const void* in) override {
        // const_cast: the SGL takes a uint64_t address and this provider does not
        // write to it for a WRITE command.  Stated here rather than hidden.
        return xfer(NVMEOF_OPC_WRITE, lba, nblocks, const_cast<void*>(in));
    }
    bool flush() override {
        fabricHeader(cap, NVMEOF_OPC_FLUSH, ++(*cid), 0);
        nvmeof_wr32(cap + 4, nsid);
        nvmeof_sgl_set_null((nvmeof_sgl*)(cap + 24));
        bool ok = submitCommand(q, d, cap, c, kWaitMs) && statusOk(c->status);
        if (ok) noteOk(); else noteFail("FLUSH");
        return ok;
    }
    // Between SCSI commands: feed the far end's keep-alive timer.  Linux nvmet
    // disconnects a controller that goes quiet for KATO, and an iSCSI session can
    // easily outlive it - the first long copy through this bridge would otherwise
    // die in the middle with no hint as to why.
    //
    // ON THE ADMIN QUEUE.  Keep Alive is an admin command (opcode 0x18, QID 0), and
    // this used to put it on the I/O queue pair - where a target is entitled to do
    // exactly nothing with it.  Measured against our own backend: the I/O path logged
    // "UNHANDLED opcode=0x18 - a real host sent this", never posted a completion, and
    // the bridge reported a failed keep-alive 60 s into every session (katoMs/2).  The
    // session survived only because nothing here enforces KATO; nvmet does, so against
    // a real Linux target this was a controller teardown waiting for the first long
    // copy.  Note the asymmetry with flush(): NVMe FLUSH is an NVM command and does
    // belong on the I/O queue.
    void tick() override {
        if (katoMs <= 0) return;
        ULONGLONG now = GetTickCount64();
        // Normally one keep-alive per KATO/6 (20 s for the 120 s this bridge asks for),
        // not per KATO/2.  Both satisfy the contract - the target only requires one per
        // KATO - but this is also the ONLY probe that runs on an idle session, and
        // "idle" is exactly how a dead backend looks from here.  Measured with KATO/2:
        // the backend was killed 9 s into a session and the bridge took 59 s to notice
        // (the first probe came at +60 s, then two fast retries).  At KATO/6 the same
        // event is detected in about 25 s.  A keep-alive is a 64-byte capsule and a
        // 16-byte completion; the cost is not worth optimising.
        //
        // While the last one FAILED the interval drops to the socket's idle wake-up, so
        // three consecutive failures accumulate in a few seconds.
        ULONGLONG gap = lastKaFailed ? 2000 : (ULONGLONG)(katoMs / 6);
        if (now - lastKa < gap) return;
        lastKa = now;
        fabricHeader(cap, NVMEOF_OPC_KEEP_ALIVE, ++(*cid), 0);
        nvmeof_sgl_set_null((nvmeof_sgl*)(cap + 24));
        bool ok = submitCommand(adm, d, cap, c, kWaitMs) && statusOk(c->status);
        lastKaFailed = !ok;
        if (ok) noteOk(); else noteFail("KEEP ALIVE");
        if (!ok) printf("  [iscsi] keep-alive to the NVMe target failed\n");
    }

private:
    bool xfer(uint8_t opcode, uint64_t lba, uint32_t nblocks, void* p) {
        // The SGL address MUST live inside the memory region registered with the
        // HCA.  The iSCSI layer hands us its own scratch vector, which is ordinary
        // heap memory and not registered, so the NVMe command names the bridge's
        // registered staging area (`buf`, kIscsiOff in the device region) and the
        // bytes are copied across on this side.  Without this the target receives
        // an SGL with addr 0x0, its RDMA Read fetches nothing, and the initiator
        // gets a bare NVMe status error - a failure with no diagnostic anywhere.
        // The copy is the price of keeping the iSCSI layer free of RDMA details.
        uint32_t bytes = nblocks * bs;
        void* stage = buf;
        if (opcode == NVMEOF_OPC_WRITE && p && p != stage) memcpy(stage, p, bytes);
        fabricHeader(cap, opcode, ++(*cid), 0);
        nvmeof_wr32(cap + 4, nsid);
        nvmeof_wr64(cap + 40, lba);
        nvmeof_wr32(cap + 48, nblocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)stage,
                             bytes, d.rkey);
        if (!submitCommand(q, d, cap, c, kWaitMs) || !statusOk(c->status)) {
            char b[64];
            printf("  [iscsi] NVMe %s lba=%llu blocks=%u -> sct=%u sc=0x%02X (%s)\n",
                   opcode == NVMEOF_OPC_READ ? "READ" : "WRITE",
                   (unsigned long long)lba, nblocks, nvmeof_status_sct(c->status),
                   nvmeof_status_sc(c->status), ndStr(c->status, b, sizeof(b)));
            noteFail(opcode == NVMEOF_OPC_READ ? "READ" : "WRITE");
            return false;
        }
        noteOk();
        if (opcode == NVMEOF_OPC_READ && p && p != stage) memcpy(p, stage, bytes);
        return true;
    }
    Queue& q;
    Queue& adm;                     // keep-alives only: 0x18 is an admin command
    Device& d;
    uint32_t nsid; uint64_t nsze; uint32_t bs;
    uint8_t* cap; uint16_t* cid; Completion* c;
    uint8_t* buf;
    bool rw;
    int katoMs;
    int consecFail = 0;
    bool lastKaFailed = false;
    ULONGLONG lastKa;
    char ser[40];
};

// ===========================================================================
//  INITIATOR - our host against a foreign target
// ===========================================================================
static int runInitiator(const char* serverIp, uint16_t port, const char* localIp,
                        const char* subnqn, const char* hostnqn, int wantQueues,
                        uint32_t blocks, bool discover,
                        const char* authKey, const char* ctrlAuthKey, bool authSkip) {
    // In discovery mode the host connects to the well-known discovery NQN and never
    // creates an I/O queue: that is what `nvme discover` does.
    const char* connectNqn = discover ? NVMEOF_DISC_SUBSYS_NAME : subnqn;
    if (discover) wantQueues = 0;
    printf("=== F5 INITIATOR %s -> %s:%u%s\n", localIp, serverIp, port,
           discover ? "   [discovery]" : "");
    printf("    subnqn  '%s'\n    hostnqn '%s'\n", connectNqn, hostnqn);

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, localIp, &local.sin_addr);
    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    InetPtonA(AF_INET, serverIp, &remote.sin_addr);

    Device dev;
    if (!dev.open(local, kRegionBytes)) return 1;
    Queue admin;
    if (!admin.create(&dev, 0)) return 1;
    // One queue pair per I/O queue this run may create.  A real host creates
    // min(what it asked for, what the target granted) queue pairs and gives each its
    // own RDMA-CM connection and its own fabrics Connect command; asking for several
    // and then using one is the host-side version of the same lie as a target that
    // grants more queues than it has (DESIGN 8.47).
    Queue io[kMaxIoQueues];
    for (uint16_t i = 0; i < kMaxIoQueues; i++) {
        if (!io[i].create(&dev, (uint16_t)(i + 1))) return 1;
    }

    // ---- 1. RDMA-CM connection, with private data filled as Linux fills it ----
    //
    // The private data is not optional: nvmet rejects a Connect with none
    // (NVME_RDMA_CM_INVALID_LEN), and it enforces hsqsize + 1 <= NVME_AQ_DEPTH on
    // the admin queue (NVME_RDMA_CM_INVALID_HSQSIZE).  Both were bugs here.
    {
        nvmeof_rdma_request_pd pd;
        nvmeof_rdma_fill_req(&pd, 0, kSqDepth, 0);
        char b[64];
        HRESULT h = admin.conn->Bind((const sockaddr*)&local, sizeof(local));
        if (h == ND_PENDING) h = admin.waitOverlapped(admin.conn, kWaitMs);
        h = admin.conn->Connect(admin.qp, (const sockaddr*)&remote, sizeof(remote),
                               kReadLimit, kReadLimit, &pd, sizeof(pd), &admin.ov);
        if (h == ND_PENDING) h = admin.waitOverlapped(admin.conn, 20000);
        bool up = ndOk(h);
        if (up) {
            h = admin.conn->CompleteConnect(&admin.ov);
            if (h == ND_PENDING) h = admin.waitOverlapped(admin.conn, kWaitMs);
            up = ndOk(h);
        }
        printf("  [conn] %s\n", up ? "admin queue pair connected" : ndStr(h, b, sizeof(b)));
        if (!up) {
            Report("admin RDMA-CM connection to the target", 0, "connect failed");
            printf("\ninitiator failures: %d\n", g_failures);
            // Release what this attempt took before returning.  This path used to leak
            // the device, its queues and the registered region - harmless when the
            // process exited right after, fatal the moment -backendretry made this
            // function run again in a loop: every retry would take another device and
            // another few MiB of registration until the adapter ran out.  No lock is
            // needed here: this runs before any iSCSI session exists.
            for (uint16_t q = 0; q < kMaxIoQueues; q++) io[q].destroy();
            admin.destroy();
            dev.close(nullptr, nullptr, 0);
            return g_failures;
        }
        Report("admin RDMA-CM connection to the target", 1, nullptr);
    }

    uint8_t cap[64];
    Completion c = {};
    uint16_t cid = 0;

    // ---- 2. fabrics Connect on the admin queue: THE FIRST COMMAND ----
    //
    // Order matters and this used to be wrong.  A fabrics controller is created by
    // the Connect command; until then nvmet's admin queue has NO controller, and
    // nvmet routes every capsule on a ctrl-less queue to its connect parser, which
    // answers anything that is not a Connect with Invalid Opcode (DESIGN 8.46).
    // The old order here was the PCIe one (CAP -> CC.EN -> CSTS -> Connect), which
    // cannot work over fabrics: it is what a real target rejected.
    //
    // The Linux host order, from nvme_rdma_configure_admin_queue ->
    // nvmf_connect_admin_queue -> nvme_enable_ctrl, is:
    //     Connect(qid 0) -> Get CAP -> Set CC -> Get CAP -> [Get CRTO]
    //     -> Set CC|EN -> poll Get CSTS until RDY -> Get VS -> Identify ...
    uint16_t cntlid = 0;
    {
        uint8_t* cd = dev.region(kConnectOff);
        memset(cd, 0, NVMEOF_CONNECT_DATA_SIZE);
        // hostid: the Linux host sends a UUID here; any fixed value is legal.
        for (int i = 0; i < 16; i++) cd[i] = (uint8_t)(0x5A ^ i);
        // 0xffff on the admin connect is not a placeholder: nvmet rejects anything
        // else with CONNECT_INVALID_PARAM (target/fabrics-cmd.c:237-243).
        nvmeof_wr16(cd + 16, NVMEOF_CNTLID_DYNAMIC);
        memcpy(cd + 256, connectNqn, strlen(connectNqn));
        memcpy(cd + 512, hostnqn, strlen(hostnqn));
        fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_CONNECT);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, dev.rkey);
        nvmeof_wr16(cap + 42, 0);                       // qid 0 = admin
        nvmeof_wr16(cap + 44, kSqSize);
        nvmeof_wr32(cap + 48, NVMEOF_KATO_DEFAULT);
        bool sent = submitCommand(admin, dev, cap, &c, kWaitMs);
        cntlid = (uint16_t)c.result;
        char d[128];
        sprintf_s(d, "cntlid=%u", cntlid);
        if (!sent || !statusOk(c.status)) dumpStatus("Connect qid 0", c);
        Report("fabrics Connect qid 0 (the target's NQN was found)", sent && statusOk(c.status) && cntlid != 0, d);
        if (!sent || !statusOk(c.status)) { printf("\ninitiator failures: %d\n", g_failures); return g_failures; }

        // ---- 2b. DH-HMAC-CHAP, when the Connect result says to ----
        //
        // The trigger is ATR, bit 17 of the Connect COMPLETION RESULT - not a status
        // code, and not a separate command.  A host that reads only the low 16 bits
        // of the result (which is all the controller id needs) never learns that it
        // has to authenticate, and then every command it sends comes back 0x4191.
        bool needsAuth = (c.result & NVMEOF_CONNECT_AUTHREQ_ATR) != 0;
        if (needsAuth && authSkip) {
            // Deliberately ignore the request and carry on.  This is the only way to
            // reach a controller's refusal gate from this project's own host, and the
            // gate is load-bearing: nvmet answers every non-fabrics command on an
            // unauthenticated admin queue with 0x4191 | DNR, and an implementation
            // that answers the command instead looks perfectly healthy.
            printf("  [auth] ATR is set (result=0x%llX) and -authskip was given: sending "
                   "a non-fabrics command anyway, which the target must refuse\n",
                   (unsigned long long)c.result);
        }
        if (needsAuth && !authSkip) {
            printf("  [auth] the Connect result asks for authentication (ATR set, "
                   "result=0x%llX)\n", (unsigned long long)c.result);
            Report("the target required authentication and a key was supplied",
                   authKey && *authKey, authKey ? "" : "no -authkey was given: every "
                   "non-fabrics command will now be refused with 0x4191");
            if (!authKey || !*authKey) { printf("\ninitiator failures: %d\n", g_failures); return g_failures; }

            nvmeof_dhchap::HostAuth ha;
            if (!ha.init(authKey, hostnqn, connectNqn, ctrlAuthKey)) {
                printf("  [auth] %s\n", ha.failWhy);
                Report("the auth key is usable", 0, ha.failWhy);
                printf("\ninitiator failures: %d\n", g_failures);
                return g_failures;
            }

            // One helper for both directions, because the two commands differ only in
            // which way the payload goes: an Auth Send RDMA-Writes our buffer to the
            // target's SGL, an Auth Receive RDMA-Reads the answer back into it.  The
            // lengths are `tl` and `al`, NOT the SGL length - nvmet checks they agree
            // with the SGL and answers SGL_INVALID_DATA | DNR when they do not.
            uint8_t* authBuf = dev.region(kAuthOff);
            auto authSend = [&](const uint8_t* msg, uint32_t len) -> bool {
                memcpy(authBuf, msg, len);
                fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_AUTH_SEND);
                cap[NVMEOF_AUTH_OFF_SPSP0] = (uint8_t)NVMEOF_AUTH_SPSP0;
                cap[NVMEOF_AUTH_OFF_SPSP1] = (uint8_t)NVMEOF_AUTH_SPSP1;
                cap[NVMEOF_AUTH_OFF_SECP]  = (uint8_t)NVMEOF_AUTH_SECP_DHCHAP;
                nvmeof_wr32(cap + NVMEOF_AUTH_OFF_AL_TL, len);
                nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)authBuf,
                                     len, dev.rkey);
                return submitCommand(admin, dev, cap, &c, kWaitMs);
            };
            // The host always asks for 4096 and the target always answers all of it
            // (zero padded), which is what nvmet does with CHAP_BUF_SIZE.
            auto authReceive = [&](uint32_t al) -> bool {
                memset(authBuf, 0, al);
                fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_AUTH_RECEIVE);
                cap[NVMEOF_AUTH_OFF_SPSP0] = (uint8_t)NVMEOF_AUTH_SPSP0;
                cap[NVMEOF_AUTH_OFF_SPSP1] = (uint8_t)NVMEOF_AUTH_SPSP1;
                cap[NVMEOF_AUTH_OFF_SECP]  = (uint8_t)NVMEOF_AUTH_SECP_DHCHAP;
                nvmeof_wr32(cap + NVMEOF_AUTH_OFF_AL_TL, al);
                nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)authBuf,
                                     al, dev.rkey);
                return submitCommand(admin, dev, cap, &c, kWaitMs);
            };

            bool ok = ha.buildNegotiate() && authSend(ha.outMsg, (uint32_t)ha.outLen);
            if (!ok || !statusOk(c.status)) {
                dumpStatus("Auth Send (Negotiate)", c);
                Report("Auth Send negotiated the exchange", 0, nullptr);
                printf("\ninitiator failures: %d\n", g_failures);
                return g_failures;
            }
            ok = authReceive(4096) && statusOk(c.status);
            if (!ok) { dumpStatus("Auth Receive (Challenge)", c); }
            if (ok && authBuf[0] == nvmeof_dhchap::kAuthTypeCommon &&
                      authBuf[1] == nvmeof_dhchap::kMsgFailure1) {
                ha.onFailure1(authBuf, 4096);
                ok = false;
            }
            Report("Auth Receive delivered a Challenge", ok && !ha.failed,
                   ha.failed ? ha.failWhy : "");

            bool hostVerified = false;
            if (ok) {
                // The Challenge names the hash and the DH group the TARGET chose;
                // the host must use those, not the ones it offered.
                printf("  [auth] Challenge: hash=0x%02X dhgroup=%u seqnum=%u\n",
                       authBuf[8], authBuf[9],
                       (unsigned)((uint32_t)authBuf[12] | ((uint32_t)authBuf[13] << 8) |
                                  ((uint32_t)authBuf[14] << 16) | ((uint32_t)authBuf[15] << 24)));
                ok = ha.onChallenge(authBuf, 4096) && authSend(ha.outMsg, (uint32_t)ha.outLen);
                if (!ok || !statusOk(c.status)) {
                    dumpStatus("Auth Send (Reply)", c);
                    Report("Auth Send delivered the response", 0,
                           ha.failed ? ha.failWhy : "");
                    printf("\ninitiator failures: %d\n", g_failures);
                    return g_failures;
                }
                Report("Auth Send delivered the response", 1, nullptr);

                memset(authBuf, 0, 4096);
                ok = authReceive(4096) && statusOk(c.status);
                if (!ok) { dumpStatus("Auth Receive (Success1)", c); }
                if (ok && authBuf[0] == nvmeof_dhchap::kAuthTypeCommon &&
                          authBuf[1] == nvmeof_dhchap::kMsgFailure1) {
                    ha.onFailure1(authBuf, 4096);
                    ok = false;
                }
                bool need2 = false;
                if (ok) ok = ha.onSuccess1(authBuf, 4096, &need2);
                hostVerified = ok && (ha.authenticated || need2);
                Report("the target accepted the host's response (Success1)",
                       hostVerified, ha.failed ? ha.failWhy : "");
                if (need2) {
                    ok = authSend(ha.outMsg, (uint32_t)ha.outLen) && statusOk(c.status);
                    Report("Success2 sent (the controller's own response verified)", ok,
                           ha.failed ? ha.failWhy : "");
                } else {
                    printf("  [auth] one-way: the target sent no controller response "
                           "(rvalid=0), so there is no Success2\n");
                }
            }
            Report("DH-HMAC-CHAP authentication completed", hostVerified && !ha.failed,
                   ha.failed ? ha.failWhy : "");
            if (!hostVerified) {
                printf("\ninitiator failures: %d\n", g_failures);
                return g_failures;
            }
        } else if (!needsAuth) {
            printf("  [auth] the Connect result carries no ATR: authentication is not "
                   "required by this target\n");
        }
    }

    // ---- 3. the controller registers, in the order a real host touches them ----
    uint64_t capValue = 0;
    {
        auto getReg = [&](uint32_t off, bool eightByte) -> bool {
            fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_PROPERTY_GET);
            // CAP is a 64-bit register and the attrib byte has to say so: nvmet's
            // nvmet_execute_prop_get answers CAP only when `attrib & 1` is set, and
            // answers VS/CC/CSTS only when it is clear.  Note the value comes back in
            // the completion's RESULT field (target/fabrics-cmd.c:81) - there is no
            // data transfer for a register read.
            cap[40] = (uint8_t)(eightByte ? NVMEOF_PROP_SIZE_8 : NVMEOF_PROP_SIZE_4);
            nvmeof_wr32(cap + 44, off);
            return submitCommand(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        };
        auto setReg = [&](uint32_t off, uint64_t val) -> bool {
            fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_PROPERTY_SET);
            cap[40] = (uint8_t)NVMEOF_PROP_SIZE_4;   // attrib&1 is ILLEGAL for a Set
            nvmeof_wr32(cap + 44, off);
            nvmeof_wr64(cap + 48, val);
            return submitCommand(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        };

        bool got = getReg(NVMEOF_PROP_CAP, true);
        capValue = got ? c.result : 0;
        char d[96];
        sprintf_s(d, "CAP=0x%llX MQES=%llu TO=%llu", (unsigned long long)capValue,
                  (unsigned long long)(capValue & 0xffff),
                  (unsigned long long)((capValue >> 24) & 0xff));
        Report("Property Get CAP", got && capValue != 0, d);

        // The CC a real host writes: CSS = NVM (0), MPS = 0, AMS = 0, and the two
        // entry sizes - IOSQES = 6 (64 B), IOCQES = 4 (16 B).  Enabled is added
        // later.  nvmet does not check these bits today, but a target that does is
        // entitled to refuse a CC of 0x0, and this is what the reference sends.
        const uint64_t kCCBase = (6ull << 16) | (4ull << 20);
        Report("Property Set CC (entry sizes, still disabled)", setReg(NVMEOF_PROP_CC, kCCBase), nullptr);

        if (getReg(NVMEOF_PROP_CAP, true)) capValue = c.result;
        // CRTO is read only when CAP says the controller is ready with a configurable
        // timeout (CRMS.CRWMS and CRMS.CRIMS both set, host/core.c).  nvmet sets
        // CRWMS alone, which is exactly why a host must ask before reading CRTO:
        // nvmet's property get does not implement that register at all.
        if ((capValue & NVMEOF_CAP_CRMS_CRWMS) && (capValue & NVMEOF_CAP_CRMS_CRIMS)) {
            uint32_t crto = 0;
            bool okc = getReg(NVMEOF_PROP_CRTO, false);
            if (okc) crto = (uint32_t)c.result;
            char dc[64]; sprintf_s(dc, "CRTO=%u", crto);
            Report("Property Get CRTO (CAP advertises CRWMS+CRIMS)", okc, dc);
        } else {
            printf("  [cap]  CRMS.CRWMS=%d CRMS.CRIMS=%d -> CRTO is not read (as the reference host)\n",
                   (capValue & NVMEOF_CAP_CRMS_CRWMS) ? 1 : 0, (capValue & NVMEOF_CAP_CRMS_CRIMS) ? 1 : 0);
        }

        Report("Property Set CC.EN=1", setReg(NVMEOF_PROP_CC, kCCBase | 1ull), nullptr);

        bool rdy = false;
        for (int i = 0; i < 20 && !rdy; i++) {
            if (!getReg(NVMEOF_PROP_CSTS, false)) break;
            rdy = (c.result & 1) != 0;
            if (!rdy) Sleep(50);
        }
        Report("controller reports ready (CSTS.RDY)", rdy, nullptr);

        bool vs = getReg(NVMEOF_PROP_VS, false);
        char dv[64]; sprintf_s(dv, "VS=0x%08X", (unsigned)(vs ? c.result : 0));
        Report("Property Get VS", vs, dv);
    }

    // Data-out probes: the same direction as the Identify's 4096-byte transfer, with
    // size and content varied.  A SMART log page is mostly zeros, so "did it arrive"
    // is checked with a page that is definitely not zeros (LID 3, firmware slot,
    // which nvmet fills with the kernel release string).
    {
        auto logPage = [&](uint8_t lid, uint32_t bytes, bool expectContent, const char* label) -> void {
            uint8_t* scratch = dev.region(kOutOff);
            memset(scratch, 0, bytes);
            fabricHeader(cap, NVMEOF_OPC_GET_LOG_PAGE, ++cid, 0);
            cap[40] = lid;                                  // LID = CDW10 bits 7:0
            // NUMDL is CDW10 bits 31:16 and NUMDU is CDW11 bits 15:0 - one dword
            // EARLIER than a PCI-style "length" would live.  Putting it in CDW11 at
            // bits 31:16 leaves NUMDL = 0, nvmet then expects 4 bytes, and it answers
            // SGL_INVALID_DATA (0x0F) - which is exactly what this probe got the
            // first time it ran.
            uint32_t numd = bytes / 4 - 1;
            nvmeof_wr16(cap + 42, (uint16_t)(numd & 0xffffu));
            nvmeof_wr16(cap + 44, (uint16_t)(numd >> 16));
            nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)scratch,
                                 bytes, dev.rkey);
            bool got = submitCommand(admin, dev, cap, &c, kWaitMs);
            int nz = 0;
            for (uint32_t i = 0; i < bytes; i++) if (scratch[i]) nz++;
            char d[160];
            sprintf_s(d, "completion=%s sct=%u sc=0x%02X nonzero_bytes=%d",
                      got ? "yes" : "NO", nvmeof_status_sct(c.status),
                      nvmeof_status_sc(c.status), nz);
            // Only the completion is asserted: a log page whose content is legally all
            // zeros (our own target zero-fills them) cannot prove the write landed,
            // and nvmet needs an exact size for some LIDs.  What proves the inbound
            // data path is the Identify below, whose 4096 bytes have known content.
            (void)expectContent;
            // On a DISCOVERY controller the expectation flips, and that is not a
            // technicality: nvmet answers Invalid Field (sct=0 sc=0x02) to the error
            // log (LID 1) and the effects log (LID 5) when the controller is a
            // discovery one, because the only log page it has is the discovery log.
            // The first discovery run against nvmet reported these two as failures.
            if (discover) {
                Report(label, got && !statusOk(c.status), d);
            } else {
                Report(label, got && statusOk(c.status), d);
            }
        };
        logPage(1, 1024, false, discover ? "a discovery controller refuses the error log (LID 1)"
                                         : "inbound RDMA Write, 1024 B (Get Log Page LID 1)");
        logPage(5, 4096, true,  discover ? "a discovery controller refuses the effects log (LID 5)"
                                         : "inbound RDMA Write, 4096 B (Get Log Page LID 5 effects)");
    }

    // ---- 4. Identify Controller: everything the host must not assume ----
    //
    // First a probe with a CNS no target implements: it needs the same capsule,
    // the same SGL and the same SEND as the real Identify, but it produces an
    // ERROR completion instead of a data transfer.  That separates "the capsule
    // never reaches the target" from "the target's RDMA Write of the data never
    // lands" - two failures with the same visible symptom (no completion).
    {
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, ++cid, 0);
        cap[40] = 0xFE;                       // no such CNS
        uint8_t* scratch = dev.region(kOutOff);
        memset(scratch, 0, NVMEOF_IDENTIFY_SIZE);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)scratch,
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        bool got = submitCommand(admin, dev, cap, &c, kWaitMs);
        char d[96];
        sprintf_s(d, "sct=%u sc=0x%02X", nvmeof_status_sct(c.status), nvmeof_status_sc(c.status));
        Report("probe: an Identify with an unknown CNS is answered (no data transfer)",
               got, got ? d : "no completion at all");
        if (!got) printf("    [diag]  even a no-data-answer Identify gets nothing: the capsule "
                         "is not being processed\n");
    }
    {
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, ++cid, 0);
        cap[40] = NVMEOF_ID_CNS_CTRL;
        uint8_t* idb = dev.region(kOutOff);   // where the target's RDMA Write lands
        memset(idb, 0, NVMEOF_IDENTIFY_SIZE);
        // A buffer the TARGET writes into.  The token is the ordinary published one
        // (Device::open exposes the wire form); what made this data arrive at all was
        // matching the peer's MTU, not the token - see interop_link.ps1.
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)idb,
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        bool sent = submitCommand(admin, dev, cap, &c, kWaitMs);
        bool ok = sent && statusOk(c.status);
        if (!ok) {
            // Which half failed?  The target answers a read-type Identify only after
            // its RDMA WRITE of the 4096-byte identify data has completed, so "no
            // completion" can mean either the write never landed or the response
            // SEND went missing.  The buffer says which - and if the write landed at
            // the wrong address, a scan of the whole region says where.
            const uint8_t* p = idb;
            printf("    [diag]  response %s; identify buffer start %02X %02X %02X %02X "
                   "%02X %02X %02X %02X | subnqn field '%s'\n",
                   sent ? "came back with a status" : "NEVER arrived",
                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                   (const char*)(p + NVMEOF_ID_CTRL_OFF_SUBNQN));
            // The subsystem NQN appears at byte 768 of an Identify Controller and
            // nowhere else in a 64 MiB region of zeros, so finding it locates the
            // target's write exactly.
            static const char kNeedle[] = "nqn.2024-01.local.rdma:linux-nvmet";
            const uint8_t* base = dev.region(0);
            int hits = 0;
            for (size_t off = 0; off + sizeof(kNeedle) < kRegionBytes; off += 8) {
                if (memcmp(base + off, kNeedle, sizeof(kNeedle) - 1) == 0) {
                    printf("    [diag]  the identify payload IS in the region at offset %zu "
                           "(expected %zu)\n", off, (size_t)kIdOff + NVMEOF_ID_CTRL_OFF_SUBNQN);
                    if (++hits >= 3) break;
                }
            }
            if (!hits) printf("    [diag]  the identify payload is nowhere in the %zu MiB region "
                              "- the target's RDMA Write never landed\n", kRegionBytes >> 20);
        }
        Report("Identify Controller", ok, nullptr);
        if (ok) {
            uint16_t idCntlid = nvmeof_rd16(idb + NVMEOF_ID_CTRL_OFF_CNTLID);
            uint16_t kas      = nvmeof_rd16(idb + NVMEOF_ID_CTRL_OFF_KAS);
            uint32_t ioccsz   = nvmeof_rd32(idb + NVMEOF_ID_CTRL_OFF_IOCCSZ);
            uint32_t iorcsz   = nvmeof_rd32(idb + NVMEOF_ID_CTRL_OFF_IORCSZ);
            uint32_t sgls     = nvmeof_rd32(idb + NVMEOF_ID_CTRL_OFF_SGLS);
            uint16_t maxcmd   = nvmeof_rd16(idb + NVMEOF_ID_CTRL_OFF_MAXCMD);
            char sn[21], mn[41], fr[9], nqn[NVMEOF_NQN_FIELD_LEN + 1];
            memcpy(sn, idb + NVMEOF_ID_CTRL_OFF_SN, 20); sn[20] = 0;
            memcpy(mn, idb + NVMEOF_ID_CTRL_OFF_MN, 40); mn[40] = 0;
            memcpy(fr, idb + NVMEOF_ID_CTRL_OFF_FR, 8);  fr[8] = 0;
            memcpy(nqn, idb + NVMEOF_ID_CTRL_OFF_SUBNQN, NVMEOF_NQN_FIELD_LEN);
            nqn[NVMEOF_NQN_FIELD_LEN] = 0;
            // Keep the foreign controller's identity: the iSCSI bridge reports it as
            // the SCSI unit serial (VPD 0x80/0x83), so a disk in Windows' Device
            // Manager can be traced back to the physical drive it really is.
            snprintf(g_foreignSerial, sizeof(g_foreignSerial), "%s", sn);
            snprintf(g_foreignModel, sizeof(g_foreignModel), "%s", mn);
            printf("    vid=0x%04X ssvid=0x%04X sn='%s'\n    mn='%s'\n    fr='%s'\n"
                   "    subnqn='%s'\n    ver=0x%08X mdts=%u sgls=0x%08X\n",
                   nvmeof_rd16(idb + NVMEOF_ID_CTRL_OFF_VID),
                   nvmeof_rd16(idb + NVMEOF_ID_CTRL_OFF_SSVID), sn, mn, fr, nqn,
                   nvmeof_rd32(idb + NVMEOF_ID_CTRL_OFF_VER),
                   (unsigned)idb[NVMEOF_ID_CTRL_OFF_MDTS], sgls);
            char d[224];
            sprintf_s(d, "cntlid=%u (Connect said %u) kas=%u ioccsz=%u iorcsz=%u maxcmd=%u",
                      idCntlid, cntlid, kas, ioccsz, iorcsz, maxcmd);
            // The same checklist nvme_check_ctrl_fabric_info() applies before a
            // Linux host will use the controller at all - and it only applies to an
            // I/O controller: a discovery controller has no I/O queues and therefore
            // reports ioccsz = iorcsz = 0, which is correct there and would fail this
            // check (the first discovery run failed it for exactly that reason).
            if (!discover) {
                Report("the target passes the host's fabrics checklist", 
                       idCntlid == cntlid && kas != 0 && ioccsz >= 4 && iorcsz >= 1 && maxcmd != 0, d);
                Report("the target advertises the keyed SGLs this host sends",
                       (sgls & NVMEOF_CTRL_SGLS_KEYED) != 0, d);
                // THE CAPABILITY BITS ARE A PROMISE, SO READ THEM BACK AND CHECK THEM.
                // ONCS is what makes a host's block layer offer discard and
                // write-zeroes at all; a bit set for a command the target has no case
                // for is worse than no bit, because the host acts on it.  This asserts
                // both halves at once: the two commands we do answer are advertised,
                // and the four we do not are absent (DESIGN 8.74).
                uint16_t oncs = nvmeof_rd16(idb + NVMEOF_ID_CTRL_OFF_ONCS);
                sprintf_s(d, "oncs=0x%04X dsm=%d write-zeroes=%d compare=%d wu=%d resv=%d ts=%d",
                          oncs, (oncs & NVMEOF_CTRL_ONCS_DSM) ? 1 : 0,
                          (oncs & NVMEOF_CTRL_ONCS_WRITE_ZEROES) ? 1 : 0,
                          (oncs & NVMEOF_CTRL_ONCS_COMPARE) ? 1 : 0,
                          (oncs & NVMEOF_CTRL_ONCS_WRITE_UNCOR) ? 1 : 0,
                          (oncs & NVMEOF_CTRL_ONCS_RESERVATIONS) ? 1 : 0,
                          (oncs & NVMEOF_CTRL_ONCS_TIMESTAMP) ? 1 : 0);
                Report("the controller advertises deallocate/write-zeroes, and nothing it cannot answer",
                       (oncs & (NVMEOF_CTRL_ONCS_DSM | NVMEOF_CTRL_ONCS_WRITE_ZEROES)) ==
                               (NVMEOF_CTRL_ONCS_DSM | NVMEOF_CTRL_ONCS_WRITE_ZEROES) &&
                           (oncs & (NVMEOF_CTRL_ONCS_COMPARE | NVMEOF_CTRL_ONCS_WRITE_UNCOR |
                                    NVMEOF_CTRL_ONCS_RESERVATIONS | NVMEOF_CTRL_ONCS_TIMESTAMP)) == 0,
                       d);
            } else {
                printf("    (discovery controller: ioccsz/iorcsz are 0 by design)\n");
            }
        } else {
            dumpStatus("Identify Controller", c);
        }
    }

    // ---- 5. namespace scan: active list (CNS 2), then namespace (CNS 0) ----
    // Skipped for discovery: a discovery controller has no namespaces, and a real
    // `nvme discover` never looks for any.
    uint32_t nsid = 0;
    uint64_t nsze = 0;
    uint8_t  lbads = 0;
    if (!discover) {
        uint8_t* list = dev.region(kIdOff + NVMEOF_IDENTIFY_SIZE);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, ++cid, 0);
        cap[40] = NVMEOF_ID_CNS_NS_ACTIVE_LIST;
        nvmeof_wr32(cap + 4, 0);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)list,
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        bool sent = submitCommand(admin, dev, cap, &c, kWaitMs);
        if (sent && statusOk(c.status)) nsid = nvmeof_rd32(list);
        char d[96];
        sprintf_s(d, "nsid=%u", nsid);
        Report("Identify CNS 2 (active namespace list)", sent && statusOk(c.status) && nsid != 0, d);
        if (!nsid) { printf("\nthe target exposes no namespace; nothing to test\n"); }
    }
    if (nsid) {
        uint8_t* idb = dev.region(kIdOff);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, ++cid, 0);
        cap[40] = NVMEOF_ID_CNS_NS;
        nvmeof_wr32(cap + 4, nsid);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)idb,
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        bool sent = submitCommand(admin, dev, cap, &c, kWaitMs);
        bool ok = sent && statusOk(c.status);
        if (ok) {
            nsze = nvmeof_rd64(idb + NVMEOF_ID_NS_OFF_NSZE);
            // lbaf[0].ds is the low byte of the 4-byte entry at lbaf[0]; the
            // format's metadata size sits in the upper two bytes.
            lbads = (uint8_t)(nvmeof_rd32(idb + NVMEOF_ID_NS_OFF_LBAF) & 0xff);
            if (lbads < 9) lbads = 9;   // only format 0 is described here
        }
        char d[160];
        sprintf_s(d, "nsze=%llu blocks, lbaf[0].ds=%u (%u B)", (unsigned long long)nsze,
                  lbads, 1u << lbads);
        Report("Identify CNS 0 (namespace geometry)", ok && nsze != 0, d);
        // The namespace-side half of the deallocate promise.  ONCS.DSM says "send me
        // a deallocate"; NSFEAT bit 0 says "a deallocated block reads as something
        // defined".  A host wants both before it enables discard on the namespace, so
        // both are checked here rather than assumed from the controller bits.
        if (ok) {
            uint8_t  nsfeat = idb[NVMEOF_ID_NS_OFF_NSFEAT];
            uint8_t  dlfeat = idb[NVMEOF_ID_NS_OFF_DLFEAT];
            uint16_t npdg   = nvmeof_rd16(idb + NVMEOF_ID_NS_OFF_NPDG);
            sprintf_s(d, "nsfeat=0x%02X thin=%d dlfeat=%u npdg=%u (0-based blocks)",
                      nsfeat, (nsfeat & NVMEOF_NS_FEAT_THIN) ? 1 : 0, dlfeat, npdg);
            Report("the namespace advertises thin provisioning (the other half of discard)",
                   (nsfeat & NVMEOF_NS_FEAT_THIN) != 0, d);
        }
        if (ok) printf("    the I/O size below follows the target's own block size\n");
    }

    // ---- 5b. discovery: ask a peer "what is here?" --------------------------
    //
    // A discovery controller is the same admin handshake with the well-known NQN,
    // and the answer is a log page (LID 0x70) that lists what a host may connect to.
    // This is the whole of `nvme discover`, and doing it here is how we read the
    // reference's behaviour from a real target instead of guessing it (DESIGN 8.50).
    if (discover) {
        // Identify CNS 1 must describe a DISCOVERY controller: ctrltype 2, the
        // discovery NQN, no namespaces.  If a target answers with its I/O identity
        // the kernel host rejects it ("Mismatching subnqn").
        uint8_t* idb = dev.region(kIdOff);
        memset(idb, 0, NVMEOF_IDENTIFY_SIZE);
        fabricHeader(cap, NVMEOF_OPC_IDENTIFY, ++cid, 0);
        nvmeof_wr32(cap + 4, 0);
        nvmeof_wr32(cap + 40, NVMEOF_ID_CNS_CTRL);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)idb,
                             NVMEOF_IDENTIFY_SIZE, dev.rkey);
        bool ident = submitCommand(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        const char* idSubnqn = (const char*)idb + NVMEOF_ID_CTRL_OFF_SUBNQN;
        char d2[224];
        sprintf_s(d2, "ctrltype=%u subnqn='%s' nn=%u",
                  idb[NVMEOF_ID_CTRL_OFF_CNTRLTYPE], idSubnqn,
                  (unsigned)nvmeof_rd32(idb + NVMEOF_ID_CTRL_OFF_NN));
        Report("Identify on a discovery controller describes discovery, not I/O",
               ident && idb[NVMEOF_ID_CTRL_OFF_CNTRLTYPE] == NVMEOF_CTRLTYPE_DISC &&
                   strcmp(idSubnqn, NVMEOF_DISC_SUBSYS_NAME) == 0, d2);

        // Read the log page: 1024-byte header first, then every record.  The kernel
        // host reads it in 4096-byte chunks with a log page offset, so the offset
        // has to work - a target that ignores it hands back the first chunk forever.
        //
        // NUMDL is CDW10 bits 31:16 (bytes 42..43) and NUMDU is CDW11 bits 15:0
        // (bytes 44..45).  Writing the length into 44 alone leaves NUMDL at 0, which
        // asks for FOUR bytes; this target answered SGL_INVALID_DATA, and the first
        // version of this loop then printed "numrec=0" for a log page that was there
        // all along (the same trap the LID 1/5 probes hit earlier, DESIGN 8.44).
        uint8_t* hdr = dev.region(kXferOff);
        memset(hdr, 0, 4096);
        uint32_t chunkDwords = (4096 / 4) - 1;
        fabricHeader(cap, NVMEOF_OPC_GET_LOG_PAGE, ++cid, 0);
        cap[40] = NVMEOF_LOG_DISC;
        nvmeof_wr16(cap + 42, (uint16_t)(chunkDwords & 0xffffu));
        nvmeof_wr16(cap + 44, (uint16_t)((chunkDwords >> 16) & 0xffffu));
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)hdr,
                             4096, dev.rkey);
        bool got = submitCommand(admin, dev, cap, &c, kWaitMs) && statusOk(c.status);
        uint64_t numrec = got ? nvmeof_rd64(hdr + 8) : 0;
        uint16_t recfmt = got ? nvmeof_rd16(hdr + 16) : 0xffff;
        printf("  [disc] header: genctr=%llu numrec=%llu recfmt=%u\n",
               (unsigned long long)(got ? nvmeof_rd64(hdr) : 0),
               (unsigned long long)numrec, recfmt);
        sprintf_s(d2, "numrec=%llu recfmt=%u", (unsigned long long)numrec, recfmt);
        Report("Get Log Page LID 0x70 (discovery) is answered", got && recfmt == 0, d2);

        // Walk the records.  The first chunk already holds the first few (the header
        // takes 1024 of its 4096 bytes); the rest need the offset.
        int shown = 0;
        for (uint64_t i = 0; i < numrec && i < 8; i++) {
            uint64_t off = NVMEOF_DISC_HDR_SIZE + i * NVMEOF_DISC_ENTRY_SIZE;
            if (off + NVMEOF_DISC_ENTRY_SIZE > 4096) {
                memset(hdr, 0, 4096);
                fabricHeader(cap, NVMEOF_OPC_GET_LOG_PAGE, ++cid, 0);
                cap[40] = NVMEOF_LOG_DISC;
                nvmeof_wr16(cap + 42, (uint16_t)(chunkDwords & 0xffffu));
                nvmeof_wr16(cap + 44, (uint16_t)((chunkDwords >> 16) & 0xffffu));
                nvmeof_wr32(cap + 48, (uint32_t)(off & 0xffffffffu));
                nvmeof_wr32(cap + 52, (uint32_t)(off >> 32));
                nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)hdr,
                                     4096, dev.rkey);
                if (!(submitCommand(admin, dev, cap, &c, kWaitMs) && statusOk(c.status))) break;
                off = 0;
            }
            const uint8_t* e = hdr + off;
            const char* sub = (const char*)e + NVMEOF_DISC_ENTRY_OFF_SUBNQN;
            const char* adr = (const char*)e + NVMEOF_DISC_ENTRY_OFF_TRADDR;
            const char* svc = (const char*)e + NVMEOF_DISC_ENTRY_OFF_TRSVCID;
            printf("  [disc] #%llu trtype=%u adrfam=%u subtype=%u portid=%u cntlid=%u "
                   "trsvcid='%s' subnqn='%s' traddr='%s'\n",
                   (unsigned long long)i, e[NVMEOF_DISC_ENTRY_OFF_TRTYPE],
                   e[NVMEOF_DISC_ENTRY_OFF_ADRFAM], e[NVMEOF_DISC_ENTRY_OFF_SUBTYPE],
                   nvmeof_rd16(e + NVMEOF_DISC_ENTRY_OFF_PORTID),
                   nvmeof_rd16(e + NVMEOF_DISC_ENTRY_OFF_CNTLID), svc, sub, adr);
            if (*sub) shown++;
        }
        sprintf_s(d2, "%d record(s) with a subsystem NQN", shown);
        Report("the discovery log names at least one connectable subsystem",
               numrec >= 1 && shown >= 1, d2);
    }

    // ---- 6. Set Features: Number of Queues, KATO, Keep Alive ----
    // Skipped for a discovery controller: the kernel host does not ask a discovery
    // controller for I/O queues, and it never creates any for one.
    uint16_t granted = 0;
    if (!discover) {
        fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, ++cid, 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_NUM_QUEUES);
        nvmeof_wr32(cap + 44, (uint32_t)((wantQueues - 1) & 0xffff) |
                              ((uint32_t)((wantQueues - 1) & 0xffff) << 16));
        bool sent = submitCommand(admin, dev, cap, &c, kWaitMs);
        granted = (uint16_t)((c.result & 0xffff) + 1);
        char d[96];
        sprintf_s(d, "asked for %d, granted %u", wantQueues, granted);
        Report("Set Features: Number of Queues", sent && statusOk(c.status), d);

        fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, ++cid, 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_KATO);
        nvmeof_wr32(cap + 44, NVMEOF_KATO_DEFAULT);
        sent = submitCommand(admin, dev, cap, &c, kWaitMs);
        // The answer is in SECONDS in this direction (nvmet_set_feat_kato) and in
        // MILLISECONDS in the other (nvmet_get_feat_kato).  Accept either unit
        // rather than demanding the reference's, but notice a value that is
        // neither: this is the field a host uses to schedule Keep Alive.
        uint64_t katoAnswer = sent ? c.result : 0;
        bool plausible = katoAnswer == NVMEOF_KATO_DEFAULT ||      // ms, our target
                         katoAnswer == NVMEOF_KATO_DEFAULT / 1000; // s, nvmet
        char d2[128];
        sprintf_s(d2, "answer=%llu (%s)", (unsigned long long)katoAnswer,
                  katoAnswer == NVMEOF_KATO_DEFAULT ? "ms" :
                  (katoAnswer == NVMEOF_KATO_DEFAULT / 1000 ? "s" : "neither unit"));
        Report("Set Features: Keep Alive Timer is accepted", sent && statusOk(c.status), d2);
        Report("the KATO answer is in one of the two units the reference uses",
               sent && statusOk(c.status) && plausible, d2);

        fabricHeader(cap, NVMEOF_OPC_KEEP_ALIVE, ++cid, 0);
        sent = submitCommand(admin, dev, cap, &c, kWaitMs);
        Report("Keep Alive", sent && statusOk(c.status), nullptr);
    }

    // ---- 5b. Get Features: the fids a real `nvme get-feature` sweep asks for ----
    //
    // This exists because a real Linux host asked our target four questions it could
    // not answer (target summary `unknownAdmin=4`): volatile write cache (0x06, asked
    // twice), temperature threshold (0x04) and async event configuration (0x0b).  A
    // target that answers Invalid Field to a Get Feature that nvmet implements is
    // incompatible in a way nothing else in this suite notices - the host logs a
    // warning and carries on.
    //
    // The expectations are the reference's own, not this project's:
    //   * 0x06 -> SUCCESS with result 1, a constant in nvmet (`nvmet_set_result(req, 1)`)
    //   * 0x0b -> SUCCESS; the value round-trips, so a Get after a Set returns it
    //   * 0x04 -> Invalid Field | DNR; nvmet's switch has `#if 0 case NVME_FEAT_TEMP_THRESH`
    //   * 0x07 and 0x0f -> SUCCESS (Number of Queues and Keep Alive Timeout)
    //
    // ON A DISCOVERY CONTROLLER THE EXPECTATION FLIPS, and getting that wrong is a
    // mistake this project has now made three times (DESIGN 8.50(4), then the log pages,
    // then here): a discovery controller has no write cache, no namespaces and no
    // event mask, so nvmet refuses all four.  The discovery session against Linux
    // reported three FAILs on a perfectly good peer because of it - the checks below
    // run against a controller that is not an I/O controller, and a failing check
    // that cannot pass is worse than no check: it makes a correct peer look broken.
    {
        auto getFeature = [&](uint8_t fid, Completion* out) -> bool {
            fabricHeader(cap, NVMEOF_OPC_GET_FEATURES, ++cid, 0);
            nvmeof_wr32(cap + 40, fid);
            return submitCommand(admin, dev, cap, out, kWaitMs);
        };
        Completion fc = {};
        char dv[96];

        // 0x04 is refused by both kinds of controller, so it is checked in both - and
        // checked ONCE: an earlier revision of this edit left the original check in
        // the I/O-controller branch as well, so a passing run printed the same line
        // twice.  Two identical reports is how a reader learns to skim them.
        bool ok4 = getFeature(NVMEOF_FID_TEMP_THRESH, &fc);
        sprintf_s(dv, "sct=%u sc=0x%02X%s", nvmeof_status_sct(fc.status),
                  nvmeof_status_sc(fc.status),
                  (fc.status & NVMEOF_STATUS_DNR) ? " DNR" : "");
        Report("Get Features 0x04 is refused with Invalid Field | DNR, as nvmet",
               ok4 && nvmeof_status_sct(fc.status) == NVMEOF_SCT_GENERIC &&
               nvmeof_status_sc(fc.status) == NVMEOF_SC_INVALID_FIELD &&
               (fc.status & NVMEOF_STATUS_DNR) != 0, dv);

        if (discover) {
            // Measure, do not assume: assert the refusal, which is what nvmet does.
            bool r1 = getFeature(NVMEOF_FID_VWC, &fc);
            bool refused1 = r1 && !statusOk(fc.status);
            bool r2 = getFeature(NVMEOF_FID_NUM_QUEUES, &fc);
            bool refused2 = r2 && !statusOk(fc.status);
            sprintf_s(dv, "sc=0x%02X / sc=0x%02X", nvmeof_status_sc(fc.status),
                      nvmeof_status_sc(fc.status));
            Report("a discovery controller refuses the I/O-controller features "
                   "(0x06, 0x07) - as nvmet", refused1 && refused2, dv);
            printf("    (the Async Event Configuration round trip is skipped: 0x0b is "
                   "one of the features a discovery controller refuses)\n");
        } else {
        bool ok1 = getFeature(NVMEOF_FID_VWC, &fc) && statusOk(fc.status);
        sprintf_s(dv, "result=%llu (nvmet answers the constant 1)",
                  (unsigned long long)fc.result);
        Report("Get Features 0x06 Volatile Write Cache is answered", ok1 && fc.result == 1, dv);

        // Async Event Configuration: set it, read it back.  The reference returns the
        // value from the Set as well, which is what makes the round trip checkable
        // without depending on how the host's own AEN bits were initialised.
        const uint32_t kAenProbe = 0x00000004u;   // NS_ATTR_CHANGED
        fabricHeader(cap, NVMEOF_OPC_SET_FEATURES, ++cid, 0);
        nvmeof_wr32(cap + 40, NVMEOF_FID_ASYNC_EVENT);
        nvmeof_wr32(cap + 44, kAenProbe);
        Completion sc2 = {};
        bool ok2 = submitCommand(admin, dev, cap, &sc2, kWaitMs) && statusOk(sc2.status) &&
                   (uint32_t)sc2.result == kAenProbe;
        bool ok3 = getFeature(NVMEOF_FID_ASYNC_EVENT, &fc) && statusOk(fc.status) &&
                   (uint32_t)fc.result == kAenProbe;
        sprintf_s(dv, "set->0x%08X read-back->0x%08X",
                  (unsigned)sc2.result, (unsigned)fc.result);
        Report("Get Features 0x0b Async Event Configuration round-trips", ok2 && ok3, dv);

        // (0x04 is checked once, above the if/else: both kinds of controller refuse it.)

        // And the two the target already implemented must still answer: a sweep that
        // only checks the three new fids would pass on a target that broke the old two.
        bool ok5 = getFeature(NVMEOF_FID_NUM_QUEUES, &fc) && statusOk(fc.status);
        bool ok6 = getFeature(NVMEOF_FID_KATO, &fc) && statusOk(fc.status);
        Report("Get Features 0x07 and 0x0f still answer after the sweep", ok5 && ok6, nullptr);
        }   // end of the I/O-controller feature sweep
    }

    // ---- 6b. Async Event: the command whose correct answer is silence ----
    //
    // A controller that answers an Async Event command has misunderstood it: the
    // command is a place for the controller to park a completion until it has
    // something to report.  Three properties are checked, in the order they can
    // be observed:
    //   * four accepted Async Events produce NO completion - and the four capsules
    //     are sent back to back, which is also what tests the target's receive
    //     RING: with a single Receive armed the later capsules are simply lost
    //     (this test is what found that);
    //   * the fifth is refused with ASYNC_LIMIT, the reference's limit, and that
    //     one IS answered - so a response receive must be posted for it.  Sending
    //     a response into a queue pair with no Receive posts is not a "lost
    //     message": the SEND exhausts its RNR retries and the QP goes to error,
    //     which flushes every Receive on it.  (Learned by doing it.)
    //   * with four commands parked, the admin queue still works.
    {
        bool posted = admin.postReceive(dev.region(kRespInOff), 16, CTX_RESP);
        Report("posted the one response receive this burst will need", posted, nullptr);

        // Four: accepted and held.
        for (int i = 0; i < 4; i++) {
            fabricHeader(cap, NVMEOF_OPC_ASYNC_EVENT, ++cid, 0);
            // One host buffer per capsule: five sends from one buffer would race
            // the NIC reading it, and two capsules could leave with the same cid.
            uint8_t* out = dev.region(kAdminCapOff + (size_t)i * 64);
            memcpy(out, cap, 64);
            if (!admin.send(out, 64, CTX_CAP)) { Report("send Async Event", 0, nullptr); break; }
        }
        for (int i = 0; i < 4; i++) {
            ND2_RESULT r = {};
            admin.reap(CTX_CAP, &r, kWaitMs);      // the capsule sends retiring
        }
        bool sawResponse = false;
        ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 700) {
            ND2_RESULT r = {};
            if (admin.poll(&r)) {
                if (r.RequestContext == CTX_RESP) { sawResponse = true; break; }
                continue;                          // a late send completion
            }
            SwitchToThread();
        }
        Report("four accepted Async Events are held, not answered", !sawResponse, nullptr);

        // Fifth: refused, and answered.
        fabricHeader(cap, NVMEOF_OPC_ASYNC_EVENT, ++cid, 0);
        uint8_t* fifthOut = dev.region(kAdminCapOff + 4 * 64);
        memcpy(fifthOut, cap, 64);
        bool fifthSent = admin.send(fifthOut, 64, CTX_CAP);
        ND2_RESULT fifth = {};
        bool gotFifth = false;
        t0 = GetTickCount64();
        while (GetTickCount64() - t0 < kWaitMs) {
            if (admin.poll(&fifth)) {
                if (fifth.RequestContext == CTX_RESP) { gotFifth = true; break; }
                continue;
            }
            SwitchToThread();
        }
        // The response payload is the completion, so read the status out of the
        // buffer the receive was posted into.
        uint16_t fifthStatus = gotFifth ? nvmeof_rd16(dev.region(kRespInOff) + 14) : 0;
        char d[128];
        sprintf_s(d, "sct=%u sc=0x%02X", nvmeof_status_sct(fifthStatus),
                  nvmeof_status_sc(fifthStatus));
        Report("the fifth outstanding Async Event is refused with ASYNC_LIMIT",
               fifthSent && gotFifth && statusOk(fifthStatus) == false &&
                   nvmeof_status_sct(fifthStatus) == NVMEOF_SC_ASYNC_LIMIT_SCT &&
                   nvmeof_status_sc(fifthStatus) == NVMEOF_SC_ASYNC_LIMIT_SC, d);

        // And with four commands parked, the admin queue still works.
        fabricHeader(cap, NVMEOF_OPC_KEEP_ALIVE, ++cid, 0);
        bool sent = submitCommand(admin, dev, cap, &c, kWaitMs);
        Report("Keep Alive still works with four Async Events outstanding",
               sent && statusOk(c.status), nullptr);
    }

    // ---- 7. one queue pair per I/O queue, each with its own fabrics Connect ----
    //
    // The number to create is the number the target granted (never more than it
    // answered): a host that creates a queue the target does not have gets a dead
    // connection, and on a real host that is a controller that never becomes ready.
    int ioUp = 0;
    int nIoWanted = (wantQueues < (int)kMaxIoQueues) ? wantQueues : (int)kMaxIoQueues;
    if (granted > 0 && nIoWanted > (int)granted) nIoWanted = (int)granted;
    for (int qi = 0; qi < nIoWanted; qi++) {
        Queue& q = io[qi];
        nvmeof_rdma_request_pd pd;
        // The 4th field is the controller id, and THIS is where it travels for an
        // I/O queue: `struct nvmf_connect_command` has no cntlid field, the CM
        // private data does (nvme_rdma_connect() fills it from ctrl->cntlid).
        nvmeof_rdma_fill_req(&pd, (uint16_t)(qi + 1), kSqDepth, cntlid);
        char b[64];
        HRESULT h = q.conn->Bind((const sockaddr*)&local, sizeof(local));
        if (h == ND_PENDING) h = q.waitOverlapped(q.conn, kWaitMs);
        h = q.conn->Connect(q.qp, (const sockaddr*)&remote, sizeof(remote),
                            kReadLimit, kReadLimit, &pd, sizeof(pd), &q.ov);
        printf("  [io] Connect qid %d -> %s\n", qi + 1, ndStr(h, b, sizeof(b)));
        if (h == ND_PENDING) h = q.waitOverlapped(q.conn, 20000);
        if (ndOk(h)) {
            h = q.conn->CompleteConnect(&q.ov);
            if (h == ND_PENDING) h = q.waitOverlapped(q.conn, kWaitMs);
        }
        if (qi == 0) {
            Report("I/O queue pair connected", ndOk(h), ndOk(h) ? nullptr : ndStr(h, b, sizeof(b)));
        }
        if (!ndOk(h)) {
            char d2[96];
            sprintf_s(d2, "queue %d of %d: %s", qi + 1, nIoWanted, ndStr(h, b, sizeof(b)));
            Report("every I/O queue pair this host asked for connected", 0, d2);
            break;
        }
        uint8_t* cd = dev.region(kConnectOff);
        memset(cd, 0, 64);
        nvmeof_wr16(cd + 16, cntlid);
        memcpy(cd + 256, connectNqn, strlen(connectNqn));
        memcpy(cd + 512, hostnqn, strlen(hostnqn));
        fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_CONNECT);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)cd,
                             NVMEOF_CONNECT_DATA_SIZE, dev.rkey);
        nvmeof_wr16(cap + 42, (uint16_t)(qi + 1));
        nvmeof_wr16(cap + 44, kSqSize);
        nvmeof_wr32(cap + 48, NVMEOF_KATO_DEFAULT);
        bool sent = submitCommand(q, dev, cap, &c, kWaitMs);
        if (!sent || !statusOk(c.status)) dumpStatus("Connect on the I/O queue", c);
        if (qi == 0) {
            Report("fabrics Connect qid 1 on its own queue pair", sent && statusOk(c.status), nullptr);
        }
        if (!sent || !statusOk(c.status)) break;
        ioUp = qi + 1;
    }
    if (nIoWanted > 1) {
        char d2[96];
        sprintf_s(d2, "%d of %d I/O queues established", ioUp, nIoWanted);
        Report("a multi-queue controller: every I/O queue has its own Connect",
               ioUp == nIoWanted, d2);
    }

    // ---- iSCSI bridge mode: serve Windows' own iSCSI initiator ----
    //
    // Placed with the other early modes, before the self-tests, because those write
    // to the namespace - and the namespace may be somebody's 1 TB disk.
    if (g_iscsiPort && nsid && nsze) {
        printf("\n-- iSCSI BRIDGE\n");
        printf("    backend : NVMe-oF namespace nsid=%u, %llu blocks x %u B = %.2f GiB\n",
               nsid, (unsigned long long)nsze, 1u << lbads,
               (double)nsze * (1u << lbads) / 1073741824.0);
        printf("    identity: serial '%s', model '%s'\n", g_foreignSerial, g_foreignModel);
        printf("    access  : %s\n", g_iscsiReadWrite ? "READ-WRITE (-iscsirw)"
                                                          : "READ-ONLY (default)");
        IscsiTarget target;
        char iqn[160];
        snprintf(iqn, sizeof(iqn), "iqn.2024-01.com.nvmeof:bridge0");
        if (!target.listenLoopback((uint16_t)g_iscsiPort, iqn, g_iscsiChunk, g_iscsiBind)) {
            printf("    cannot listen; stopping\n");
            dev.close(nullptr, nullptr, 0);
            return 1;
        }
        NvmeIscsiBackend backend(io[0], admin, dev, nsid, nsze, 1u << lbads, cap, &cid, &c,
                                 dev.region(kIscsiOff), g_iscsiReadWrite,
                                 NVMEOF_KATO_DEFAULT);
        std::vector<uint8_t> scratch(kIscsiBytes);
        printf("\n    On Windows, in another window:\n");
        printf("      New-IscsiTargetPortal -TargetPortalAddress 127.0.0.1\n");
        printf("      $t = Get-IscsiTarget | Where-Object NodeAddress -like '*nvmeof-bridge*'\n");
        printf("      Connect-IscsiTarget -NodeAddress $t.NodeAddress -IsPersistent $false\n");
        int rc = 0;
        {
            // Publish the bridge so the service control handler can stop it.  Without
            // this a service stop would kill the process instead of ending the accept
            // loop, and the sessions Windows still holds would see a hard reset.
            g_svcBridge = &target;
            rc = target.run(backend, scratch);
            g_svcBridge = nullptr;
        }
        // A LOST BACKEND IS AN EXIT, not a log line.  Measured before this: with the
        // NVMe-oF target gone, the bridge kept the iSCSI session alive and answered
        // every SCSI command with a hard error forever, and a restart of the backend
        // changed nothing (the queue pairs are dead - ND_CANCELED on every submit), so
        // the disk never came back without restarting the bridge.  Exit non-zero so the
        // SCM restarts the service, which reconnects through -backendretry; a
        // user-requested stop must still exit 0, or the restart policy would fight it.
        if (g_backendLost && !InterlockedCompareExchange(&g_svcStopRequested, 0, 0)) {
            printf("\n  [iscsi] exiting because the backend is gone (the service will restart "
                   "and reconnect)\n");
            rc = 1;
        }
        for (uint16_t q = 0; q < kMaxIoQueues; q++) io[q].destroy();
        dev.close(nullptr, nullptr, 0);
        printf("\ninitiator failures: %d\n", g_failures + (rc ? 1 : 0));
        return g_failures + (rc ? 1 : 0);
    }

    // ---- survey mode: describe the namespace and stop, without writing to it ----
    //
    // Placed BEFORE the volume-mode block because a survey is the safe thing to run
    // against a real disk: volume mode would also be read-only for -dump, but survey
    // is the mode that exists purely to answer "what is this, and is it the disk I
    // think it is".
    if (g_survey && nsid && nsze) {
        int rc = surveyDisk(io[0], dev, nsid, nsze, 1u << lbads, cap, cid, c);
        for (uint16_t q = 0; q < kMaxIoQueues; q++) io[q].destroy();
        dev.close(nullptr, nullptr, 0);
        printf("\ninitiator failures: %d\n", g_failures + rc);
        return g_failures + rc;
    }

    // ---- volume mode: move the WHOLE namespace and stop here ----
    //
    // A filesystem transfer must not leave fingerprints, and every test below this
    // point WRITES to the namespace - the 8-block probe at LBA 0, the multi-queue
    // patterns (`k * 17 + q * 31`), the write-zeroes/DSM probes.  So when -dump or
    // -push is given, the move happens FIRST and the probes are skipped entirely.
    //
    // This was not a design choice, it was a bug found by reading the medium back:
    // the push reported "every command succeeded", and Linux then read a namespace
    // whose block 0 held `00 11 22 33 44 55 ...` - the multi-queue test's pattern,
    // which had overwritten the volume's boot sector a few hundred lines later.  A
    // volume tool that runs an I/O self-test on the volume is not a volume tool.
    if ((g_dumpFile || g_pushFile) && nsid && nsze) {
        const uint32_t chunkBlocks = 128;                 // 64 KiB at 512 B
        // The namespace's logical block size, straight out of Identify
        // (`lbaf[0].ds` -> lbads).  A volume move that assumed 512 would corrupt any
        // namespace with a different format, and this runs before the I/O section
        // that declares its own copy.
        uint32_t blockSize = 1u << lbads;
        if (blockSize != 512 && blockSize != 4096) {
            printf("\n-- volume mode: namespace reports a %u-byte logical block, which "
                   "this initiator does not move\n", blockSize);
            printf("initiator failures: %d\n", g_failures + 1);
            dev.close(nullptr, nullptr, 0);
            return g_failures + 1;
        }
        uint64_t total = (uint64_t)nsze * blockSize;
        const char* path = g_dumpFile ? g_dumpFile : g_pushFile;
        bool isDump = (g_dumpFile != nullptr);
        printf("\n-- %s the whole namespace (%llu blocks x %u B = %llu bytes)\n",
               isDump ? "READING OUT" : "WRITING BACK", (unsigned long long)nsze,
               blockSize, (unsigned long long)total);
        printf("    local file: %s\n    chunk: %u blocks (%u bytes) per command\n",
               path, chunkBlocks, chunkBlocks * blockSize);

        HANDLE h = CreateFileA(path, isDump ? GENERIC_WRITE : GENERIC_READ,
                               isDump ? 0 : FILE_SHARE_READ, nullptr,
                               isDump ? CREATE_ALWAYS : OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = true;
        if (h == INVALID_HANDLE_VALUE) {
            printf("    cannot open '%s' (error %lu)\n", path, GetLastError());
            ok = false;
        } else {
            LARGE_INTEGER fsz = {};
            GetFileSizeEx(h, &fsz);
            if (!isDump && (uint64_t)fsz.QuadPart != total) {
                printf("    '%s' is %lld bytes but the namespace is %llu - a partial "
                       "image is refused rather than written\n", path,
                       (long long)fsz.QuadPart, (unsigned long long)total);
                ok = false;
            }
        }
        if (h != INVALID_HANDLE_VALUE && ok) {
            uint8_t* buf = dev.region(kXferOff);
            uint64_t done = 0;
            ULONGLONG t0 = GetTickCount64();
            while (done < nsze) {
                uint32_t n = (uint32_t)((nsze - done) < chunkBlocks ? (nsze - done)
                                                                   : chunkBlocks);
                uint32_t nb = n * blockSize;
                DWORD moved = 0;
                if (!isDump) {
                    if (!ReadFile(h, buf, nb, &moved, nullptr) || moved != nb) {
                        printf("    short read from '%s' at block %llu\n", path,
                               (unsigned long long)done);
                        ok = false;
                        break;
                    }
                }
                fabricHeader(cap, isDump ? NVMEOF_OPC_READ : NVMEOF_OPC_WRITE, ++cid, 0);
                nvmeof_wr32(cap + 4, nsid);
                nvmeof_wr64(cap + 40, done);
                nvmeof_wr32(cap + 48, n - 1);
                nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24),
                                     (uint64_t)(uintptr_t)buf, nb, dev.rkey);
                if (!submitCommand(io[0], dev, cap, &c, kWaitMs) || !statusOk(c.status)) {
                    dumpStatus(isDump ? "READ" : "WRITE", c);
                    ok = false;
                    break;
                }
                if (isDump) {
                    if (!WriteFile(h, buf, nb, &moved, nullptr) || moved != nb) {
                        printf("    short write to '%s' at block %llu (error %lu)\n",
                               path, (unsigned long long)done, GetLastError());
                        ok = false;
                        break;
                    }
                }
                done += n;
                if ((done % 2048) == 0 || done == nsze)
                    printf("    %llu / %llu blocks (%.0f%%)\n",
                           (unsigned long long)done, (unsigned long long)nsze,
                           100.0 * (double)done / (double)nsze);
            }
            if (isDump) {
                FlushFileBuffers(h);
            } else if (ok) {
                // A push that is not flushed is a push the peer may lose on a power
                // cut; the whole point is that the bytes are on the medium.
                fabricHeader(cap, NVMEOF_OPC_FLUSH, ++cid, 0);
                nvmeof_wr32(cap + 4, nsid);
                if (!submitCommand(io[0], dev, cap, &c, kWaitMs) || !statusOk(c.status)) {
                    dumpStatus("FLUSH", c);
                    ok = false;
                }
            }
            CloseHandle(h);
            double secs = (double)(GetTickCount64() - t0) / 1000.0;
            char d[192];
            sprintf_s(d, "%llu bytes in %.2f s (%.1f MiB/s), %s",
                      (unsigned long long)total, secs,
                      secs > 0 ? ((double)total / 1048576.0) / secs : 0.0,
                      ok ? "every command succeeded" : "STOPPED EARLY");
            Report(isDump ? "the whole namespace was read out over NVMe-oF"
                          : "the local file was written back over NVMe-oF", ok, d);
        } else {
            Report(isDump ? "the whole namespace was read out over NVMe-oF"
                          : "the local file was written back over NVMe-oF", false,
                   "the local file could not be used");
        }
        printf("\n-- volume mode: the I/O self-tests are skipped on purpose (they write "
               "to the namespace)\n");
        for (uint16_t q = 0; q < kMaxIoQueues; q++) io[q].destroy();
        dev.close(nullptr, nullptr, 0);
        printf("\ninitiator failures: %d\n", g_failures);
        return g_failures;
    }

    // ---- 8. I/O: write, read back, compare; then Flush ----
    if (ioUp && nsid && nsze) {
        uint32_t blockSize = 1u << lbads;
        if (blocks == 0) blocks = 8;
        if (blocks > nsze) blocks = (uint32_t)nsze;
        uint32_t bytes = blocks * blockSize;
        if (bytes > 1024u * 1024) { bytes = 1024u * 1024; blocks = bytes / blockSize; }
        uint8_t* buf = dev.region(kXferOff);
        for (uint32_t i = 0; i < bytes; i++) buf[i] = (uint8_t)((i * 31u + 7u) ^ 0x5A);

        char d[192];
        sprintf_s(d, "%u blocks x %u B = %u B at slba 0", blocks, blockSize, bytes);
        printf("\n-- I/O against the foreign target\n    %s\n", d);

        fabricHeader(cap, NVMEOF_OPC_WRITE, ++cid, 0);
        nvmeof_wr32(cap + 4, nsid);
        nvmeof_wr64(cap + 40, 0);
        nvmeof_wr32(cap + 48, blocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)buf, bytes, dev.rkey);
        bool w = submitCommand(io[0], dev, cap, &c, kWaitMs) && statusOk(c.status);
        if (!w) dumpStatus("WRITE", c);
        Report("WRITE accepted", w, d);

        for (uint32_t i = 0; i < bytes; i++) buf[i] = 0xA5;
        fabricHeader(cap, NVMEOF_OPC_READ, ++cid, 0);
        nvmeof_wr32(cap + 4, nsid);
        nvmeof_wr64(cap + 40, 0);
        nvmeof_wr32(cap + 48, blocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)buf, bytes, dev.rkey);
        bool r = submitCommand(io[0], dev, cap, &c, kWaitMs) && statusOk(c.status);
        if (!r) dumpStatus("READ", c);
        uint32_t bad = 0, firstBad = 0;
        for (uint32_t i = 0; i < bytes; i++) {
            if (buf[i] != (uint8_t)((i * 31u + 7u) ^ 0x5A)) {
                if (bad == 0) firstBad = i;
                bad++;
            }
        }
        sprintf_s(d, "%u of %u bytes differ%s", bad, bytes,
                  bad ? "" : " (the foreign target stored and returned them)");
        if (bad) printf("    first difference at byte %u: got 0x%02X want 0x%02X\n",
                        firstBad, buf[firstBad], (uint8_t)((firstBad * 31u + 7u) ^ 0x5A));
        Report("READ returned exactly the bytes WRITE sent", r && bad == 0, d);

        fabricHeader(cap, NVMEOF_OPC_FLUSH, ++cid, 0);
        nvmeof_wr32(cap + 4, nsid);
        bool f = submitCommand(io[0], dev, cap, &c, kWaitMs) && statusOk(c.status);
        if (!f) dumpStatus("FLUSH", c);
        Report("FLUSH is answered", f, nullptr);

        // An out-of-range transfer must be REFUSED, not silently clamped: the
        // status code is part of the protocol, and a target that answers success
        // to an impossible command is worse than one that errors.
        fabricHeader(cap, NVMEOF_OPC_READ, ++cid, 0);
        nvmeof_wr32(cap + 4, nsid);
        nvmeof_wr64(cap + 40, nsze + 1024);          // past the end, deliberately
        nvmeof_wr32(cap + 48, blocks - 1);
        nvmeof_sgl_set_keyed((nvmeof_sgl*)(cap + 24), (uint64_t)(uintptr_t)buf, bytes, dev.rkey);
        bool oor = submitCommand(io[0], dev, cap, &c, kWaitMs);
        char d3[160];
        sprintf_s(d3, "sct=%u sc=0x%02X (%s)", nvmeof_status_sct(c.status),
                  nvmeof_status_sc(c.status),
                  statusOk(c.status) ? "SERVED - wrong" : "refused");
        // The rule is "refused, not served" - the exact code is the target's to
        // choose.  nvmet answers INTERNAL (0x06) here because its backing store
        // fails the I/O, where our own target answers LBA Range (0x80); both are
        // refusals and a real host handles either.  Asserting one of them is how a
        // test starts encoding one implementation instead of the rule.
        Report("an out-of-range LBA is refused with a status, not served",
               oor && !statusOk(c.status), d3);
    }

    // ---- 8b. pipelined I/O: several commands in flight, the way a real host runs
    //
    // This is the shape a Linux host uses - it does not send one command and wait.
    // It is also what tests the target's I/O receive RING: with a single Receive
    // armed, the second and later capsules of a burst are lost, and the loss shows
    // up as a timeout on the host rather than as an error on the target.
    if (ioUp && nsid && nsze) {
        // Exactly the window the target advertises (crqsize = 32): a host is
        // entitled to use all of it, so the test does.  The old ring held 8 and
        // this burst would have lost 24 capsules - silently, on the target side.
        const int kFlight = (int)kSqDepth;
        uint32_t blockSize = 1u << lbads;
        uint32_t pblocks = 8;
        uint32_t pbytes = pblocks * blockSize;
        if (pblocks * kFlight > nsze) pblocks = (uint32_t)(nsze / kFlight);
        pbytes = pblocks * blockSize;

        // One response buffer per in-flight command.  Posting kFlight Receives into
        // ONE 16-byte buffer would have every answer overwrite the previous one,
        // and the statuses read below would be whichever arrived last.
        for (int i = 0; i < kFlight; i++) {
            io[0].postReceive(dev.region(kRespInOff + (size_t)i * 16), 16,
                           (void*)(CTX_HOST_RESP_BASE + (uintptr_t)i));
        }
        for (int i = 0; i < kFlight; i++) {
            uint8_t* b = dev.region(kXferOff + (size_t)i * pbytes);
            for (uint32_t k = 0; k < pbytes; k++) b[k] = (uint8_t)(k * 17u + i * 31u);
            uint8_t* cc = dev.region(kAdminCapOff + (size_t)i * 64);
            fabricHeader(cc, NVMEOF_OPC_WRITE, ++cid, 0);
            nvmeof_wr32(cc + 4, nsid);
            nvmeof_wr64(cc + 40, (uint64_t)i * pblocks);
            nvmeof_wr32(cc + 48, pblocks - 1);
            nvmeof_sgl_set_keyed((nvmeof_sgl*)(cc + 24), (uint64_t)(uintptr_t)b, pbytes, dev.rkey);
            io[0].send(cc, 64, CTX_CAP);
        }
        int answers = 0, okAnswers = 0;
        ULONGLONG t0 = GetTickCount64();
        while (answers < kFlight && GetTickCount64() - t0 < kWaitMs) {
            ND2_RESULT r = {};
            if (io[0].poll(&r)) {
                uintptr_t rctx = (uintptr_t)r.RequestContext;
                if (rctx >= CTX_HOST_RESP_BASE && rctx < CTX_HOST_RESP_BASE + (uintptr_t)kFlight) {
                    answers++;
                    uint16_t st = nvmeof_rd16(dev.region(kRespInOff + (rctx - CTX_HOST_RESP_BASE) * 16) + 14);
                    // Success is (SCT generic, SC 0) - NOT "phase bit set".  Linux
                    // reads a fabrics completion's status as `status >> 1` and never
                    // looks at bit 0; nvmet leaves it clear, so requiring it here
                    // reported "32 answered, 0 with success" for a burst that was
                    // entirely successful.
                    if (statusOk(st)) okAnswers++;
                }
                continue;
            }
            SwitchToThread();
        }
        char d[160];
        sprintf_s(d, "%d of %d commands answered, %d with success", answers, kFlight, okAnswers);
        Report("a pipelined I/O burst is answered in full (target receive ring)",
               answers == kFlight && okAnswers == kFlight, d);

        for (int i = 0; i < kFlight; i++) {
            io[0].postReceive(dev.region(kRespInOff + (size_t)i * 16), 16,
                           (void*)(CTX_HOST_RESP_BASE + (uintptr_t)i));
        }
        for (int i = 0; i < kFlight; i++) {
            uint8_t* b = dev.region(kXferOff + (size_t)i * pbytes);
            uint8_t* cc = dev.region(kAdminCapOff + (size_t)i * 64);
            fabricHeader(cc, NVMEOF_OPC_READ, ++cid, 0);
            nvmeof_wr32(cc + 4, nsid);
            nvmeof_wr64(cc + 40, (uint64_t)i * pblocks);
            nvmeof_wr32(cc + 48, pblocks - 1);
            nvmeof_sgl_set_keyed((nvmeof_sgl*)(cc + 24), (uint64_t)(uintptr_t)b, pbytes, dev.rkey);
            io[0].send(cc, 64, CTX_CAP);
        }
        answers = 0;
        t0 = GetTickCount64();
        while (answers < kFlight && GetTickCount64() - t0 < kWaitMs) {
            ND2_RESULT r = {};
            if (io[0].poll(&r)) {
                uintptr_t rctx = (uintptr_t)r.RequestContext;
                if (rctx >= CTX_HOST_RESP_BASE && rctx < CTX_HOST_RESP_BASE + (uintptr_t)kFlight) answers++;
                continue;
            }
            SwitchToThread();
        }
        uint32_t pbad = 0;
        for (int i = 0; i < kFlight; i++) {
            uint8_t* b = dev.region(kXferOff + (size_t)i * pbytes);
            for (uint32_t k = 0; k < pbytes; k++) {
                if (b[k] != (uint8_t)(k * 17u + i * 31u)) pbad++;
            }
        }
        sprintf_s(d, "%d of %d answered, %u bytes differ", answers, kFlight, pbad);
        Report("a pipelined read burst returns every command's own data",
               answers == kFlight && pbad == 0, d);
    }

    // ---- 8c. multi-queue: one command in flight per queue, all at the same time
    //
    // This is what a multi-queue controller is FOR, and it is the only test that
    // can tell "eight queues connected" apart from "eight queues used": a host that
    // connects eight and sends everything down the first looks identical in a
    // connection log.  Each queue gets its own capsule buffer, its own response
    // buffer and its own result check, so a target that mixes queues up (or serves
    // one queue's completion with another's data) fails here and nowhere else.
    if (ioUp > 1 && nsid && nsze) {
        uint32_t blockSize = 1u << lbads;
        uint32_t qblocks = 8;
        if ((uint64_t)qblocks * ioUp > nsze) qblocks = (uint32_t)(nsze / ioUp);
        uint32_t qbytes = qblocks * blockSize;
        printf("\n-- multi-queue: %d queues, %u B each in flight simultaneously\n", ioUp, qbytes);

        // Distinct data per queue, so a completion that lands on the wrong queue is
        // caught by the data check and not just by the count.
        for (int qi = 0; qi < ioUp; qi++) {
            uint8_t* b = dev.region(kXferOff + (size_t)qi * qbytes);
            for (uint32_t k = 0; k < qbytes; k++) b[k] = (uint8_t)(k * 13u + qi * 71u + 5u);
            io[qi].postReceive(dev.region(kRespInOff + (size_t)qi * 16), 16,
                               (void*)(CTX_HOST_RESP_BASE + (uintptr_t)qi));
            uint8_t* cc = dev.region(kAdminCapOff + (size_t)qi * 64);
            fabricHeader(cc, NVMEOF_OPC_WRITE, ++cid, 0);
            nvmeof_wr32(cc + 4, nsid);
            nvmeof_wr64(cc + 40, (uint64_t)qi * qblocks);
            nvmeof_wr32(cc + 48, qblocks - 1);
            nvmeof_sgl_set_keyed((nvmeof_sgl*)(cc + 24), (uint64_t)(uintptr_t)b, qbytes, dev.rkey);
            io[qi].send(cc, 64, CTX_CAP);
        }
        int qanswered = 0, qok = 0;
        ULONGLONG tq = GetTickCount64();
        while (qanswered < ioUp && GetTickCount64() - tq < kWaitMs) {
            bool any = false;
            for (int qi = 0; qi < ioUp; qi++) {
                ND2_RESULT r = {};
                if (!io[qi].poll(&r)) continue;
                any = true;
                if ((uintptr_t)r.RequestContext >= CTX_HOST_RESP_BASE &&
                    (uintptr_t)r.RequestContext < CTX_HOST_RESP_BASE + (uintptr_t)ioUp) {
                    uintptr_t slot = (uintptr_t)r.RequestContext - CTX_HOST_RESP_BASE;
                    qanswered++;
                    uint16_t stt = nvmeof_rd16(dev.region(kRespInOff + slot * 16) + 14);
                    if (statusOk(stt)) qok++;
                }
            }
            if (!any) SwitchToThread();
        }
        // Every byte of every queue's buffer must still hold what that queue sent.
        int qbadQueues = 0;
        for (int qi = 0; qi < ioUp; qi++) {
            uint8_t* b = dev.region(kXferOff + (size_t)qi * qbytes);
            for (uint32_t k = 0; k < qbytes; k++) {
                if (b[k] != (uint8_t)(k * 13u + qi * 71u + 5u)) { qbadQueues++; break; }
            }
        }
        char dq[176];
        sprintf_s(dq, "%d of %d queues answered with success, %d queues with wrong data",
                  qok, ioUp, qbadQueues);
        Report("every I/O queue runs its own command at the same time",
               qanswered == ioUp && qok == ioUp && qbadQueues == 0, dq);

        // And read them all back the same way, which is the direction where the
        // TARGET writes into each queue's buffer: a target that kept one shared data
        // buffer would answer both commands and deliver the wrong bytes to one.
        for (int qi = 0; qi < ioUp; qi++) {
            uint8_t* b = dev.region(kXferOff + (size_t)qi * qbytes);
            memset(b, 0, qbytes);
            io[qi].postReceive(dev.region(kRespInOff + (size_t)qi * 16), 16,
                               (void*)(CTX_HOST_RESP_BASE + (uintptr_t)qi));
            uint8_t* cc = dev.region(kAdminCapOff + (size_t)qi * 64);
            fabricHeader(cc, NVMEOF_OPC_READ, ++cid, 0);
            nvmeof_wr32(cc + 4, nsid);
            nvmeof_wr64(cc + 40, (uint64_t)qi * qblocks);
            nvmeof_wr32(cc + 48, qblocks - 1);
            nvmeof_sgl_set_keyed((nvmeof_sgl*)(cc + 24), (uint64_t)(uintptr_t)b, qbytes, dev.rkey);
            io[qi].send(cc, 64, CTX_CAP);
        }
        int qranswered = 0;
        tq = GetTickCount64();
        while (qranswered < ioUp && GetTickCount64() - tq < kWaitMs) {
            bool any = false;
            for (int qi = 0; qi < ioUp; qi++) {
                ND2_RESULT r = {};
                if (!io[qi].poll(&r)) continue;
                any = true;
                if ((uintptr_t)r.RequestContext >= CTX_HOST_RESP_BASE &&
                    (uintptr_t)r.RequestContext < CTX_HOST_RESP_BASE + (uintptr_t)ioUp) qranswered++;
            }
            if (!any) SwitchToThread();
        }
        int rbadQueues = 0;
        for (int qi = 0; qi < ioUp; qi++) {
            uint8_t* b = dev.region(kXferOff + (size_t)qi * qbytes);
            for (uint32_t k = 0; k < qbytes; k++) {
                if (b[k] != (uint8_t)(k * 13u + qi * 71u + 5u)) { rbadQueues++; break; }
            }
        }
        sprintf_s(dq, "%d of %d queues answered, %d queues with wrong data",
                  qranswered, ioUp, rbadQueues);
        Report("a parallel read across every queue returns each queue its own data",
               qranswered == ioUp && rbadQueues == 0, dq);
    }

    // ---- 9. leaving: disable the controller, then tear the queue pairs down ----
    //
    // There is NO fabrics Disconnect command (DESIGN 8.44).  A host leaves the way
    // the Linux host leaves: a Property Set of CC with EN = 0, and then the RDMA
    // connections go away.  This suite used to send fctype 0x08 and assert the
    // target accepted it - a command the reference header does not define.
    {
        // The fabricated command must be REFUSED, so it can never come back as a
        // thing both of our ends happen to agree on.  This has to be asked BEFORE
        // the controller is disabled below: a target that has just been told
        // CC.EN=0 is entitled to stop, and this probe would then have no target to
        // refuse it - which is exactly how this assertion failed the first time.
        fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, 0x08);
        Completion fake = {};
        bool sent2 = submitCommand(admin, dev, cap, &fake, kWaitMs);
        char d2[128];
        sprintf_s(d2, "sct=%u sc=0x%02X", nvmeof_status_sct(fake.status),
                  nvmeof_status_sc(fake.status));
        Report("an fctype that does not exist (0x08) is refused, not answered",
               sent2 && !statusOk(fake.status) &&
                   nvmeof_status_sc(fake.status) == NVMEOF_SC_INVALID_OPCODE, d2);

        fabricHeader(cap, NVMEOF_OPC_FABRICS, ++cid, NVMEOF_FCTYPE_PROPERTY_SET);
        nvmeof_wr32(cap + 44, NVMEOF_PROP_CC);      // attrib 0: CC is 32 bits
        nvmeof_wr64(cap + 48, 0);                   // EN = 0
        Completion dc = {};
        bool sent = submitCommand(admin, dev, cap, &dc, kWaitMs);
        char d[96];
        sprintf_s(d, "status=0x%04X", dc.status);
        Report("Property Set CC.EN=0 disables the controller (how a host really leaves)",
               sent && statusOk(dc.status), d);
    }

    admin.destroy();
    for (uint16_t q = 0; q < kMaxIoQueues; q++) io[q].destroy();
    dev.close(nullptr, nullptr, 0);
    printf("\ninitiator failures: %d\n", g_failures);
    return g_failures;
}

// ===========================================================================
//  TARGET - for a Linux host (nvme-cli) to drive
// ===========================================================================
struct F5State {
    bool     enabled = false;
    uint16_t cntlid = 1;
    int      ioQueuesGranted = 0;
    int      katoMs = 0;
    int      keepAlives = 0;
    int      ioCommands = 0;
    int      flushes = 0;
    uint64_t bytesWritten = 0;
    uint64_t bytesRead = 0;
    // Bytes cleared by Write Zeroes (0x08) or Dataset Management (0x09).  Kept apart
    // from bytesWritten on purpose: neither command carries a data phase, so counting
    // them as "written" would make the byte count say something the wire did not.
    uint64_t bytesZeroed = 0;
    int      zeroesCommands = 0;
    int      dsmCommands = 0;
    int      invalidates = 0;
    int      connects = 0;
    int      unknownAdmin = 0;
    int      unknownIo = 0;
    bool     disabledByHost = false;   // CC.EN 1 -> 0: the controller is disabled
    bool     disableNoted = false;     // ...and that has been reported once
    // CSTS bits this target maintains besides RDY.  See NVMEOF_CSTS_* in the wire
    // header: SHST is what a host waits for after a shutdown request, and CFS is how
    // a controller says it is fatally broken.
    uint32_t ccShnPrev = 0;            // last CC.SHN field seen
    bool     cstsShst = false;         // shutdown complete
    bool     cstsCfs  = false;         // fatal error (a failed DH-HMAC-CHAP sets it)
    int      pendingAers = 0;     // Async Event commands held, not completed
    int      deferredAers = 0;    // ... total accepted
    // Async Event Configuration (feature 0x0b), as the host last set it.  nvmet keeps
    // exactly this one word and hands it back on a Get; a target that answers
    // Invalid Field to the Get makes `nvme get-feature -f 0x0b` fail for no reason.
    uint32_t aenMask = 0;

    // One entry per (I/O queue, receive slot): a command whose data transfer is in
    // flight.  A slot is consumed by a capsule, held until its transfer completes,
    // and only then re-armed - so the ring depth is the number of commands that can
    // be in flight, and no two commands ever share an entry.  The table is indexed
    // by queue as well as slot, because each of the eight queue pairs owns its own
    // ring and its own in-flight commands.
    struct IoSlot { bool used = false; uint16_t cid = 0; bool isWrite = false;
                    uint32_t bytes = 0; uint64_t slba = 0;
                    // -nvmetiming: when this command's capsule was decoded.  t0 for the
                    // target's own duration, tQpc for cross-process correlation (a
                    // machine-wide clock; steady_clock's epoch is per process).
                    std::chrono::steady_clock::time_point t0; long long tQpc = 0; };
    IoSlot io[kMaxIoQueues][kCapSlots];

    // Commands served per I/O queue.  This is the evidence that a host really used
    // more than one queue: "8 queues connected" would also be true of a host that
    // connected eight and sent everything down the first.
    int      ioPerQueue[kMaxIoQueues] = {};
    int      ioQueuesConnected = 0;

    // Capsules whose completion was seen while this target was waiting for a data
    // transfer.  They are queued, not dropped: nvmeof::Queue::reap() DISCARDS a
    // completion it is not waiting for, and that is what made a pipelined burst
    // answer 1 of 4 commands (DESIGN 8.40 note; the same trap as F4's bug 2).
    int      pending[kCapSlots] = {};
    int      pendingCount = 0;
    const char* subnqn = "nqn.2024-01.local.rdma:windows-nd";

    // ---- discovery ----
    // A host connects to the well-known discovery subsystem to ask "what is here?".
    // It is the same admin handshake with a different NQN, and it must be answered
    // by a *discovery controller*: Identify reports ctrltype 2, the discovery NQN,
    // no namespaces and no I/O capsule sizes (there are no I/O queues), and
    // Get Log Page LID 0x70 returns one entry per subsystem/port.
    bool     discovery = false;
    char     connSubnqn[256] = {};   // what the host actually connected to
    uint16_t port = 4420;            // goes into the entry's trsvcid, as text
    const char* ip = "0.0.0.0";      // ...and into its traddr
    int      discLogReads = 0;       // Get Log Page LID 0x70 answered
    int      ctrlIndex = 0;          // which controller this is (0-based)

    // ---- DH-HMAC-CHAP ----
    // One state machine per controller, and it only ever runs on the admin queue:
    // nvmet clears the ATR bit for qid != 0, so "authenticate each queue" would be
    // work no host asks for.  `auth.enabled` means a host key was configured; a
    // controller with no key must NOT advertise ATR, because a host that then has
    // no key of its own fails the connect with -ENOKEY and gives up
    // (host/fabrics.c:590 - -ENOKEY is not retried).
    nvmeof_dhchap::TargetAuth auth;
    bool     authAtConnect = false;  // ATR was advertised on this controller's Connect
    int      authSends = 0;          // Auth Send commands answered
    int      authReceives = 0;       // Auth Receive commands answered
    int      authRefused = 0;        // non-fabrics commands refused with 0x4191
    int      authAborted = 0;        // Auth Send aborted before its payload was read
    int      authFatal = 0;          // controllers that hit nvmet_ctrl_fatal_error (CSTS.CFS)
    // One Auth Send in flight at a time, and the SGL it came from: the payload is
    // read into kAuthOff and the SAME slot must be re-armed afterwards, exactly as
    // the Connect data path does.
    int      authSlot = -1;
};

// Running totals across every controller a target run served.  A run can be a
// discovery controller followed by an I/O controller, and the summary line has to
// describe the whole run rather than whichever controller happened to be last.
struct TargetTotals {
    int connects = 0, ioQueuesGranted = 0, ioQueuesConnected = 0, ioCommands = 0;
    int flushes = 0, invalidates = 0, keepAlives = 0, deferredAers = 0;
    int discLogReads = 0, unknownAdmin = 0, unknownIo = 0;
    int katoMs = 0;
    int authSends = 0, authReceives = 0, authRefused = 0, authAborted = 0;
    int  authFatal = 0;
    uint64_t bytesWritten = 0, bytesRead = 0, bytesZeroed = 0;
    int zeroesCommands = 0, dsmCommands = 0;

    void add(const F5State& s) {
        connects += s.connects;
        ioQueuesGranted = (s.ioQueuesGranted > ioQueuesGranted) ? s.ioQueuesGranted : ioQueuesGranted;
        ioQueuesConnected += s.ioQueuesConnected;
        ioCommands += s.ioCommands;
        flushes += s.flushes;
        invalidates += s.invalidates;
        keepAlives += s.keepAlives;
        deferredAers += s.deferredAers;
        discLogReads += s.discLogReads;
        unknownAdmin += s.unknownAdmin;
        unknownIo += s.unknownIo;
        authSends += s.authSends;
        authReceives += s.authReceives;
        authRefused += s.authRefused;
        authAborted += s.authAborted;
        authFatal += s.authFatal;
        if (s.katoMs > katoMs) katoMs = s.katoMs;
        bytesWritten += s.bytesWritten;
        bytesRead += s.bytesRead;
        bytesZeroed += s.bytesZeroed;
        zeroesCommands += s.zeroesCommands;
        dsmCommands += s.dsmCommands;
    }
};

static void fillIdentifyCtrl(uint8_t* id, const F5State& st) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_VID, 0x15B3);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_SSVID, 0x103C);
    memcpy(id + NVMEOF_ID_CTRL_OFF_SN, "NDVMEOF0000000000001", 20);
    memcpy(id + NVMEOF_ID_CTRL_OFF_MN, "NetworkDirect NVMe-oF prototype          ", 40);
    memcpy(id + NVMEOF_ID_CTRL_OFF_FR, "0.5.0   ", 8);
    // The controller id in Identify MUST equal the one the admin Connect
    // assigned: nvme_check_ctrl_fabric_info() compares them and rejects the
    // controller outright ("Mismatching cntlid: Connect %u vs Identify %u").
    // This field was missing here until the new assertion above caught it.
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_CNTLID, st.cntlid);
    id[NVMEOF_ID_CTRL_OFF_MDTS] = 0;
    id[NVMEOF_ID_CTRL_OFF_CNTRLTYPE] = NVMEOF_CTRLTYPE_IO;
    id[NVMEOF_ID_CTRL_OFF_SQES] = (uint8_t)((0x6 << 4) | 0x6);
    id[NVMEOF_ID_CTRL_OFF_CQES] = (uint8_t)((0x4 << 4) | 0x4);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_MAXCMD, kSqDepth);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_NN, 1);
    // No volatile write cache: the namespace is this process's memory.  A host
    // that sees VWC=0 does not need to flush before an unmount, but Flush is
    // answered anyway.
    // VOLATILE WRITE CACHE: CONDITIONED ON THE BACKING, BECAUSE THE TWO BACKINGS
    // MAKE OPPOSITE PROMISES.
    //
    // VWC = 0 tells a host "a completed write is already durable, you need not
    // flush".  For a file-backed namespace that is FALSE here: writes land in this
    // process's RAM cache and only FLUSH writes them back to the file (that is the
    // whole point of -nsfile), so a host that trusted VWC = 0 could unmount without
    // a flush and lose bytes it was told were safe.  nvmet reports 1 for the same
    // reason - it sits on a real block device (f3_io.cpp has recorded that since the
    // first target).
    //
    // A memory-only namespace is the case the older comment was written for: Flush
    // has nothing to write anywhere, so the honest value there is 0 and the host's
    // flush path stays off the critical path.  The four F-suites with in-memory
    // namespaces (f3/f6/f7 and f1/f4, which chose 1) are unchanged.
    id[NVMEOF_ID_CTRL_OFF_VWC] = g_nsFile ? 1 : 0;
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_KAS, 1);
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_SGLS, NVMEOF_CTRL_SGLS_ADVERTISED);
    // ONCS: WHAT WE ANSWER, NOT WHAT WOULD LOOK GOOD.
    //
    // This field sat at 0 while Write Zeroes and DSM were implemented, tested and
    // byte-verified - the Linux side drove both explicitly (`nvme write-zeroes`,
    // `nvme dsm`) and they worked.  The cost of the omission is concrete: a host's
    // block layer does not offer discard or write-zeroes on a namespace whose
    // controller answers ONCS = 0, so `blkdiscard`, `fstrim` and every filesystem
    // discard path were dead on a namespace that could have served them.
    //
    // Computed from kNvmeOfIoCaps (nvmeof_wire.h), so the field cannot promise a
    // command the I/O dispatch has no case for - and the table is unit-tested.
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_ONCS, nvmeofOncsFromCaps());
    memcpy(id + NVMEOF_ID_CTRL_OFF_SUBNQN, st.subnqn, strlen(st.subnqn));
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ, 4);   // one 64 B SQE, no in-capsule data
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IORCSZ, 1);
    nvmeof_wr16(id + NVMEOF_ID_CTRL_OFF_ICDOFF, 0);
    id[NVMEOF_ID_CTRL_OFF_MSDBD] = 1;
    nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_VER, 0x00010400);

    // A CHECK, NOT A COMMENT, for the volatile-write-cache bit: it must agree with
    // the backing this process was actually given.  VWC = 0 on a file-backed
    // namespace tells a host its writes are durable without a Flush, which is false
    // here and would lose bytes the host was told were safe; VWC = 1 on a memory-only
    // namespace makes Flush meaningless.  Printed with the [FAIL] marker that
    // run_all.ps1 scans for, so the rule is gated rather than remembered.  Skipped on
    // a discovery controller, which has no namespace at all.
    if (!st.discovery) {
        int vwc = id[NVMEOF_ID_CTRL_OFF_VWC] ? 1 : 0;
        int want = g_nsFile ? 1 : 0;
        if (vwc != want) {
            printf("  [FAIL] Identify says VWC=%d but the namespace backing is %s - "
                   "a host would %s\n", vwc, g_nsFile ? "a file (needs Flush)" : "memory (nothing to flush)",
                   vwc ? "flush needlessly" : "skip the flush that makes its data durable");
        } else {
            printf("  [ok]   Identify VWC=%d, which matches the %s namespace\n", vwc,
                   g_nsFile ? "file-backed" : "memory-only");
        }
    }

    // A discovery controller is a different kind of controller, not a second I/O
    // controller: the host checks cntrltype, the subnqn it is connected to, and
    // expects no namespaces and no I/O queues.  Reporting the I/O values here makes
    // `nvme discover` fail with "Mismatching subnqn" (the host compares the Identify
    // subnqn with the NQN it used to connect).
    if (st.discovery) {
        id[NVMEOF_ID_CTRL_OFF_CNTRLTYPE] = NVMEOF_CTRLTYPE_DISC;
        memset(id + NVMEOF_ID_CTRL_OFF_SUBNQN, 0, 256);
        memcpy(id + NVMEOF_ID_CTRL_OFF_SUBNQN, NVMEOF_DISC_SUBSYS_NAME,
               strlen(NVMEOF_DISC_SUBSYS_NAME));
        nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_NN, 0);
        nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IOCCSZ, 0);
        nvmeof_wr32(id + NVMEOF_ID_CTRL_OFF_IORCSZ, 0);
    }
}

static void fillIdentifyNs(uint8_t* id) {
    memset(id, 0, NVMEOF_IDENTIFY_SIZE);
    // The geometry the BACKEND really has.  With -nsfile that is the file's size, and a
    // host told a larger number writes past the end of the medium - which the host
    // cannot detect, because the write "succeeds" into our memory cache and only the
    // flush reveals that there was nowhere to put it.
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NSZE, g_nsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NCAP, g_nsBlocks);
    nvmeof_wr64(id + NVMEOF_ID_NS_OFF_NUSE, g_nsBlocks);
    id[NVMEOF_ID_NS_OFF_NLBAF] = 0;
    id[NVMEOF_ID_NS_OFF_FLBAS] = 0;
    id[NVMEOF_ID_NS_OFF_LBAF + 2] = 9;          // lbaf[0].ds = 9 -> 512 B
    // THIN PROVISIONING, and the granularity hints that go with it.
    //
    // NSFEAT bit 0 says "this namespace can be deallocated and a deallocated block
    // reads as something defined" - it is the namespace-side half of the promise
    // that ONCS.DSM makes on the controller side, and without it a host has no
    // reason to believe a deallocate did anything.  Our deallocate zeroes the
    // range, which is what a deallocated block has to read as (DESIGN 8.54).
    id[NVMEOF_ID_NS_OFF_NSFEAT] = NVMEOF_NS_FEAT_THIN;
    // DLFEAT stays 0 = "not reported", and that is a deliberate stop rather than an
    // oversight: the field encodes WHAT a deallocated block reads as (all-zero vs
    // all-ones vs undefined), and that encoding is in the NVMe base spec, which this
    // repository cannot cross-check - ref/linux_nvme.h names the field and not its
    // bits, and this project's rule is that a wire value is read from a header, never
    // remembered.  It is recorded in ROADMAP A2: if the Linux side reports no discard
    // after ONCS and NSFEAT are set, nailing DLFEAT's encoding from the spec is the
    // next step (0 is "unreported", not "no support").
    //
    // The granularity hints are 0-based in LOGICAL BLOCKS: 7 means 8 blocks = 4 KiB,
    // which is the page the file backend actually writes in.  0 would mean "one
    // block", i.e. 512 bytes, and would have every host issuing 512-byte deallocates.
    nvmeof_wr16(id + NVMEOF_ID_NS_OFF_NPWG, 7);
    nvmeof_wr16(id + NVMEOF_ID_NS_OFF_NPWA, 7);
    nvmeof_wr16(id + NVMEOF_ID_NS_OFF_NPDG, 7);
    nvmeof_wr16(id + NVMEOF_ID_NS_OFF_NPDA, 7);
    nvmeof_wr16(id + NVMEOF_ID_NS_OFF_NOWS, 7);
}

// The discovery log page: a 1024-byte header (generation counter, record count,
// format) followed by one 1024-byte entry per thing a host can connect to.
//
// Two entries, and the second one is copied from what **nvmet actually answers**
// (measured with `-discover` against the Linux peer, DESIGN 8.50):
//
//   #0  subtype 3 (NVME_NQN_CURR: "this is the current discovery subsystem"),
//       cntlid 0xffff, subnqn = the discovery NQN
//   #1  subtype 2 (NVME_NQN_NVME), cntlid 0xffff (dynamic - there is no controller
//       id until a host connects), subnqn = the I/O subsystem
//
// Both carry the port number as TEXT in trsvcid and the address in traddr, which is
// what `nvme discover` prints.  The first version of this used subtype 1 for its own
// entry; 1 means "referral to ANOTHER discovery controller", which is a different
// claim, and the reference uses 3.
static void buildDiscoveryLog(uint8_t* buf, Device& d, const F5State& st) {
    (void)d;
    if (!buf) return;
    char portText[16];
    sprintf_s(portText, "%u", (unsigned)st.port);

    nvmeof_disc_hdr_set(buf, 1, 2);      // genctr 1, two records
    nvmeof_disc_entry_set(buf + NVMEOF_DISC_HDR_SIZE, NVMEOF_NQN_CURR, 1,
                          NVMEOF_CNTLID_DYNAMIC, NVMEOF_DISC_SUBSYS_NAME, st.ip, portText);
    nvmeof_disc_entry_set(buf + NVMEOF_DISC_HDR_SIZE + NVMEOF_DISC_ENTRY_SIZE,
                          NVMEOF_NQN_NVME, 1, NVMEOF_CNTLID_DYNAMIC, st.subnqn,
                          st.ip, portText);
}


// Wait for one specific data-transfer completion WITHOUT discarding anything else.
//
// Queue::reap(expect, ...) throws away every completion whose context is not the
// one being waited for, which is exactly wrong here: while this target is reading
// a Connect's 1024 bytes, the next admin capsule can complete, and while it is
// moving a Read's data, the next I/O capsule can complete.  Those completions are
// queued into st.pending and processed by the main loop afterwards.
static bool waitForData(Queue& q, Device& d, F5State& st, void* dataCtx) {
    (void)d;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        ND2_RESULT r = {};
        if (q.poll(&r)) {
            if (r.RequestContext == dataCtx) {
                char b[64];
                printf("  [data]  completion for the transfer: status=%s bytes=%u\n",
                       ndStr(r.Status, b, sizeof(b)), (unsigned)r.BytesTransferred);
                return ndOk(r.Status) != 0;
            }
            uintptr_t x = (uintptr_t)r.RequestContext;
            if (x >= CTX_ADMIN_CAP_BASE && x < CTX_ADMIN_CAP_BASE + kCapSlots &&
                ndOk(r.Status) && r.BytesTransferred == 64) {
                int slot = (int)(x - CTX_ADMIN_CAP_BASE);
                if (st.pendingCount < (int)kCapSlots) st.pending[st.pendingCount++] = slot;
                printf("  [admin] slot %d arrived during a data transfer; queued\n", slot);
            }
            continue;
        }
        if (GetTickCount64() - t0 >= kWaitMs) return false;
        SwitchToThread();
    }
}

// Post one one-sided transfer and wait for it - or, when there is nothing to move,
// post NOTHING and report success.
//
// A zero-length RDMA Write/Read is not a no-op an HCA completes: the request is
// posted and no completion ever comes back, so a target that waits for one waits for
// ever, and because this target answers one capsule at a time on each queue, that
// queue stops there for good.  This is measured, not theoretical.  nvme-cli's
// `persistent-event-log` sent Get Log Page LID 0x0d with numd=0xFFFFFFFF and a
// ZERO-BYTE SGL; `(numd + 1) * 4` computed in 32 bits wrapped to 0, the length check
// then agreed with the host's 0-byte SGL, the target posted a 0-byte write, the admin
// queue never advanced again, and 7.6 s after the connect the host's Keep Alive timer
// expired and it tore the controller down.  The symptom on the host side was
// `I/O tag 1 (3001) opcode 0x18 (Keep Alive) QID 0 timeout`, which points at keep-alive
// handling and has nothing to do with it (DESIGN 9.1).
//
// Note the completion's BytesTransferred is NOT printed as the transfer length: for an
// RDMA Write this provider reports a value unrelated to the request (a 512-byte write
// completed as "bytes=643", twice), so printing it as if it were the length invents a
// number.  The length that was asked for is what gets printed.
static bool carryData(Queue& q, Device& d, F5State& st, const char* what, bool isWrite,
                      void* buf, uint32_t len, uint64_t remoteAddr, uint32_t remoteKey,
                      void* ctx) {
    if (len == 0) {
        printf("  [data]  %s: 0 bytes to move - nothing is posted, nothing to wait for\n",
               what);
        return true;
    }
    if (isWrite) {
        if (!q.write(buf, len, remoteAddr, remoteKey, ctx)) return false;
    } else {
        if (!q.read(buf, len, remoteAddr, remoteKey, ctx)) return false;
    }
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        ND2_RESULT r = {};
        if (q.poll(&r)) {
            if (r.RequestContext == ctx) {
                char b[64];
                printf("  [data]  %s: %s, %u bytes asked for\n", what,
                       ndStr(r.Status, b, sizeof(b)), len);
                return ndOk(r.Status) != 0;
            }
            uintptr_t x = (uintptr_t)r.RequestContext;
            if (x >= CTX_ADMIN_CAP_BASE && x < CTX_ADMIN_CAP_BASE + kCapSlots &&
                ndOk(r.Status) && r.BytesTransferred == 64) {
                int slot = (int)(x - CTX_ADMIN_CAP_BASE);
                if (st.pendingCount < (int)kCapSlots) st.pending[st.pendingCount++] = slot;
                printf("  [admin] slot %d arrived during a data transfer; queued\n", slot);
            }
            continue;
        }
        if (GetTickCount64() - t0 >= kWaitMs) return false;
        SwitchToThread();
    }
}

// Answer one Auth Send (fctype 0x05) or Auth Receive (fctype 0x06) capsule.
//
// The two directions are not symmetric and the difference is the one thing to get
// right: an Auth Send carries the host's payload in its SGL and the target READS
// it; an Auth Receive carries an allocation length and the target WRITES the
// answer back.  `nvme_is_write()` on a fabrics command is literally `fctype & 1`,
// which is why Auth Send (0x05) is a "write" and Auth Receive (0x06) is not.
//
// Every check below is one nvmet performs before it looks at the payload, and each
// is reported with the byte that failed: a target that skips them answers a
// malformed auth command with "wrong HMAC", a true statement about a run that
// never had a chance.
static void targetAuth(Queue& q, Device& d, F5State& st, const uint8_t* cap,
                       const Sgl& sgl, bool isSend,
                       uint8_t* psct, uint8_t* psc, bool* pDnr) {
    const char* which = isSend ? "Auth Send" : "Auth Receive";
    const char* lenField = isSend ? "tl" : "al";
    uint32_t len = nvmeof_rd32(cap + NVMEOF_AUTH_OFF_AL_TL);

    auto refuse = [&](const char* why) {
        *psct = NVMEOF_SCT_GENERIC;
        *psc  = NVMEOF_SC_INVALID_FIELD;
        *pDnr = true;                      // nvmet: NVME_SC_INVALID_FIELD | DNR
        printf("  [auth] %s refused: %s\n", which, why);
    };

    // 1. secp / spsp0 / spsp1 / non-zero length.  nvmet answers Invalid Field | DNR
    //    to each of these with error_loc on the offending byte.
    char why[128];
    if (cap[NVMEOF_AUTH_OFF_SECP] != NVMEOF_AUTH_SECP_DHCHAP) {
        sprintf_s(why, "secp=0x%02X, not DH-HMAC-CHAP (0x%02X)",
                  cap[NVMEOF_AUTH_OFF_SECP], (unsigned)NVMEOF_AUTH_SECP_DHCHAP);
        refuse(why);
        return;
    }
    if (cap[NVMEOF_AUTH_OFF_SPSP0] != NVMEOF_AUTH_SPSP0 ||
        cap[NVMEOF_AUTH_OFF_SPSP1] != NVMEOF_AUTH_SPSP1) {
        sprintf_s(why, "spsp0/spsp1 = 0x%02X/0x%02X, must both be 0x01",
                  cap[NVMEOF_AUTH_OFF_SPSP0], cap[NVMEOF_AUTH_OFF_SPSP1]);
        refuse(why);
        return;
    }
    if (len == 0) {
        sprintf_s(why, "%s = 0", lenField);
        refuse(why);
        return;
    }
    // 2. The command's length and the SGL's length must agree (nvmet's
    //    nvmet_check_transfer_len), and the mismatch code is SGL_INVALID_DATA|DNR -
    //    not Invalid Field, because the SGL flag bit is set on every RDMA command.
    if (len != sgl.len) {
        printf("  [auth] %s: %s=%u but the SGL says %u -> SGL_INVALID_DATA | DNR\n",
               which, lenField, len, sgl.len);
        *psct = NVMEOF_SCT_GENERIC;
        *psc  = NVMEOF_SC_SGL_INVALID_DATA;
        *pDnr = true;
        return;
    }
    // 3. The payload has to fit the buffer this target registered for it.
    if (len > kAuthBytes) {
        printf("  [auth] %s: %u bytes does not fit the %u-byte auth buffer -> "
               "SGL_INVALID_DATA | DNR\n", which, len, (unsigned)kAuthBytes);
        *psct = NVMEOF_SCT_GENERIC;
        *psc  = NVMEOF_SC_SGL_INVALID_DATA;
        *pDnr = true;
        return;
    }

    if (isSend) {
        uint8_t* buf = d.region(kAuthOff);
        memset(buf, 0, len);
        if (!q.read(buf, len, sgl.addr, sgl.key, CTX_DATA)) {
            printf("  [auth] Auth Send: the RDMA Read of %u bytes was NOT posted\n", len);
            *psc = NVMEOF_SC_DATA_XFER_ERROR;
            return;
        }
        if (!waitForData(q, d, st, CTX_DATA)) {
            printf("  [auth] Auth Send: the Read did not complete; the command is "
                   "abandoned without an answer\n");
            st.authAborted++;
            *psc = NVMEOF_SC_DATA_XFER_ERROR;
            return;
        }
        st.authSends++;
        printf("  [auth] Auth Send: %u bytes in, auth_type=0x%02X auth_id=0x%02X "
               "(step=%d)\n", len, buf[0], buf[1], st.auth.step);
        if (st.auth.enabled) {
            st.auth.onAuthSend(buf, len);
        } else {
            // No host key configured.  nvmet reads a Negotiate even then and only
            // fails when the host's Reply arrives ("no host key"); this target says
            // so one message earlier with the same Failure1 the host would have got.
            printf("        no host key configured -> Failure1 (rescode_exp=0x%02X)\n",
                   (unsigned)nvmeof_dhchap::kFailureFailed);
            st.auth.failed = true;
            snprintf(st.auth.failWhy, sizeof(st.auth.failWhy),
                     "no host key configured on this target");
            nvmeof_dhchap::buildFailure1Msg(st.auth.pendingMsg, st.auth.tId,
                                            nvmeof_dhchap::kFailureFailed);
            st.auth.pendingLen = 8;
        }
        if (st.auth.failed) {
            // Any failure is terminal in the reference: the next Auth Receive gets
            // Failure1 and the controller is then fatally errored (CSTS.CFS).
            printf("        auth failed: %s\n", st.auth.failWhy);
        }
    } else {
        uint8_t* buf = d.region(kAuthOff);
        st.authReceives++;
        // onAuthReceive answers with whatever the last Auth Send queued - including
        // a Failure1 built by the no-key path above - and always returns exactly
        // `al` bytes, zero padded, because that is what nvmet copies to the SGL and
        // what a host that posted al = 4096 expects to receive.
        size_t got = st.auth.onAuthReceive(len, buf);
        printf("  [auth] Auth Receive: al=%u, answering %u bytes (auth_type=0x%02X "
               "auth_id=0x%02X)\n", len, (unsigned)got, buf[0], buf[1]);
        // `len` is the host's allocation length, so it is host-controlled and may be 0;
        // carryData() refuses to post a 0-byte transfer (see its comment).
        if (!carryData(q, d, st, "Auth Receive", true, buf, len, sgl.addr, sgl.key, CTX_DATA)) {
            printf("  [auth] Auth Receive: the RDMA Write of %u bytes was NOT posted\n", len);
            *psc = NVMEOF_SC_DATA_XFER_ERROR;
            return;
        }
        // The success/failure payload has been delivered.  A FAILURE1 step ends the
        // controller in the reference (nvmet_ctrl_fatal_error): CSTS.CFS, then the
        // connection goes away.
        if (st.auth.failed) {
            st.authFatal++;
            printf("        controller is now in a fatal error state (CSTS.CFS), as nvmet\n");
        }
    }
}

// Answer one admin capsule.  Deliberately tolerant: a foreign host is entitled
// to send things this target does not implement, and what it sends is the point
// of the exercise - so every command is logged and unknown ones are answered
// with a status rather than ignored.
static bool targetAdmin(Queue& q, Device& d, F5State& st, int slot) {
    const size_t capOff  = kAdminCapOff  + (size_t)slot * 64;
    const size_t respOff = kAdminRespOff + (size_t)slot * 16;
    const uint8_t* cap = d.region(capOff);
    uint8_t  opcode = cap[0];
    uint16_t cid    = nvmeof_rd16(cap + 2);
    uint8_t  fctype = cap[4];
    uint8_t  sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
    uint64_t result = 0;
    Sgl sgl = parseSgl(cap);

    // Wire-level evidence.  A foreign host is the only thing that can tell us what
    // the capsule REALLY looks like, and when it disagrees with what we send, the
    // bytes are the only argument that settles it (DESIGN 8.29/8.40/8.44: five
    // constants so far were fabricated and survived because both of our own
    // endpoints agreed with each other).  Printed for every admin capsule.
    {
        printf("  [wire]  op=0x%02X cid=%u fctype=0x%02X sgl(byte39)=0x%02X type=0x%02X "
               "keyed=%d addr=0x%llX len=%u key=0x%08X\n",
               opcode, cid, fctype, cap[39], sgl.type, (int)sgl.present,
               (unsigned long long)sgl.addr, sgl.len, sgl.key);
        for (int i = 0; i < 64; i += 16) {
            printf("  [wire]   %02d: %02X %02X %02X %02X %02X %02X %02X %02X  "
                   "%02X %02X %02X %02X %02X %02X %02X %02X\n", i,
                   cap[i+0], cap[i+1], cap[i+2], cap[i+3], cap[i+4], cap[i+5], cap[i+6], cap[i+7],
                   cap[i+8], cap[i+9], cap[i+10], cap[i+11], cap[i+12], cap[i+13], cap[i+14], cap[i+15]);
        }
    }

    // nvmet checks SQE byte 1 BEFORE it parses anything, and so must this target:
    // a target that tolerates a bad flags byte cannot catch the bug it causes, and
    // that is exactly how our host shipped for months with 0x00 there while every
    // real target answered Invalid Field to the whole init sequence (DESIGN 8.46).
    if (!nvmeof_sqe_flags_ok(cap[1])) {
        printf("  [admin] SQE flags 0x%02X refused: nvmet wants PSDT = 0b01 (0x40) and no fuse bits\n",
               cap[1]);
        buildCompletionDnr(d.region(respOff), cid, 0, 0,
                           NVMEOF_SCT_GENERIC, NVMEOF_SC_INVALID_FIELD);
        q.postReceive(d.region(capOff), 64, (void*)(CTX_ADMIN_CAP_BASE + (uintptr_t)slot));
        return q.send(d.region(respOff), 16, CTX_RESP);
    }
    // ...and the descriptor shape, which nvmet checks for every command too.
    {
        uint8_t dsct = NVMEOF_SCT_GENERIC, dsc = NVMEOF_SC_SUCCESS;
        bool isWrite = (opcode == NVMEOF_OPC_WRITE) ||
                       (opcode == NVMEOF_OPC_FABRICS && (fctype & 1));
        if (nvmeof_sgl_check_rdma(cap + 24, isWrite ? 1 : 0, &dsct, &dsc)) {
            printf("  [admin] dptr 0x%02X refused: nvmet's RDMA transport accepts only a keyed "
                   "descriptor (0x40/0x4f) here\n", cap[39]);
            buildCompletionDnr(d.region(respOff), cid, 0, 0, dsct, dsc);
            q.postReceive(d.region(capOff), 64, (void*)(CTX_ADMIN_CAP_BASE + (uintptr_t)slot));
            return q.send(d.region(respOff), 16, CTX_RESP);
        }
    }

    // A controller with a host key refuses every command that is not a fabrics one
    // until the host has authenticated - that is nvmet's nvmet_check_auth_status(),
    // reached through nvmet_check_ctrl_status() for admin commands
    // (target/admin-cmd.c:1642) and nvmet_parse_io_cmd() for I/O.  The exemption for
    // fabrics commands is not an optimisation: Connect, Property Get/Set and the auth
    // commands themselves MUST work before authentication, or the host cannot even
    // find out that it has to authenticate.
    //
    // It applies to the admin queue only, and that is the reference's rule too:
    // nvmet_check_auth_status() returns true for qid > 0.
    bool dnr = false;
    if (opcode != NVMEOF_OPC_FABRICS && st.auth.enabled && !st.auth.authenticated) {
        st.authRefused++;
        printf("  [admin] opcode 0x%02X refused before authentication -> 0x%X | DNR\n",
               opcode, (unsigned)NVMEOF_SC_AUTH_REQUIRED);
        buildCompletionDnr(d.region(respOff), cid, 0, 0,
                           NVMEOF_SC_AUTH_REQUIRED_SCT, NVMEOF_SC_AUTH_REQUIRED_SC);
        q.postReceive(d.region(capOff), 64, (void*)(CTX_ADMIN_CAP_BASE + (uintptr_t)slot));
        return q.send(d.region(respOff), 16, CTX_RESP);
    }

    if (opcode == NVMEOF_OPC_FABRICS) {
        if (fctype == NVMEOF_FCTYPE_PROPERTY_SET) {
            uint32_t off = nvmeof_rd32(cap + 44);
            uint64_t val = nvmeof_rd64(cap + 48);
            printf("  [admin] Property Set off=0x%X val=0x%llX\n", off,
                   (unsigned long long)val);
            if (off == NVMEOF_PROP_CC) {
                bool wasEnabled = st.enabled;
                st.enabled = (val & NVMEOF_CC_EN) != 0;
                // CC.SHN (bits 15:14): the host asking for a shutdown notification.
                // nvmet sets CSTS.SHST_CMPLT on the 0 -> nonzero transition and clears
                // it when the field goes back to 0, and the host's disconnect path
                // POLLS for that bit - a target that never sets it makes every
                // disconnect end with "Device not ready; aborting shutdown, CSTS=0x1".
                uint32_t shn = ((uint32_t)val & NVMEOF_CC_SHN_MASK) >> NVMEOF_CC_SHN_SHIFT;
                if (shn && !st.ccShnPrev) {
                    st.cstsShst = true;
                    printf("        shutdown notification requested (CC.SHN=%u) -> CSTS.SHST=complete\n",
                           (unsigned)shn);
                } else if (!shn && st.ccShnPrev) {
                    st.cstsShst = false;
                }
                st.ccShnPrev = shn;
                // CC.EN 1 -> 0 is how a host disables this controller - and it is BOTH
                // "I am leaving" and the first half of a CONTROLLER RESET, which the
                // host follows with a reconnect.  The two are indistinguishable at
                // this level, and the reference keeps answering either way: nvmet
                // clears CSTS.RDY and leaves the queue pair alone, because the
                // transport teardown is the host's move, not the target's.
                //
                // This target used to break out of its controller loop here, which
                // destroyed the queue pairs at exactly the moment the host was about
                // to poll CSTS.RDY to confirm the disable.  The poll then had nowhere
                // to go and the host sat in its 60-second command timeout before it
                // would reconnect - measured: `nvme nvme2: I/O tag 1 (4001) opcode
                // 0x7f (Property Get) QID 0 timeout` sixty seconds into `nvme reset`.
                // Now the controller stays answering (CSTS.RDY reads 0, which is what
                // the host is waiting for) and the loop ends when the host really
                // does drop the queue pairs - ND_CANCELED - or after the idle timeout.
                if (wasEnabled && !st.enabled) {
                    st.disabledByHost = true;
                    printf("        controller disabled by the host (CC.EN=0); "
                           "still answering CSTS until the host drops its queue pairs\n");
                }
            } else sc = NVMEOF_SC_INVALID_FIELD;
        } else if (fctype == NVMEOF_FCTYPE_PROPERTY_GET) {
            uint32_t off = nvmeof_rd32(cap + 44);
            // The attrib byte says how wide the register is, and a target is
            // expected to check it (nvmet does): CAP is the only 64-bit register
            // here, and asking for it as 32-bit - or for anything else as 64-bit -
            // is Invalid Field.  Ignoring this field is what let our host send the
            // wrong width for months without anything noticing.
            bool eightByte = ((cap[40] & NVMEOF_PROP_ATTRIB_SIZE_MASK) == NVMEOF_PROP_SIZE_8);
            if (eightByte && off != NVMEOF_PROP_CAP) {
                printf("  [admin] Property Get: 8-byte read of non-CAP offset 0x%X\n", off);
                sc = NVMEOF_SC_INVALID_FIELD;
            } else if (!eightByte && off == NVMEOF_PROP_CAP) {
                printf("  [admin] Property Get: CAP needs an 8-byte read\n");
                sc = NVMEOF_SC_INVALID_FIELD;
            } else switch (off) {
            case NVMEOF_PROP_CAP:   result = NVMEOF_CAP_VALUE; break;
            case NVMEOF_PROP_VS:    result = 0x00010400u; break;
            case NVMEOF_PROP_CC:    result = st.enabled ? NVMEOF_CC_EN : 0u; break;
            case NVMEOF_PROP_CSTS:
                // RDY, CFS and SHST - all three are read by a real host and only the
                // first was ever reported here.
                result = (st.enabled ? NVMEOF_CSTS_RDY : 0u) |
                         ((st.cstsCfs || st.authFatal > 0) ? NVMEOF_CSTS_CFS : 0u) |
                         (st.cstsShst ? NVMEOF_CSTS_SHST_CMPLT : 0u);
                break;
            default: sc = NVMEOF_SC_INVALID_FIELD; break;
            }
            printf("  [admin] Property Get off=0x%X -> 0x%llX\n", off,
                   (unsigned long long)result);
        } else if (fctype == NVMEOF_FCTYPE_CONNECT) {
            uint16_t qid = nvmeof_rd16(cap + 42);
            st.connects++;
            // The Connect capsule is where a real host states its keep-alive timer
            // (in MILLISECONDS: nvme_rdma sends `ctrl->kato * 1000`, and nvmet stores
            // DIV_ROUND_UP(kato, 1000) seconds).  This target printed the value and
            // threw it away, so st.katoMs stayed 0 - and a controller with KATO 0 is
            // required to answer every Keep Alive with "Keep Alive Timeout Invalid".
            // The Linux host therefore failed its first keep-alive, started error
            // recovery, froze the I/O queue and hung: 13 of 28 reads were still in
            // flight when this target then exited after 120 idle seconds
            // (DESIGN 8.47).
            if (qid == 0 && st.katoMs == 0) {
                st.katoMs = (int)nvmeof_rd32(cap + 48);
            }
            printf("  [admin] fabrics Connect qid=%u sqsize=%u kato=%u\n", qid,
                   nvmeof_rd16(cap + 44), nvmeof_rd32(cap + 48));
            if (!sgl.present || sgl.len < NVMEOF_CONNECT_DATA_SIZE) {
                printf("        connect data: SGL unusable (present=%d len=%u, need %u)\n",
                       (int)sgl.present, sgl.len, (unsigned)NVMEOF_CONNECT_DATA_SIZE);
                sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_FORMAT;
            } else if (!q.read(d.region(kConnectOff), NVMEOF_CONNECT_DATA_SIZE,
                               sgl.addr, sgl.key, CTX_DATA)) {
                printf("        connect data: RDMA Read of %u bytes was NOT posted\n",
                       (unsigned)NVMEOF_CONNECT_DATA_SIZE);
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                printf("        connect data: RDMA Read posted (addr=0x%llX key=0x%08X len=%u)\n",
                       (unsigned long long)sgl.addr, sgl.key,
                       (unsigned)NVMEOF_CONNECT_DATA_SIZE);
                if (!waitForData(q, d, st, CTX_DATA)) {
                    printf("        connect data: the Read did not complete - no answer is sent\n");
                    sc = NVMEOF_SC_DATA_XFER_ERROR;
                }
                else if (qid == 0) {
                    result = st.cntlid;
                    const char* hostNqn = (const char*)d.region(kConnectOff) + 512;
                    const char* wantNqn = (const char*)d.region(kConnectOff) + 256;
                    printf("        hostnqn='%s'\n        subsysnqn='%s'\n", hostNqn, wantNqn);
                    // Which subsystem the host asked for decides what kind of
                    // controller this is.  nvmet looks the name up and answers
                    // CONNECT_INVALID_PARAM | DNR when there is no such subsystem,
                    // so an unknown name is refused here too rather than being
                    // silently treated as an I/O controller.
                    strncpy_s(st.connSubnqn, sizeof(st.connSubnqn), wantNqn, _TRUNCATE);
                    st.discovery = (strcmp(wantNqn, NVMEOF_DISC_SUBSYS_NAME) == 0);
                    if (st.discovery) {
                        printf("        -> discovery controller (Get Log Page lid=0x%02X)\n",
                               NVMEOF_LOG_DISC);
                    } else if (strcmp(wantNqn, st.subnqn) != 0) {
                        printf("        no such subsystem on this target\n");
                        sct = NVMEOF_SCT_COMMAND_SPECIFIC;
                        sc  = NVMEOF_SC_CONNECT_INVALID_PARAM;
                    }
                    // DH-HMAC-CHAP is set up HERE, from the Connect data, because
                    // that is where the two NQNs come from: the host key is bound to
                    // the host NQN and the controller key to the subsystem NQN, and
                    // neither NQN appears anywhere in the auth payloads.  A target
                    // that authenticated against the wrong NQN derives a different
                    // Kt and rejects a host that did everything right.
                    //
                    // A DISCOVERY controller never authenticates, and that is the
                    // reference's rule, not a shortcut: nvmet_setup_auth() returns
                    // before it looks at any host key when the subsystem is the
                    // discovery one, so `nvmet_has_auth()` is false and ATR is never
                    // set.  Setting it here would break `nvme discover` outright -
                    // that command takes no secret, so the host would see the bit,
                    // find it has no key, and fail the connect with -ENOKEY.
                    if (st.discovery && g_targetAuthKey && *g_targetAuthKey) {
                        printf("        discovery controller: no authentication "
                               "(nvmet_setup_auth returns early for the discovery "
                               "subsystem, so ATR is never set here)\n");
                    }
                    if (!st.discovery && sc == NVMEOF_SC_SUCCESS &&
                        g_targetAuthKey && *g_targetAuthKey) {
                        st.auth.dhGroupId = g_targetDhGroup;
                        if (!st.auth.init(g_targetAuthKey, hostNqn, st.subnqn)) {
                            // The reference turns a bad configured key into a
                            // CONNECT failure, not into an auth failure: the key is
                            // the operator's input and no host can fix it.
                            printf("        host key unusable: %s -> CONNECT failure\n",
                                   st.auth.failWhy);
                            sct = NVMEOF_SCT_COMMAND_SPECIFIC;
                            sc  = NVMEOF_SC_CONNECT_INVALID_HOST;
                        } else {
                            st.authAtConnect = true;
                            // A bidirectional target holds a controller key too, and
                            // must answer with its own response (rvalid=1) for the
                            // host to be able to verify it.
                            if (g_targetCtrlKey && *g_targetCtrlKey &&
                                !nvmeof_dhchap::parseKey(g_targetCtrlKey, st.auth.ctrlKey)) {
                                printf("        controller key unusable: ignoring it "
                                       "(one-way authentication)\n");
                                st.auth.ctrlKey = nvmeof_dhchap::Key();
                            }
                            // ATR: bit 17 of the Connect RESULT.  Same dword as the
                            // controller id, which is why it is easy to drop.
                            result = (uint64_t)(st.cntlid | NVMEOF_CONNECT_AUTHREQ_ATR);
                            printf("        authentication required: ATR set "
                                   "(result=0x%llX), key hash=0x%02X dhgroup=%u%s\n",
                                   (unsigned long long)result, st.auth.hashId,
                                   (unsigned)st.auth.dhGroupId,
                                   st.auth.ctrlKey.valid ? ", bidirectional" : "");
                        }
                    }
                } else if (nvmeof_rd16(d.region(kConnectOff) + 16) != st.cntlid) {
                    printf("        I/O queue names cntlid %u, expected %u\n",
                           nvmeof_rd16(d.region(kConnectOff) + 16), st.cntlid);
                    sct = NVMEOF_SCT_COMMAND_SPECIFIC; sc = NVMEOF_SC_CONNECT_INVALID_PARAM;
                } else {
                    result = st.cntlid;
                }
            }
        } else if (fctype == NVMEOF_FCTYPE_AUTH_SEND ||
                   fctype == NVMEOF_FCTYPE_AUTH_RECEIVE) {
            // Both auth commands are legal on the admin queue before the controller
            // is enabled, and both complete with status 0 even when the *payload*
            // says the exchange failed: Failure1 is a message, not a status.
            targetAuth(q, d, st, cap, sgl, fctype == NVMEOF_FCTYPE_AUTH_SEND,
                       &sct, &sc, &dnr);
        } else {
            // nvmet answers an unknown fctype with Invalid Opcode | DNR.  There is
            // no disconnect fctype to special-case: a host disables the controller
            // (CC.EN=0, handled above) and drops the queue pairs.
            printf("  [admin] unhandled fabrics fctype=0x%02X\n", fctype);
            st.unknownAdmin++;
            sct = NVMEOF_SCT_GENERIC;
            sc = NVMEOF_SC_INVALID_OPCODE;
        }
    } else if (opcode == NVMEOF_OPC_SET_FEATURES || opcode == NVMEOF_OPC_GET_FEATURES) {
        bool isSet = (opcode == NVMEOF_OPC_SET_FEATURES);
        uint8_t  fid = (uint8_t)(nvmeof_rd32(cap + 40) & 0xff);
        uint32_t cdw11 = nvmeof_rd32(cap + 44);
        if (!isSet && sgl.len != 0) {
            // A Get Features capsule that carries a data buffer.  nvmet refuses every
            // one of them with "Data SGL Length Invalid", whatever the fid, and that was
            // measured over eight rows - including fids it implements and answers in the
            // completion (0x01 arbitration, 0x06 VWC, 0x07 number of queues) and the
            // data-structure fids nvme-cli attaches a buffer to by itself (0x03 LBA
            // range, 0x0c auto power state, 0x0e timestamp, 0x16 host behaviour,
            // 0x17 sanitize).  The earlier guess here was a per-fid "Invalid Field",
            // which is what a fid-by-fid reading of nvmet's switch produces and is
            // wrong: the check is on the TRANSFER LENGTH, before the fid is looked at
            // (linux/f5_nvmet_ref.sh, f5_nvmet_ref3.sh).
            printf("  [admin] Get feature fid=0x%02X carries %u bytes: the reference "
                   "answers SGL length invalid to any data-buffer Get Features\n",
                   fid, sgl.len);
            sct = NVMEOF_SCT_GENERIC;
            sc  = NVMEOF_SC_SGL_INVALID_DATA;
            dnr = true;
        } else switch (fid) {
        case NVMEOF_FID_NUM_QUEUES:
            if (isSet) {
                uint16_t want = (uint16_t)((cdw11 & 0xffff) + 1);
                // Grant only what this target can actually serve: the answer here is
                // the contract, and the target really owns kMaxIoQueues queue pairs,
                // capsule rings and slot tables - see kMaxIoQueues.  Answering a
                // larger number is how a real host creates queues this target cannot
                // accept and then fails on the second one (DESIGN 8.47).
                st.ioQueuesGranted = (want > kMaxIoQueues) ? kMaxIoQueues : want;
            }
            result = (uint64_t)((st.ioQueuesGranted - 1) & 0xffff) |
                     ((uint64_t)((st.ioQueuesGranted - 1) & 0xffff) << 16);
            printf("  [admin] %s Number of Queues -> %d\n",
                   isSet ? "Set" : "Get", st.ioQueuesGranted);
            break;
        case NVMEOF_FID_KATO:
            if (isSet) {
                st.katoMs = (int)cdw11;
                result = (uint64_t)((st.katoMs + 999) / 1000);   // seconds, as nvmet
            } else {
                result = (uint64_t)st.katoMs;                    // milliseconds
            }
            printf("  [admin] %s KATO -> %llu\n", isSet ? "Set" : "Get",
                   (unsigned long long)result);
            break;
        case NVMEOF_FID_VWC:
            // Volatile Write Cache.  nvmet answers the constant 1 to a GET
            // (admin-cmd.c: `case NVME_FEAT_VOLATILE_WC: nvmet_set_result(req, 1)`)
            // even on a target with no write cache at all, and that is what a host
            // reads back - so this cannot be derived from the Identify's VWC bit
            // without differing from the reference.
            //
            // A SET is refused, and that is measured too: nvmet has no case for
            // NVME_FEAT_VOLATILE_WC in nvmet_execute_set_features, so it falls through
            // to Invalid Field | DNR.  This target used to accept the Set and print
            // "nothing is stored" - a friendlier answer than the reference's, which is
            // exactly the kind of difference that hides a real one.
            if (isSet) {
                printf("  [admin] Set Volatile Write Cache -> Invalid Field | DNR "
                       "(nvmet has no set case for it)\n");
                sc = NVMEOF_SC_INVALID_FIELD;
                dnr = true;
                break;
            }
            result = 1;
            printf("  [admin] Get Volatile Write Cache -> 1 (as nvmet)\n");
            break;
        case NVMEOF_FID_ASYNC_EVENT:
            // Async Event Configuration.  The reference stores the mask and returns
            // the same value from the Set as well as from a later Get
            // (`WRITE_ONCE(aen_enabled, val32); nvmet_set_result(req, val32);`), so a
            // host that sets it and reads it back sees what it wrote.
            if (isSet) st.aenMask = cdw11;
            result = st.aenMask;
            printf("  [admin] %s Async Event Configuration -> 0x%08X\n",
                   isSet ? "Set" : "Get", (unsigned)st.aenMask);
            break;
        case NVMEOF_FID_TEMP_THRESH:
            // Temperature Threshold.  The reference refuses this one too - its switch
            // has `#if 0 case NVME_FEAT_TEMP_THRESH:` and falls through to the same
            // answer - and it is spelled out as its own case rather than left to the
            // default so that "refused on purpose, exactly as nvmet does" stays
            // distinguishable from "not implemented" in the counter a real host's
            // feature sweep is judged by.
            printf("  [admin] %s Temperature Threshold -> Invalid Field | DNR (as nvmet)\n",
                   isSet ? "Set" : "Get");
            sc = NVMEOF_SC_INVALID_FIELD;
            dnr = true;
            break;
        default:
            printf("  [admin] unhandled %s feature fid=0x%02X -> Invalid Field | DNR\n",
                   isSet ? "Set" : "Get", fid);
            st.unknownAdmin++;
            sc = NVMEOF_SC_INVALID_FIELD;
            dnr = true;
            break;
        }
    } else if (opcode == NVMEOF_OPC_KEEP_ALIVE) {
        if (st.katoMs == 0) {
            // A controller whose keep-alive timer is disabled must answer this way
            // (the host then knows the feature is off rather than that it was
            // ignored) - but say so in the log, because a host that hits this goes
            // into error recovery and the symptom on its side is a hung I/O queue.
            printf("  [admin] Keep Alive while KATO == 0 -> KA_TIMEOUT_INVALID "
                   "(the host will treat this as a failed keep-alive)\n");
            sc = NVMEOF_SC_KA_TIMEOUT_INVALID;
        }
        else {
            st.keepAlives++;
            printf("  [admin] Keep Alive #%d (kato=%d ms)\n", st.keepAlives, st.katoMs);
        }
    } else if (opcode == NVMEOF_OPC_IDENTIFY) {
        uint8_t cns = cap[40];
        uint32_t nsid = nvmeof_rd32(cap + 4);
        printf("  [admin] Identify cns=%u nsid=%u\n", cns, nsid);
        uint8_t* src = nullptr;
        if (cns == NVMEOF_ID_CNS_CTRL) { fillIdentifyCtrl(d.region(kIdOff), st); src = d.region(kIdOff); }
        else if (cns == NVMEOF_ID_CNS_NS && (nsid == 1 || nsid == 0xFFFFFFFFu)) {
            fillIdentifyNs(d.region(kIdOff)); src = d.region(kIdOff);
        } else if (cns == NVMEOF_ID_CNS_NS_ACTIVE_LIST) {
            uint8_t* list = d.region(kIdOff);
            memset(list, 0, NVMEOF_IDENTIFY_SIZE);
            nvmeof_wr32(list, 1);
            src = list;
        } else if (cns == NVMEOF_ID_CNS_NS_DESC_LIST) {
            uint8_t* list = d.region(kIdOff);
            memset(list, 0, NVMEOF_IDENTIFY_SIZE);
            list[0] = NVMEOF_NIDT_CSI; list[1] = NVMEOF_NIDT_CSI_LEN;
            list[4] = NVMEOF_CSI_NVM;
            src = list;
        } else if (cns == NVMEOF_ID_CNS_CS_CTRL || cns == NVMEOF_ID_CNS_CS_NS) {
            // Command-set-specific Identify: zeroes are a legal answer and are
            // what a host expects from a controller with no CSI-specific data.
            memset(d.region(kIdOff), 0, NVMEOF_IDENTIFY_SIZE);
            src = d.region(kIdOff);
        } else {
            printf("        unhandled identify cns=%u\n", cns);
            st.unknownAdmin++;
            sc = NVMEOF_SC_INVALID_FIELD;
        }
        if (src) {
            // nvmet checks the transfer length against the structure size BEFORE it
            // fills anything (nvmet_check_transfer_len); without the same check a host
            // that offered a short buffer would get 4096 bytes written past the end of
            // it - the RDMA Write past the SGL that DESIGN 8.47 records.
            if (sgl.len != NVMEOF_IDENTIFY_SIZE) {
                printf("        Identify carries %u bytes, not %u -> SGL_INVALID_DATA\n",
                       sgl.len, (unsigned)NVMEOF_IDENTIFY_SIZE);
                sc = NVMEOF_SC_SGL_INVALID_DATA;
            } else if (!carryData(q, d, st, "Identify", true, src, NVMEOF_IDENTIFY_SIZE,
                                  sgl.addr, sgl.key, CTX_DATA)) {
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            }
        }
    } else if (opcode == NVMEOF_OPC_ASYNC_EVENT) {
        // Async Event: a command a real controller ACCEPTS AND DOES NOT COMPLETE.
        //
        // Both easy answers are wrong.  Answering success would tell the host an
        // event had arrived (it would look up the event type in a completion that
        // says nothing); answering an error would tell it the command was
        // refused.  The command is held until there is something to report, which
        // for a controller with a static namespace may be never - that is a
        // legitimate implementation, and it is what the reference does.
        //
        // The limit is the reference's: nvmet keeps up to NVMET_ASYNC_EVENTS (4)
        // and answers NVME_SC_ASYNC_LIMIT beyond that.
        if (st.pendingAers >= NVMEOF_AER_MAX_PENDING) {
            printf("  [admin] Async Event #%d -> ASYNC_LIMIT (already holding %d)\n",
                   st.pendingAers + 1, st.pendingAers);
            sct = NVMEOF_SC_ASYNC_LIMIT_SCT;
            sc  = NVMEOF_SC_ASYNC_LIMIT_SC;
        } else {
            st.pendingAers++;
            st.deferredAers++;
            printf("  [admin] Async Event accepted and HELD (%d outstanding, no completion)\n",
                   st.pendingAers);
            // No completion: re-arm THIS slot so the next admin command still
            // lands, and answer nothing.
            q.postReceive(d.region(capOff), 64, (void*)(CTX_ADMIN_CAP_BASE + (uintptr_t)slot));
            return true;
        }
    } else if (opcode == NVMEOF_OPC_ABORT) {
        // Mandatory in the spec and trivial to answer: "no, that command was not
        // found".  A host that loses a command sends this.
        printf("  [admin] Abort (answered: command not found)\n");
        result = 1;
    } else if (opcode == NVMEOF_OPC_GET_LOG_PAGE) {
        // `nvme list` and `nvme smart-log` read log pages, and for the pages a
        // target has nothing to say about, zeroes with a success status are the
        // legal answer.  WHICH PAGES those are is not a free choice - it is the
        // measured behaviour of the reference, and the difference is not cosmetic:
        //
        //   LID   nvmet 6.18 (measured)              this target
        //   0x01  success                            success, zero-filled
        //   0x02  success                            success, zero-filled
        //   0x03  success                            success, zero-filled
        //   0x0c  success (ANA)                      success, zero-filled
        //   0x05  SGL length invalid at 512 B        success (the effects log is real here)
        //   0x04  Internal Error                     Invalid Field | DNR
        //   0x09  Invalid Namespace or Format        Invalid Field | DNR
        //   0x12  SGL length invalid at 512 B        Invalid Field | DNR
        //   0x16  Invalid Namespace or Format        Invalid Field | DNR
        //   else  Invalid Field | DNR                Invalid Field | DNR
        //
        // The row that used to be wrong is 0x0d (Persistent Event Log), and it cost a
        // controller.  `nvme persistent-event-log` reads the 512-byte event-log header
        // FIRST; a zero-filled success tells it the header is fine, so it goes on to
        // issue the SAME log page with numd=0xFFFFFFFF and a ZERO-BYTE SGL.  Answering
        // success for every LID walked the host straight into the one request that
        // killed the queue pair; nvmet answers Invalid Field and the host stops after
        // the first read (DESIGN 9.1).  The four rows nvmet answers with an odd status
        // are nvmet failing to produce the page rather than a specification of the
        // right answer: an error is an error, so this target answers the plain one.
        //
        // The length is NUMDL (CDW10 bits 31:16) plus NUMDU (CDW11 bits 15:0), in
        // DWORDs, 0-based - and the LID is the LOW byte of CDW10, which is why
        // reading the length out of bytes 40..43 mixes the LID into it.  That is
        // what this code did: a Linux host's 512-byte error-log read was computed as
        // 33292300 bytes, clamped to 4096, and written into the host's 512-byte
        // buffer - an RDMA Write past the end of the peer's SGL, which came back
        // ND_ACCESS_VIOLATION and killed the queue pair (DESIGN 8.47).
        //
        // `asked` is computed in 64 bits ON PURPOSE.  `(numd + 1) * 4` in 32 bits
        // wraps to 0 for numd=0xFFFFFFFF, which is the value the host really sent:
        // the wrap made a 16 GiB request look like a 0-byte one, the length check
        // below then agreed with the host's 0-byte SGL, and the target posted a
        // 0-byte RDMA Write that never completed - the admin queue stopped there and
        // the host's Keep Alive timer expired 7.6 s after the connect.
        uint32_t numd = (uint32_t)nvmeof_rd16(cap + 42) |
                        ((uint32_t)nvmeof_rd16(cap + 44) << 16);
        uint64_t asked64 = ((uint64_t)numd + 1) * 4;
        uint32_t asked = asked64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)asked64;
        // Log Page Offset (LPOL/LPOU, CDW12/CDW13).  A discovery log is read in
        // chunks - the host asks for a slice at an offset - so ignoring this field
        // returns the first chunk over and over and the host sees the same records
        // forever (or, worse, records it already has).
        uint64_t lpo = (uint64_t)nvmeof_rd32(cap + 48) |
                       ((uint64_t)nvmeof_rd32(cap + 52) << 32);
        bool supported = (cap[40] == NVMEOF_LOG_ERROR ||
                          cap[40] == NVMEOF_LOG_SMART ||
                          cap[40] == NVMEOF_LOG_FW_SLOT ||
                          cap[40] == NVMEOF_LOG_ANA ||
                          cap[40] == NVMEOF_LOG_CMD_EFFECTS);
        if (cap[40] == NVMEOF_LOG_DISC) {
            printf("  [admin] Get Log Page lid=0x%02X (discovery) numd=%u -> %u bytes at offset %llu\n",
                   cap[40], numd, asked, (unsigned long long)lpo);
        } else if (st.discovery) {
            // nvmet answers Invalid Field here, and this was measured rather than
            // assumed: `-discover` against Linux nvmet got sct=0 sc=0x02 for the error
            // log (LID 1) and the effects log (LID 5) on a discovery controller, whose
            // only log page is the discovery one (DESIGN 8.50).
            printf("  [admin] Get Log Page lid=%u refused: this is a discovery controller\n", cap[40]);
            sct = NVMEOF_SCT_GENERIC;
            sc  = NVMEOF_SC_INVALID_FIELD;
        } else if (!supported) {
            printf("  [admin] Get Log Page lid=0x%02X numd=%u -> Invalid Field | DNR "
                   "(the reference refuses this page too; answering zeros here made\n"
                   "          nvme-cli's persistent-event-log send a second request that "
                   "killed the queue pair)\n", cap[40], numd);
            sct = NVMEOF_SCT_GENERIC;
            sc  = NVMEOF_SC_INVALID_FIELD;
            dnr = true;
        } else {
            printf("  [admin] Get Log Page lid=%u numd=%u -> %u bytes at offset %llu (zero-filled)\n",
                   cap[40], numd, asked, (unsigned long long)lpo);
        }
        // Exactly like the reference: the SGL's length IS the transfer length, and
        // the length the command asks for has to match it (nvmet_check_transfer_len).
        // Writing SGL-length bytes but only initialising `asked` of them would leak
        // whatever the buffer held before.
        if (sc != 0) {
            // Already refused above - do not touch the host's buffer.
        } else if (asked64 != sgl.len) {
            printf("        the command asks for %llu bytes but the SGL carries %u\n",
                   (unsigned long long)asked64, sgl.len);
            sc = NVMEOF_SC_SGL_INVALID_DATA;
        } else if (lpo > NVMEOF_IDENTIFY_SIZE) {
            // The host should not ask past the end; if it does, answer with an error
            // rather than silently returning the wrong slice.
            printf("        log page offset %llu is past the %u-byte page\n",
                   (unsigned long long)lpo, (unsigned)NVMEOF_IDENTIFY_SIZE);
            sc = NVMEOF_SC_INVALID_FIELD;
        } else if (asked == 0) {
            // A zero-byte transfer is not a no-op an HCA completes: the request is
            // posted and never reaped, and this target answers one capsule at a time
            // per queue.  Nothing to write and nothing to wait for - complete the
            // command here (see carryData() for the full story).
            printf("        the host asked for 0 bytes: nothing is posted\n");
        } else {
            uint8_t* buf = d.region(kIdOff);
            if (asked > NVMEOF_IDENTIFY_SIZE) asked = NVMEOF_IDENTIFY_SIZE;
            memset(buf, 0, NVMEOF_IDENTIFY_SIZE);
            uint32_t avail = (uint32_t)(NVMEOF_IDENTIFY_SIZE - lpo);
            if (asked > avail) asked = avail;
            if (cap[40] == NVMEOF_LOG_DISC) {
                buildDiscoveryLog(buf, d, st);
                st.discLogReads++;
                printf("        %llu record(s) in the log; sending %u bytes from offset %llu\n",
                       (unsigned long long)nvmeof_rd64(buf + 8), asked,
                       (unsigned long long)lpo);
            }
            if (!q.write(buf + lpo, asked, sgl.addr, sgl.key, CTX_DATA)) sc = NVMEOF_SC_DATA_XFER_ERROR;
            else if (!waitForData(q, d, st, CTX_DATA)) sc = NVMEOF_SC_DATA_XFER_ERROR;
        }
    } else {
        printf("  [admin] UNHANDLED opcode=0x%02X - a real host sent this\n", opcode);
        st.unknownAdmin++;
        sc = NVMEOF_SC_INVALID_OPCODE;
    }

    if (dnr) buildCompletionDnr(d.region(respOff), cid, 0, result, sct, sc);
    else     buildCompletion   (d.region(respOff), cid, 0, result, sct, sc);
    q.postReceive(d.region(capOff), 64, (void*)(CTX_ADMIN_CAP_BASE + (uintptr_t)slot));
    return q.send(d.region(respOff), 16, CTX_RESP);
}

// Answer one I/O capsule.  Same shape as the admin path, with the NVM command
// set: Read, Write, Flush - and the SGL subtypes a real host marks.
//
// `qi` is the I/O queue this capsule arrived on, which decides three things: which
// capsule ring slot to re-arm, which response buffer to fill, and which in-flight
// table the command occupies.  Getting any of them wrong shows up as a host whose
// I/O hangs on one queue while another keeps working.
static bool targetIo(Queue& q, Device& d, F5State& st, int qi, int slot) {
    const size_t capOff  = kIoCapOff  + (size_t)qi * kIoCapStride  + (size_t)slot * 64;
    const size_t respOff = kIoRespOff + (size_t)qi * kIoRespStride + (size_t)slot * 16;
    const uint8_t* cap = d.region(capOff);
    uint8_t  op = cap[0];
    uint16_t cid = nvmeof_rd16(cap + 2);
    uint8_t  sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
    // Do Not Retry: nvmet sets it on the Connect rejections below, and a host that
    // is told to retry a queue it may not have will retry it forever.
    bool     dnr = false;

    // Same pre-parse checks nvmet does on every queue, admin or I/O.
    if (!nvmeof_sqe_flags_ok(cap[1])) {
        printf("  [io]    SQE flags 0x%02X refused: nvmet wants PSDT = 0b01 (0x40)\n", cap[1]);
        st.unknownIo++;
        buildCompletionDnr(d.region(respOff), cid, 0, 0,
                           NVMEOF_SCT_GENERIC, NVMEOF_SC_INVALID_FIELD);
        q.postReceive(d.region(capOff), 64, CTX_IO_CAP(qi, slot));
        return q.send(d.region(respOff), 16, CTX_IO_RESP(qi, slot));
    }
    {
        uint8_t dsct = NVMEOF_SCT_GENERIC, dsc = NVMEOF_SC_SUCCESS;
        if (nvmeof_sgl_check_rdma(cap + 24, op == NVMEOF_OPC_WRITE ? 1 : 0, &dsct, &dsc)) {
            printf("  [io]    dptr 0x%02X refused (nvmet's RDMA transport rule)\n", cap[39]);
            st.unknownIo++;
            buildCompletionDnr(d.region(respOff), cid, 0, 0, dsct, dsc);
            q.postReceive(d.region(capOff), 64, CTX_IO_CAP(qi, slot));
            return q.send(d.region(respOff), 16, CTX_IO_RESP(qi, slot));
        }
    }

    if (op == NVMEOF_OPC_FLUSH) {
        st.flushes++;
        st.ioPerQueue[qi]++;
        // A file-backed namespace has something to do here; a memory one does not.
        // This is the whole reason the backend writes through on FLUSH: a real host
        // mounts a filesystem on this namespace and depends on FLUSH meaning "it is
        // on the medium", so answering it without persisting would be a lie the host
        // cannot see until it reboots.
        if (g_nsHandle != INVALID_HANDLE_VALUE) {
            if (!nsFileFlush(d, false)) sc = NVMEOF_SC_INTERNAL;
            else printf("  [io]    q%d FLUSH -> %u blocks written to %s\n", qi + 1,
                        (unsigned)g_nsBlocks, g_nsFile);
        } else {
            printf("  [io]    q%d FLUSH (memory namespace: nothing to persist)\n", qi + 1);
        }
    } else if (op == NVMEOF_OPC_WRITE || op == NVMEOF_OPC_READ) {
        uint32_t nsid = nvmeof_rd32(cap + 4);
        uint64_t slba = nvmeof_rd64(cap + 40);
        uint32_t blocks = (uint32_t)(nvmeof_rd32(cap + 48) & 0xFFFF) + 1;
        uint32_t bytes = blocks * kBlockSize;
        Sgl sgl = parseSgl(cap);
        st.ioCommands++;
        st.ioPerQueue[qi]++;
        if (sgl.invalidate) st.invalidates++;
        // PER-COMMAND I/O PRINT, OFF BY DEFAULT, and the measurement is why: this
        // one line used to run on every READ and WRITE, into an UNBUFFERED stdout
        // (setvbuf _IONBF in main) that a caller usually redirects to a file.  Each
        // printf is then a synchronous write in the middle of the poll loop.
        //
        // Measured (DESIGN 8.68): the same serial 48 MiB read took 9.83 s with the
        // target's stdout going to a file and 0.19 s with it going to NUL - 4.9 MiB/s
        // against 255 MiB/s, a 52x difference from LOGGING, which had been misread as
        // "12-22 ms of completion latency in the RDMA provider" for a whole
        // investigation.  A target that prints per command cannot be benchmarked, and
        // a bridge whose backend prints per command inherits it: the iSCSI bridge's
        // "-iscsitime nvme" was 13.5 ms per command for exactly this reason.
        if (g_ioTrace) {
            printf("  [io]    cmd %d q%d %s nsid=%u slba=%llu blocks=%u sgl(addr=0x%llX key=0x%08X len=%u%s)\n",
                   st.ioCommands, qi + 1, op == NVMEOF_OPC_WRITE ? "WRITE" : "READ", nsid,
                   (unsigned long long)slba, blocks, (unsigned long long)sgl.addr, sgl.key,
                   sgl.len, sgl.invalidate ? " INVALIDATE" : "");
        }
        if (nsid != 1) { sc = NVMEOF_SC_INVALID_NS; }
        else if (slba + blocks > g_nsBlocks) { sc = NVMEOF_SC_LBA_RANGE; }
        else if (!sgl.present) { sc = NVMEOF_SC_SGL_INVALID_TYPE; }
        else if (sgl.len < bytes) { sc = NVMEOF_SC_DATA_XFER_ERROR; }
        else {
            // The Invalidate subtype is SERVED, not refused: a default Linux host
            // marks 0x4f on every read and write, and its completion path
            // invalidates its own key when the response carries no invalidation.
            //
            // The transfer is STARTED here and answered when it completes: this
            // target does not wait for its own data movement, because waiting is
            // what stops it from taking the next capsule - and a host with eight
            // commands in flight sends the next capsule immediately.
            uint8_t* ns = d.region(kNsOff + (size_t)slba * kBlockSize);
            bool ok = (op == NVMEOF_OPC_WRITE)
                          ? q.read(ns, bytes, sgl.addr, sgl.key, CTX_IO_DATA(qi, slot))
                          : q.write(ns, bytes, sgl.addr, sgl.key, CTX_IO_DATA(qi, slot));
            if (!ok) {
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                st.io[qi][slot].used = true;
                st.io[qi][slot].cid = cid;
                st.io[qi][slot].isWrite = (op == NVMEOF_OPC_WRITE);
                st.io[qi][slot].bytes = bytes;
                st.io[qi][slot].slba = slba;
                st.io[qi][slot].t0 = std::chrono::steady_clock::now();
                st.io[qi][slot].tQpc = nowUs();
                if (st.io[qi][slot].isWrite) st.bytesWritten += bytes;
                else st.bytesRead += bytes;
                // Note: this slot's receive is NOT re-armed yet.  It is re-armed
                // when the transfer completes, so one slot carries exactly one
                // command and the ring depth is the number of commands in flight.
                return true;
            }
        }
    } else if (op == NVMEOF_OPC_WRITE_ZEROES) {
        // Write Zeroes: the reference serves it (`nvme write-zeroes` came back rc=0
        // against Linux nvmet, and rc=0 with Invalid Command Opcode against this
        // target before this branch existed - f5_dirb_demo.sh / f5_nvmet_ref.sh).
        // mkfs and blkdiscard use it, and with Deallocate set it is also what a
        // filesystem means by "this region is free".
        uint32_t nsid   = nvmeof_rd32(cap + 4);
        uint64_t slba   = nvmeof_rd64(cap + 40);
        uint32_t cdw12  = nvmeof_rd32(cap + 48);
        uint32_t blocks = (cdw12 & 0xFFFFu) + 1;
        bool     deac   = (cdw12 & NVMEOF_WZ_DEAC) != 0;
        Sgl      sgl    = parseSgl(cap);
        st.ioCommands++;
        st.ioPerQueue[qi]++;
        if (sgl.invalidate) st.invalidates++;
        printf("  [io]    cmd %d q%d WRITE ZEROES nsid=%u slba=%llu blocks=%u deac=%d\n",
               st.ioCommands, qi + 1, nsid, (unsigned long long)slba, blocks, (int)deac);
        // No data phase at all: whatever the host put in the dptr, it is not used -
        // and a length is not allowed there (nvmet checks the transfer length against
        // 0 for every no-data opcode).
        if (nsid != 1) { sc = NVMEOF_SC_INVALID_NS; }
        else if (sgl.len != 0) {
            printf("        a no-data command carries a %u-byte dptr -> SGL_INVALID_DATA\n",
                   sgl.len);
            sc = NVMEOF_SC_SGL_INVALID_DATA;
        }
        else if (slba + blocks > g_nsBlocks) { sc = NVMEOF_SC_LBA_RANGE; }
        else {
            uint32_t bytes = blocks * kBlockSize;
            uint8_t* ns = d.region(kNsOff + (size_t)slba * kBlockSize);
            memset(ns, 0, bytes);
            st.bytesZeroed += bytes;
            st.zeroesCommands++;
            printf("        %u bytes zeroed at LBA %llu%s\n", bytes,
                   (unsigned long long)slba,
                   deac ? " (Deallocate set: the blocks read back as zeros, which is "
                          "what a deallocated block has to read as)" : "");
        }
    } else if (op == NVMEOF_OPC_DSM) {
        // Dataset Management.  The range list is a DATA PHASE THE TARGET READS, and
        // this target reads it synchronously - the same shape the admin path uses for
        // a Connect's 1024 bytes.  It is worth the wait here: the alternative is a
        // third kind of in-flight state (a command whose data has not arrived yet) for
        // a command whose list is at most a few KiB.
        uint32_t nsid = nvmeof_rd32(cap + 4);
        uint32_t nr   = (nvmeof_rd32(cap + 40) & 0xFFu) + 1;      // 0-based
        uint32_t attr = nvmeof_rd32(cap + 44);
        Sgl      sgl  = parseSgl(cap);
        st.ioCommands++;
        st.ioPerQueue[qi]++;
        if (sgl.invalidate) st.invalidates++;
        printf("  [io]    cmd %d q%d DATASET MANAGEMENT nsid=%u ranges=%u attr=0x%08X\n",
               st.ioCommands, qi + 1, nsid, nr, attr);
        if (nsid != 1) { sc = NVMEOF_SC_INVALID_NS; }
        else if (!sgl.present) { sc = NVMEOF_SC_SGL_INVALID_TYPE; }
        else if (sgl.len != nr * NVMEOF_DSM_RANGE_SIZE) {
            printf("        %u range(s) need %u bytes, the SGL carries %u -> SGL_INVALID_DATA\n",
                   nr, nr * NVMEOF_DSM_RANGE_SIZE, sgl.len);
            sc = NVMEOF_SC_SGL_INVALID_DATA;
        }
        else if (nr > NVMEOF_DSM_MAX_RANGES || sgl.len > kDsmBytes) {
            printf("        %u ranges (%u bytes) is more than this target accepts (%u)\n",
                   nr, sgl.len, NVMEOF_DSM_MAX_RANGES);
            sc = NVMEOF_SC_INVALID_FIELD;
        }
        else {
            uint8_t* list = d.region(kDsmOff);
            if (!carryData(q, d, st, "DSM range list", false, list, sgl.len,
                           sgl.addr, sgl.key, CTX_DATA)) {
                sc = NVMEOF_SC_DATA_XFER_ERROR;
            } else {
                // WHICH BIT MEANS "deallocate" was measured, because the two obvious
                // answers differ and only one of them matches the reference:
                //
                //   dsm -b 8 -d        (CDW11 AD only)      nvmet: zeroed   this target: nothing
                //   dsm -b 8 -a 4      (per-range AD only)  nvmet: nothing  this target: zeroed
                //   dsm -b 8 -a 4 -d   (both)               nvmet: zeroed   this target: zeroed
                //   dsm -b 8           (neither)            nvmet: nothing  this target: nothing
                //
                // (linux/f5_dsm_check.sh, run against both targets.)  So nvmet acts on
                // the COMMAND-level bit in CDW11 and ignores the per-range context
                // attributes, and this target had it the other way round - which made
                // `nvme dsm -d`, the form nvme-cli and the kernel's own discard path
                // produce, report success and change nothing.  A per-range AD with no
                // command AD is therefore logged and NOT acted on: the reference does
                // not act on it either, and acting would make the two disagree.
                bool deallocate = (attr & NVMEOF_DSM_AD) != 0;
                // Every range is checked BEFORE anything is zeroed: a command that
                // touches an LBA outside the namespace must change nothing, not half.
                sc = NVMEOF_SC_SUCCESS;
                for (uint32_t i = 0; i < nr; i++) {
                    const uint8_t* r = list + (size_t)i * NVMEOF_DSM_RANGE_SIZE;
                    uint32_t rattr  = nvmeof_rd32(r);
                    uint32_t rblocks = nvmeof_rd32(r + 4) + 1;
                    uint64_t rslba  = nvmeof_rd64(r + 8);
                    if (rslba + rblocks > g_nsBlocks) {
                        printf("        range %u (slba=%llu nlb=%u) is past the %u-block "
                               "namespace -> LBA_RANGE, nothing is changed\n",
                               i, (unsigned long long)rslba, rblocks, (unsigned)g_nsBlocks);
                        sc = NVMEOF_SC_LBA_RANGE;
                        break;
                    }
                    printf("        range %u: slba=%llu nlb=%u ctxattr=0x%08X%s\n", i,
                           (unsigned long long)rslba, rblocks, rattr,
                           (rattr & NVMEOF_DSM_AD) && !deallocate
                               ? " (AD in the range but not in CDW11: the reference "
                                 "does not act on this either)"
                               : (deallocate ? " -> deallocate" : ""));
                }
                if (sc == NVMEOF_SC_SUCCESS && deallocate) {
                    for (uint32_t i = 0; i < nr; i++) {
                        const uint8_t* r = list + (size_t)i * NVMEOF_DSM_RANGE_SIZE;
                        uint32_t rblocks = nvmeof_rd32(r + 4) + 1;
                        uint64_t rslba  = nvmeof_rd64(r + 8);
                        uint8_t* ns = d.region(kNsOff + (size_t)rslba * kBlockSize);
                        memset(ns, 0, (size_t)rblocks * kBlockSize);
                        st.bytesZeroed += rblocks * kBlockSize;
                    }
                    st.dsmCommands++;
                    printf("        %u range(s) deallocated (zeroed, which is what a "
                           "deallocated block has to read as)\n", nr);
                } else if (sc == NVMEOF_SC_SUCCESS) {
                    st.dsmCommands++;
                    printf("        no AD bit in CDW11: the ranges are attributes only, "
                           "nothing is changed (as the reference)\n");
                }
            }
        }
    } else if (op == NVMEOF_OPC_FABRICS) {
        // A fabrics Connect on an I/O queue names the queue id it is establishing.
        // qid 0 is the admin queue (nvmet: CONNECT_INVALID_PARAM + DNR) and an
        // index beyond what this target granted is a queue it has no queue pair
        // for, so accepting it would let a host believe it owns a queue that does
        // not exist.
        //
        // NOT checked here: cntlid.  `struct nvmf_connect_command` has no cntlid
        // field - it is in the connect DATA (offset 16), which only the admin
        // queue transfers (ref/linux_nvme.h).  A first version of this check read
        // the reserved bytes at offset 20 and rejected our own initiator, which is
        // what a check invented instead of read looks like.
        uint16_t qid = nvmeof_rd16(cap + 42);
        printf("  [io]    q%d fabrics Connect (fctype=0x%02X qid=%u)\n", qi + 1, cap[4], qid);
        if (cap[4] != NVMEOF_FCTYPE_CONNECT) { sc = NVMEOF_SC_INVALID_OPCODE; }
        else if (qid == 0 || qid > st.ioQueuesGranted) {
            sct = NVMEOF_SCT_COMMAND_SPECIFIC;
            sc  = NVMEOF_SC_CONNECT_INVALID_PARAM;
            dnr = true;
        }
    } else {
        printf("  [io]    UNHANDLED opcode=0x%02X - a real host sent this\n", op);
        st.unknownIo++;
        sc = NVMEOF_SC_INVALID_OPCODE;
    }

    if (dnr) buildCompletionDnr(d.region(respOff), cid, 0, 0, sct, sc);
    else     buildCompletion(d.region(respOff), cid, 0, 0, sct, sc);
    q.postReceive(d.region(capOff), 64, CTX_IO_CAP(qi, slot));
    return q.send(d.region(respOff), 16, CTX_IO_RESP(qi, slot));
}

// The other half: a data transfer for (queue `qi`, slot `slot`) has completed, so
// the command can be answered and that slot can take the next one.
static bool targetIoComplete(Queue& q, Device& d, F5State& st, int qi, int slot, HRESULT status) {
    const size_t capOff  = kIoCapOff  + (size_t)qi * kIoCapStride  + (size_t)slot * 64;
    const size_t respOff = kIoRespOff + (size_t)qi * kIoRespStride + (size_t)slot * 16;
    uint8_t sct = NVMEOF_SCT_GENERIC, sc = NVMEOF_SC_SUCCESS;
    if (!ndOk(status)) {
        char b[64];
        printf("  [io]    q%d slot %d data transfer %s\n", qi + 1, slot, ndStr(status, b, sizeof(b)));
        sc = NVMEOF_SC_DATA_XFER_ERROR;
    }
    buildCompletion(d.region(respOff), st.io[qi][slot].cid, 0, 0, sct, sc);
    st.io[qi][slot].used = false;
    if (g_nvmeTiming) {
        printf("    [nvme] target got capsule qpc=%lld, %s %u B: %lld us from capsule to answer\n",
               st.io[qi][slot].tQpc,
               st.io[qi][slot].isWrite ? "READ(data-in)" : "WRITE(data-out)",
               st.io[qi][slot].bytes,
               (long long)std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - st.io[qi][slot].t0).count());
    }
    // Re-arm this slot BEFORE answering: the host may send the next command the
    // instant it sees the completion (DESIGN 8.10(4)).
    q.postReceive(d.region(capOff), 64, CTX_IO_CAP(qi, slot));
    auto tPost = std::chrono::steady_clock::now();
    bool sent = q.send(d.region(respOff), 16, CTX_IO_RESP(qi, slot));
    if (g_nvmeTiming) {
        // How long the SEND call itself takes, and how long its own completion takes
        // to appear locally.  Together they separate "the reply never made it onto the
        // wire" from "the reply left but the initiator did not see it": the initiator
        // measured 23-45 ms waiting for this 16-byte reply while this target produced
        // it 31 us after the capsule.
        auto tCall = std::chrono::steady_clock::now();
        ND2_RESULT rr = {};
        HRESULT sst = q.reap(CTX_IO_RESP(qi, slot), &rr, 2000);
        auto tDone = std::chrono::steady_clock::now();
        printf("    [nvme] target reply: send call %lld us, own completion %lld us (%s) qpc=%lld\n",
               (long long)std::chrono::duration_cast<std::chrono::microseconds>(tCall - tPost).count(),
               (long long)std::chrono::duration_cast<std::chrono::microseconds>(tDone - tPost).count(),
               ndOk(sst) ? "ok" : "not seen in 2 s",
               nowUs());
    }
    return sent;
}

static int runTarget(const char* ip, uint16_t port, int maxControllers) {
    printf("=== F5 TARGET on %s:%u (waiting for a foreign host: nvme-cli, or F5 -initiator)\n",
           ip, port);
    printf("    controllers this run will serve: %s\n",
           maxControllers == 0 ? "unlimited" : (maxControllers == 1 ? "1" : "as many as asked"));
    printf("    subnqn '%s'\n", "nqn.2024-01.local.rdma:windows-nd");
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    InetPtonA(AF_INET, ip, &local.sin_addr);

    Device dev;
    if (!dev.open(local, kRegionBytes)) return 1;

    // The namespace backend.  Without -nsfile the region stays what it always was: a
    // memory buffer with no medium behind it.  With it, the file is the medium and the
    // region is only its cache.
    if (g_nsFile) {
        if (!nsFileOpen(dev, g_nsFile)) return 1;
    } else {
        printf("    namespace: %u MiB in memory (no -nsfile: nothing survives the run)\n",
               (unsigned)((g_nsBlocks * kBlockSize) >> 20));
    }

    IND2Listener* listener = nullptr;
    HRESULT hr = dev.adapter->CreateListener(IID_IND2Listener, dev.ovFile, (VOID**)&listener);
    if (FAILED(hr)) { printf("CreateListener failed\n"); return 1; }
    hr = listener->Bind((const sockaddr*)&local, sizeof(local));
    if (!ndOk(hr)) {
        char b[64];
        printf("listener Bind %s\n", ndStr(hr, b, sizeof(b)));
        // 0xC0000043 is an NTSTATUS (STATUS_SHARING_VIOLATION), NOT an HRESULT:
        // HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) is 0x80070005 and would never
        // match.  The ND provider hands back NTSTATUS values directly, which is also
        // why ndStr prints them in that form.  It means the RDMA-CM port is STILL
        // HELD, and the two things that hold it are worth naming because the value
        // says nothing and the failure it produces is misleading: the initiator
        // reports "connection refused", which reads like the target never started.
        // The usual cause is a previous process on this port killed with -Force while
        // it owned the listener (measured: killing an f5_interop backend that served
        // on 4420 left that port unusable for every later run, with no process and no
        // netstat entry to show for it - the state lives in the provider, not in a
        // process).  Restarting the adapter that owns the address clears it, and a
        // port that was never used (4421 in that same run) bound normally throughout.
        if ((uint32_t)hr == 0xC0000043u) {
            printf("          the RDMA-CM port is held by a previous process.  Either use\n"
                   "          another port (-target <ip> <port>), or clear it by restarting\n"
                   "          the adapter that owns the address, e.g.\n"
                   "            Get-NetAdapter -InterfaceIndex <idx> | Restart-NetAdapter\n");
        }
        return 1;
    }
    // The backlog must cover the burst: the host issues its I/O connects back to
    // back, and a connection that arrives with the backlog full is refused by the
    // peer before this target's accept loop ever sees it.
    listener->Listen((int)kMaxIoQueues + 2);

    // A READY LINE, FOR THE HARNESS TO WAIT ON.
    //
    // Everything above this point is printed BEFORE the listener starts accepting, so a
    // script that sleeps a fixed two seconds and then connects is racing the provider's
    // initialisation - and it loses that race on a cold start (right after a reboot, or
    // after a NIC restart, which is what this project's own measurement sessions do).
    // The failure it produces is an INITIATOR-side ND_CONNECTION_REFUSED, i.e. "connect
    // failed", which reads like the target never started and sends the reader after the
    // wrong bug.  Measured while chasing exactly that: the same command pair connected
    // every time with a four-second gap and failed every time with two.
    //
    // run_f5.ps1 now waits for this line instead of sleeping.  Keep it on one line and
    // keep the wording: the script matches "listener armed".
    {
        char ipStr[INET_ADDRSTRLEN] = {};
        InetNtopA(AF_INET, &local.sin_addr, ipStr, sizeof(ipStr));
        printf("[listener armed] %s:%u is accepting connections\n", ipStr, ntohs(local.sin_port));
        fflush(stdout);
    }

    // =========================================================================
    //  One iteration of this loop is ONE CONTROLLER.
    //
    //  A real host talks to a discovery controller and to an I/O controller as two
    //  separate controllers, each with its own admin queue pair: `nvme discover`
    //  connects, reads the log page and goes away, and only then does `nvme connect`
    //  arrive.  Serving exactly one connection and exiting (which is what this
    //  target used to do) makes that sequence impossible - the discovery connection
    //  would consume the only admin queue pair.
    //
    //  maxControllers defaults to 1 so every existing test keeps the behaviour it
    //  asserts (F7 case C specifically checks that the target EXITS when its host
    //  disappears).  -serve N allows N controllers, -serve 0 means "keep serving
    //  until idle".
    // =========================================================================
    TargetTotals tot;
    int ctrlIndex = 0;
    for (;;) {
        if (maxControllers != 0 && ctrlIndex >= maxControllers) break;
        ctrlIndex++;

        Queue admin;
        if (!admin.create(&dev, 0)) return 1;
        // One queue pair per I/O queue the host may create.  They are created per
        // controller because a connector is consumed by accepting one connection:
        // the second controller needs fresh ones.
        Queue io[kMaxIoQueues];
        for (uint16_t i = 0; i < kMaxIoQueues; i++) {
            if (!io[i].create(&dev, (uint16_t)(i + 1))) return 1;
        }

        hr = listener->GetConnectionRequest(admin.conn, &admin.ov);
        if (hr == ND_PENDING) hr = admin.waitOverlapped(listener, (DWORD)g_reconnectWaitMs);
        if (!ndOk(hr)) {
            if (ctrlIndex == 1) { printf("admin connection request failed\n"); return 1; }
            // Not an error for a later controller: the host simply had nothing more
            // to do, which is exactly what `nvme discover` followed by `nvme connect`
            // looks like from here.
            //
            // How long to wait is a parameter and not a constant, because the two
            // cases this covers are genuinely different: `nvme discover` followed by
            // `nvme connect` arrives within a second, while a CONTROLLER RESET
            // (`nvme reset`) tears the controller down and reconnects on the kernel's
            // own schedule, which is not bounded by 20 s.  With the default, a reset
            // makes this target exit just before the host comes back, and the host
            // then sits in `nvme reset` until its own Ctrl Loss Timeout (600 s) - the
            // admin battery spent ten minutes there before this parameter existed.
            printf("  no further connection within %u s; %d controller(s) served\n",
                   (unsigned)(g_reconnectWaitMs / 1000), ctrlIndex - 1);
            break;
        }

        // The private data is validated exactly as nvmet validates it, so this target
        // cannot be more forgiving than the world and hide a host-side bug.
        {
            uint8_t pdbuf[512] = {};
            nvmeof_rdma_request_pd req = {};
            ULONG pdLen = sizeof(pdbuf);
            hr = admin.conn->GetPrivateData(pdbuf, &pdLen);
            memcpy(&req, pdbuf, sizeof(req));
            uint16_t qid = 0;
            uint16_t pdErr = nvmeof_rdma_check_req(&req, pdLen, &qid);
            printf("  [conn] controller %d private data: hr=0x%08X len=%u recfmt=%u qid=%u "
                   "hrqsize=%u hsqsize=%u\n",
                   ctrlIndex, (unsigned)hr, pdLen, req.recfmt, req.qid, req.hrqsize, req.hsqsize);
            if (FAILED(hr) || pdErr != 0) {
                nvmeof_rdma_reject_pd rej = {};
                rej.recfmt = 0;
                rej.sts = pdErr ? pdErr : NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH;
                printf("  [conn] rejecting with nvme_rdma status %u\n", rej.sts);
                admin.conn->Reject(&rej, sizeof(rej));
                admin.destroy();
                for (uint16_t i = 0; i < kMaxIoQueues; i++) io[i].destroy();
                return 1;
            }
            // Arm the whole admin receive ring before Accept.  Every capsule needs a
            // Receive to land in, and a host is allowed to send the next admin
            // command the moment it sees the previous answer.
            for (size_t i = 0; i < kCapSlots; i++) {
                if (!admin.postReceive(dev.region(kAdminCapOff + i * 64), 64,
                                       (void*)(CTX_ADMIN_CAP_BASE + i))) return 1;
            }
            printf("  [conn] %zu admin capsule receives armed\n", kCapSlots);
            nvmeof_rdma_accept_pd acc = {};
            acc.recfmt = 0; acc.crqsize = kSqDepth;
            hr = admin.conn->Accept(admin.qp, kReadLimit, kReadLimit, &acc, sizeof(acc), &admin.ov);
            if (hr == ND_PENDING) hr = admin.waitOverlapped(admin.conn, kWaitMs);
            if (!ndOk(hr)) {
                printf("admin Accept failed\n");
                admin.destroy();
                for (uint16_t i = 0; i < kMaxIoQueues; i++) io[i].destroy();
                return 1;
            }
            printf("  [conn] accepted; advertised crqsize=%u\n", acc.crqsize);
            // An RDMA Read is only legal if the connection negotiated at least one
            // outstanding read for us: the responder side promises that many.  Print
            // what was actually agreed - asking for 16 and getting 0 is silent, and
            // then every Read we post is rejected by the peer's responder.
            {
                ULONG inL = 0, outL = 0;
                if (SUCCEEDED(admin.conn->GetReadLimits(&inL, &outL)))
                    printf("  [conn] negotiated read limits: inbound=%u (we serve) outbound=%u (we may issue)\n",
                           (unsigned)inL, (unsigned)outL);
                else
                    printf("  [conn] GetReadLimits failed\n");
            }
        }

    F5State st;
    st.port = port;
    st.ip = ip;
    st.ctrlIndex = ctrlIndex - 1;
    bool adminDead = false;
    bool idleExit = false;
    // One pending-accept / connected / dead flag per I/O queue pair.  Each queue has
    // its own connector, so a host that loses one queue keeps the others working -
    // which is the whole point of a multi-queue controller: a real host with eight
    // queues keeps issuing I/O on the seven that are still alive.
    bool ioPending[kMaxIoQueues]   = {};
    bool ioConnected[kMaxIoQueues] = {};
    bool ioDead[kMaxIoQueues]      = {};
    int  ioNextToAccept = 0;
    ULONGLONG lastActivity = GetTickCount64();

    for (;;) {
        // ---- accept the next I/O queue the host asked for --------------------
        // One outstanding GetConnectionRequest at a time, always on the queue whose
        // turn it is: the listener's backlog (see Listen) holds the rest of the
        // burst, and serialising the accepts keeps "which connector owns this
        // request" unambiguous.  With several requests in flight at once there is
        // no way to tell which queue id an arriving connection meant.
        if (st.ioQueuesGranted > 0 &&
            ioNextToAccept < (int)kMaxIoQueues &&
            ioNextToAccept < st.ioQueuesGranted &&
            !ioPending[ioNextToAccept] && !ioConnected[ioNextToAccept] && !ioDead[ioNextToAccept]) {
            // Note: no re-arm blast here.  The admin ring is armed once and each
            // slot re-arms itself, so a Receive cancelled by the connection request
            // is reported as a completion for THAT slot and recovered there.
            Queue& nq = io[ioNextToAccept];
            HRESULT h2 = listener->GetConnectionRequest(nq.conn, &nq.ov);
            ioPending[ioNextToAccept] = (h2 == ND_PENDING) || SUCCEEDED(h2);
        }
        for (int qi = 0; qi < (int)kMaxIoQueues; qi++) {
            if (!ioPending[qi]) continue;
            Queue& nq = io[qi];
            HRESULT h2 = nq.pollOverlapped(nq.conn);
            if (h2 == ND_PENDING) continue;
            ioPending[qi] = false;
            if (ndOk(h2)) {
                // Validate the private data exactly as nvmet does, including the
                // controller id: an I/O queue's CM request carries it (there is no
                // cntlid in the Connect command), and a queue that names a
                // controller this target never created belongs to somebody else.
                uint8_t pdbuf[512] = {};
                nvmeof_rdma_request_pd req = {};
                ULONG pdLen = sizeof(pdbuf);
                HRESULT hpd = nq.conn->GetPrivateData(pdbuf, &pdLen);
                memcpy(&req, pdbuf, sizeof(req));
                uint16_t pdQid = 0;
                uint16_t pdErr = nvmeof_rdma_check_req(&req, pdLen, &pdQid);
                printf("  [conn] I/O %d private data: len=%u recfmt=%u qid=%u cntlid=%u\n",
                       qi + 1, (unsigned)pdLen, req.recfmt, req.qid, req.cntlid);
                if (FAILED(hpd) || pdErr != 0 || req.cntlid != st.cntlid) {
                    nvmeof_rdma_reject_pd rej = {};
                    rej.recfmt = 0;
                    // The enum values are ints; the field is 16-bit on the wire, so
                    // say so here rather than letting /W4 warn about a narrowing that
                    // is actually the protocol's own width.
                    rej.sts = (uint16_t)(pdErr ? pdErr
                             : (req.cntlid != st.cntlid) ? NVMEOF_RDMA_ERROR_INVALID_CNTLID
                                                         : NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH);
                    printf("  [conn] rejecting I/O queue %d with nvme_rdma status %u\n", qi + 1, rej.sts);
                    nq.conn->Reject(&rej, sizeof(rej));
                    ioDead[qi] = true;
                    if (qi == ioNextToAccept) ioNextToAccept++;
                    continue;
                }
                // The whole I/O receive ring for THIS queue, before Accept: a host
                // with a queue depth of 32 pipelines by design, and a capsule that
                // arrives with no Receive posted is lost, not retried.
                for (size_t i = 0; i < kCapSlots; i++) {
                    if (!nq.postReceive(dev.region(kIoCapOff + (size_t)qi * kIoCapStride + i * 64),
                                        64, CTX_IO_CAP(qi, i))) break;
                }
                // The I/O accept carries the SAME reply private data as the admin
                // one.  Accepting with none (nullptr, 0) is what this target used to
                // do, and the Linux host dropped the connection immediately
                // afterwards - it reads the reply's nvme_rdma_cm_rep (recfmt +
                // crqsize) for every queue it creates, not just the admin queue.
                nvmeof_rdma_accept_pd ioAcc = {};
                ioAcc.recfmt = 0; ioAcc.crqsize = kSqDepth;
                h2 = nq.conn->Accept(nq.qp, kReadLimit, kReadLimit, &ioAcc, sizeof(ioAcc), &nq.ov);
                if (h2 == ND_PENDING) h2 = nq.waitOverlapped(nq.conn, kWaitMs);
            }
            if (ndOk(h2)) {
                ioConnected[qi] = true;
                st.ioQueuesConnected++;
                if (qi == ioNextToAccept) ioNextToAccept++;
                printf("  [conn] I/O queue %d connected (%d of %d granted)\n",
                       qi + 1, st.ioQueuesConnected, st.ioQueuesGranted);
            } else {
                char b[64];
                printf("  [conn] I/O queue %d accept failed: %s\n", qi + 1, ndStr(h2, b, sizeof(b)));
                ioDead[qi] = true;
                if (qi == ioNextToAccept) ioNextToAccept++;
            }
        }

        bool didWork = false;
        // Capsules whose completions arrived while a data transfer was in flight.
        // They are processed here, one per loop turn, so that handling one may
        // queue another without recursing.
        if (st.pendingCount > 0) {
            int slot = st.pending[0];
            for (int i = 1; i < st.pendingCount; i++) st.pending[i - 1] = st.pending[i];
            st.pendingCount--;
            didWork = true;
            if (!adminDead && !targetAdmin(admin, dev, st, slot)) break;
            // No break on `disabledByHost`: see the CC.EN=0 note in targetAdmin.  A
            // target that stops answering here turns every controller reset into a
            // 60-second host stall, and a `nvme disconnect` into the same.
            if (st.disabledByHost && !st.disableNoted) {
                st.disableNoted = true;
                printf("  [admin] controller disabled; answering until the host's queue pairs go away\n");
            }
        }
        bool stop = false;
        for (int qi = 0; qi < (int)kMaxIoQueues && !stop; qi++) {
            if (!ioConnected[qi] || ioDead[qi]) continue;
            ND2_RESULT r = {};
            if (!io[qi].poll(&r)) continue;
            uintptr_t ictx    = (uintptr_t)r.RequestContext;
            uintptr_t capBase = CTX_IO_CAP_BASE + (uintptr_t)qi * kCapSlots;
            uintptr_t datBase = CTX_DATA_BASE   + (uintptr_t)qi * kCapSlots;
            if (ictx >= capBase && ictx < capBase + kCapSlots) {
                int slot = (int)(ictx - capBase);
                if (ndOk(r.Status) && r.BytesTransferred == 64) {
                    didWork = true;
                    if (!targetIo(io[qi], dev, st, qi, slot)) stop = true;
                } else if (r.Status == ND_CANCELED) {
                    printf("  [io] q%d slot %d: ND_CANCELED; that queue pair is dead\n", qi + 1, slot);
                    ioDead[qi] = true;
                } else {
                    printf("  [io] q%d slot %d did not deliver a capsule (st=0x%08X); re-arming it\n",
                           qi + 1, slot, (unsigned)r.Status);
                    io[qi].postReceive(dev.region(kIoCapOff + (size_t)qi * kIoCapStride +
                                                  (size_t)slot * 64),
                                       64, CTX_IO_CAP(qi, slot));
                }
            } else if (ictx >= datBase && ictx < datBase + kCapSlots) {
                int slot = (int)(ictx - datBase);
                if (st.io[qi][slot].used) {
                    didWork = true;
                    if (!targetIoComplete(io[qi], dev, st, qi, slot, r.Status)) stop = true;
                }
            } else {
                didWork = true;   // a completion for a response send
            }
        }
        if (stop) break;
        if (!adminDead) {
            ND2_RESULT r = {};
            if (admin.poll(&r)) {
                uintptr_t actx = (uintptr_t)r.RequestContext;
                if (actx >= CTX_ADMIN_CAP_BASE && actx < CTX_ADMIN_CAP_BASE + kCapSlots) {
                    int slot = (int)(actx - CTX_ADMIN_CAP_BASE);
                    if (ndOk(r.Status) && r.BytesTransferred == 64) {
                        didWork = true;
                        if (!targetAdmin(admin, dev, st, slot)) break;
                        // No break on `disabledByHost` - see the CC.EN=0 note above.
                        if (st.disabledByHost && !st.disableNoted) {
                            st.disableNoted = true;
                            printf("  [admin] controller disabled; answering until the host's queue pairs go away\n");
                        }
                    } else {
                        // A slot whose Receive came back empty.  ND_CANCELED means
                        // the queue pair is finished; anything else means this one
                        // slot posted nothing, so re-arm it and carry on - the
                        // other slots are unaffected, which is the point of a ring.
                        if (r.Status == ND_CANCELED) {
                            printf("  [admin] slot %d: ND_CANCELED; queue pair declared dead\n", slot);
                            adminDead = true;
                        } else {
                            printf("  [admin] slot %d did not deliver a capsule (st=0x%08X); re-arming it\n",
                                   slot, (unsigned)r.Status);
                            admin.postReceive(dev.region(kAdminCapOff + (size_t)slot * 64), 64,
                                              (void*)(CTX_ADMIN_CAP_BASE + (uintptr_t)slot));
                        }
                    }
                } else {
                    didWork = true;   // a completion for something else (a response send)
                }
            }
        }
        // "The host has gone" = the admin queue is dead AND no I/O queue is still
        // alive.  With several queue pairs, one dead queue is not the host leaving:
        // a real host tears its queues down one at a time.
        bool anyIoAlive = false;
        for (int qi = 0; qi < (int)kMaxIoQueues; qi++) {
            if (ioConnected[qi] && !ioDead[qi]) anyIoAlive = true;
        }
        if (adminDead && !anyIoAlive) {
            printf("  all queue pairs returned ND_CANCELED; the host has gone\n");
            break;
        }
        if (didWork) { lastActivity = GetTickCount64(); continue; }
        if (GetTickCount64() - lastActivity > 120000) {
            printf("  idle for 120 s; stopping\n");
            idleExit = true;
            break;
        }
        SwitchToThread();
    }

    // ---- the end of one controller: report it, then decide whether to serve more
    printf("\ncontroller %d summary: connects=%d discovery=%d ioQueuesGranted=%d "
           "ioQueuesConnected=%d ioCommands=%d written=%llu read=%llu discLogReads=%d "
           "zeroes=%d dsm=%d zeroed=%llu unknownAdmin=%d unknownIo=%d\n",
           ctrlIndex, st.connects, st.discovery ? 1 : 0, st.ioQueuesGranted,
           st.ioQueuesConnected, st.ioCommands,
           (unsigned long long)st.bytesWritten, (unsigned long long)st.bytesRead,
           st.discLogReads, st.zeroesCommands, st.dsmCommands,
           (unsigned long long)st.bytesZeroed, st.unknownAdmin, st.unknownIo);
    // Per-queue command counts: "eight queues connected" is also true of a host that
    // connects eight and sends everything down the first, so the distribution is the
    // evidence that multi-queue is actually being used.
    if (st.ioQueuesConnected > 0) {
        printf("target io per queue:");
        for (uint16_t q = 0; q < kMaxIoQueues; q++) printf(" q%u=%d", (unsigned)(q + 1), st.ioPerQueue[q]);
        printf("\n");
    }
    tot.add(st);
    admin.destroy();
    for (uint16_t q = 0; q < kMaxIoQueues; q++) io[q].destroy();
    if (idleExit) break;
    }   // end of one controller

    // ---- totals across every controller this run served ---------------------
    // One aggregate line so a reader (or a script) that looks for `target summary:`
    // sees the whole run, including a discovery controller followed by an I/O one.
    printf("\ntarget summary: controllers=%d connects=%d discLogReads=%d "
           "ioQueuesGranted=%d ioQueuesConnected=%d ioCommands=%d flushes=%d "
           "written=%llu read=%llu invalidatesServed=%d keepAlives=%d katoMs=%d "
           "deferredAers=%d unknownAdmin=%d unknownIo=%d "
           "zeroesCommands=%d dsmCommands=%d bytesZeroed=%llu "
           "authSends=%d authReceives=%d authRefused=%d authAborted=%d authFatal=%d\n",
           ctrlIndex, tot.connects, tot.discLogReads, tot.ioQueuesGranted,
           tot.ioQueuesConnected, tot.ioCommands, tot.flushes,
           (unsigned long long)tot.bytesWritten, (unsigned long long)tot.bytesRead,
           tot.invalidates, tot.keepAlives, tot.katoMs, tot.deferredAers,
           tot.unknownAdmin, tot.unknownIo,
           tot.zeroesCommands, tot.dsmCommands, (unsigned long long)tot.bytesZeroed,
           tot.authSends, tot.authReceives, tot.authRefused, tot.authAborted,
           tot.authFatal);
    listener->Release();
    dev.close(nullptr, nullptr, 0);
    printf("\ntarget failures: %d\n", g_failures);
    return g_failures;
}

// ---------------------------------------------------------------------------
//  Windows service mode (-service).
//
//  Why it is here: the bridge is the one piece of this project that is meant to
//  be left running on a machine - Windows' iSCSI initiator connects to it and
//  the disk appears.  A console program that must be started by hand after every
//  reboot is not deployable, and "run it as a scheduled task" is a way to have a
//  service without the stop signal, the failure recovery or the event log.
//
//  The SCM gives no arguments (they live in the service's ImagePath) and calls
//  ServiceMain on its own thread, so the payload has to run INSIDE the service
//  thread rather than before StartServiceCtrlDispatcher returns.  The command
//  line is parsed as usual and stashed here for that thread to use, which keeps
//  one argument parser instead of two.
//
//  Only the bridge (-iscsi) can be stopped gracefully: the handler closes its
//  listening socket, accept() fails and run() returns.  A -target service is
//  accepted too but stops only when the process is killed, and install.ps1 does
//  not create one.
// ---------------------------------------------------------------------------
//  The state these functions use (g_serviceMode, g_svcBridge, ...) lives at the
//  top of the file, because the bridge run path publishes g_svcBridge long
//  before this point.

static DWORD WINAPI svcCtrl(DWORD ctrl, DWORD, LPVOID, LPVOID) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_ss.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_ssh, &g_ss);
        // Both, because they cover different states: the flag stops a retry wait (no
        // bridge exists yet), and stop() ends the accept loop (bridge is up).
        InterlockedExchange(&g_svcStopRequested, 1);
        if (g_svcBridge) g_svcBridge->stop();
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_ssh, &g_ss);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static int dispatchPayload(int argc, char** argv);   // the normal mode dispatch

static void WINAPI svcMain(DWORD, LPSTR*) {
    g_ssh = RegisterServiceCtrlHandlerExA(g_svcName.c_str(), svcCtrl, nullptr);
    if (!g_ssh) return;
    // A service has no console: an unredirected printf goes nowhere at all, which is
    // the worst possible failure mode for a data path.  Redirect here - i.e. only
    // once the SCM has connected, so the console path keeps its console.
    //
    // _fsopen with _SH_DENYNO, not freopen: the CRT's default sharing makes the live
    // log unreadable ("the process cannot access the file because it is being used by
    // another process"), and a log that cannot be tailed while the service runs is
    // exactly the log you need when it misbehaves.
    if (!g_svcLog.empty()) {
        // Three layers, because a console-less service is hostile to stdio and each
        // attempt alone failed in a different way:
        //   freopen_s   -> output works, but the file cannot be read while the
        //                  service runs ("used by another process"), so the live log
        //                  is useless exactly when it is needed
        //   _fsopen + *stdout = *f -> file created, 0 bytes, every printf vanished
        //                  (_fileno(stdout) is -1 without a console)
        //   _dup2       -> same silence
        // So: a shared Win32 handle for the SCM's std handles, the FILE object copied,
        // and fd 1 pointed at it.  FILE_SHARE_READ keeps `Get-Content -Wait` working.
        HANDLE h = CreateFileA(g_svcLog.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SetStdHandle(STD_OUTPUT_HANDLE, h);
            SetStdHandle(STD_ERROR_HANDLE, h);
            int fd = _open_osfhandle((intptr_t)h, _O_APPEND | _O_TEXT);
            if (fd >= 0) _dup2(fd, 1);
        }
        FILE* f = _fsopen(g_svcLog.c_str(), "a", _SH_DENYNO);
        if (f) {
            *stdout = *f;
            setvbuf(stdout, nullptr, _IONBF, 0);
        }
        FILE* g = _fsopen(g_svcLog.c_str(), "a", _SH_DENYNO);
        if (g) {
            *stderr = *g;
            setvbuf(stderr, nullptr, _IONBF, 0);
        }
    }
    g_ss.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ss.dwCurrentState     = SERVICE_RUNNING;
    SetServiceStatus(g_ssh, &g_ss);
    printf("[service] %s started (pid %lu)\n", g_svcName.c_str(), GetCurrentProcessId());
    g_svcRc = dispatchPayload(g_svcArgc, g_svcArgv);
    printf("[service] %s exiting (rc %d)\n", g_svcName.c_str(), g_svcRc);
    g_ss.dwCurrentState     = SERVICE_STOPPED;
    g_ss.dwWin32ExitCode    = (DWORD)g_svcRc;
    SetServiceStatus(g_ssh, &g_ss);
}

int main(int argc, char** argv) {
    // Pre-scan for the service log BEFORE anything touches stdout.
    //
    // This ordering is the whole fix.  MSVC's stdio latches the OS handle for a
    // stream the first time it is configured or used, and `setvbuf` below does
    // exactly that.  For a service started by the SCM there is no console, so the
    // latched handle is invalid, and three later attempts to redirect (freopen,
    // _fsopen + *stdout = *f, CreateFile + SetStdHandle + _dup2) all produced a
    // created-but-empty file.  Redirect first, configure after.
    for (int i = 1; i + 1 < argc; i++) {
        if      (strcmp(argv[i], "-service") == 0) g_serviceMode = true;
        else if (strcmp(argv[i], "-svcname") == 0) g_svcName = argv[i + 1];
        else if (strcmp(argv[i], "-log")     == 0) g_svcLog  = argv[i + 1];
    }
    bool logFailed = false;
    if (g_serviceMode && !g_svcLog.empty()) {
        // freopen_s, and the ordering with setvbuf below is the whole fix: MSVC's
        // stdio latches the OS handle for a stream the first time it is configured,
        // and for a service started by the SCM there is no console, so redirecting
        // afterwards leaves a created-but-empty file.  Measured alternatives that do
        // NOT work in the UCRT, so nobody has to try them again: _fsopen followed by
        // `*stdout = *f` (file created, 0 bytes) and CreateFile + SetStdHandle +
        // _open_osfhandle + _dup2 (same).  freopen rebinds the stream properly.
        FILE* f = nullptr;
        if (freopen_s(&f, g_svcLog.c_str(), "a", stdout) != 0) logFailed = true;
        FILE* g = nullptr;
        if (freopen_s(&g, g_svcLog.c_str(), "a", stderr) != 0) logFailed = true;
    }
    // Unbuffered stdout, and this is not cosmetic.  When the CRT sees a redirected
    // stdout it switches to 4 KiB block buffering, so the newest diagnostics sit in
    // memory - and a long-running server is killed rather than exited, which throws
    // them away.  That cost a full debugging round here: the tail of the iSCSI log
    // read as "the initiator went silent after our security-stage Login Response"
    // when in truth the log was simply missing its last few kilobytes, and the real
    // last PDU had long since arrived.  A log that can lie about the LAST event is
    // worse than no log.
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (g_serviceMode && !g_svcLog.empty()) {
        printf("[service] log: %s%s\n", g_svcLog.c_str(), logFailed ? " (open FAILED)" : "");
    }
    // Primitives first, before anything else needs a command line: a wrong hash or a
    // byte-reversed DH secret makes every later failure look like a protocol bug.
    if (argc >= 2 && strcmp(argv[1], "-authselftest") == 0) {
        printf("=== DH-HMAC-CHAP primitives, against published vectors\n");
        int f = nvmeof_auth::selfTest();
        printf("=== DH-HMAC-CHAP protocol pieces (target half)\n");
        nvmeof_dhchap::selfTest(f);
        return f;
    }
    // -genkey [sha256|sha384|sha512]: print a fresh DHHC-1 key.
    //
    // The SAME string has to be configured on both ends for an interop run, and the
    // format is fiddly enough (base64 + a CRC32 that nvmet verifies) that generating
    // it by hand is a good way to spend an hour on "the target rejected my key".
    // nvme-cli's `nvme gen-dhchap-key` produces exactly this shape.
    if (argc >= 2 && strcmp(argv[1], "-genkey") == 0) {
        int hh = nvmeof_auth::HASH_SHA256;
        if (argc >= 3) {
            const char* h = argv[2];
            if      (strcmp(h, "sha384") == 0) hh = nvmeof_auth::HASH_SHA384;
            else if (strcmp(h, "sha512") == 0) hh = nvmeof_auth::HASH_SHA512;
            else if (strcmp(h, "sha256") != 0) {
                printf("unknown hash '%s' (sha256, sha384, sha512)\n", h);
                return 2;
            }
        }
        uint8_t secret[64] = {};
        size_t  slen = (size_t)nvmeof_auth::hashLen((uint8_t)hh);
        uint8_t raw[68] = {};
        char b64[192] = {};
        DWORD n = sizeof(b64);
        if (!slen || !NT_SUCCESS(BCryptGenRandom(nullptr, secret, (ULONG)slen,
                                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            printf("key generation failed\n");
            return 1;
        }
        memcpy(raw, secret, slen);
        nvmeof_dhchap::putLe32(raw + slen, nvmeof_dhchap::crc32Ieee(secret, slen));
        if (!CryptBinaryToStringA(raw, (DWORD)(slen + 4),
                                  CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &n)) {
            printf("base64 failed\n");
            return 1;
        }
        const char* names[] = { "", "sha256", "sha384", "sha512" };
        printf("DHHC-1:%02X:%s:\n", (unsigned)hh, b64);
        // Print the re-parsed key too: a generator whose output its own parser
        // rejects is a trap for the next person, and the CRC makes that possible.
        nvmeof_dhchap::Key back;
        char text[192];
        sprintf_s(text, "DHHC-1:%02X:%s:", (unsigned)hh, b64);
        if (!nvmeof_dhchap::parseKey(text, back)) {
            printf("INTERNAL: the key just generated does not parse\n");
            return 1;
        }
        fprintf(stderr, "  (%s, %zu-byte secret, CRC verified by re-parsing: ok)\n",
                names[hh], back.secretLen);
        return 0;
    }
    if (argc < 4) {
        printf("usage:\n"
               "  %s -initiator <serverIp> <port> <localIp> [-subnqn <nqn>] [-hostnqn <nqn>]\n"
               "               [-queues <n>] [-blocks <n>] [-discover]\n"
               "               [-authkey <DHHC-1:..>] [-authctrlkey <DHHC-1:..>] [-authskip]\n"
               "               [-iscsi <port> [-iscsiaddr <ip>] [-iscsirw] [-iscsitrace] [-iscsitime] [-iscsimbl <bytes>] [-iscsir2tcfg <file>]]\n"
               "               [-backendretry <s>] [-nvmetiming]\n"
               "  %s -target    <ip> <port> [-serve <n>] [-authkey <DHHC-1:..>]\n"
               "                          [-authdhgroup 2048|3072|4096] [-reconnectwait <s>] [-iotrace]\n"
               "                          (n = 1 by default; 0 = keep serving)\n"
               "\n"
               "  %s -genkey    [sha256|sha384|sha512]     print a fresh DHHC-1 key\n"
               "  %s -authselftest                         DH-HMAC-CHAP self-test\n"
               "\n"
               "  The default subnqn is this project's own target.  For Linux use the\n"
               "  subsystem NQN that linux/nvmet_setup.sh created, e.g.\n"
               "    -subnqn nqn.2024-01.local.rdma:linux-nvmet\n"
               "\n"
               "  -authkey makes the target REQUIRE DH-HMAC-CHAP (ATR in the Connect\n"
               "  result) and refuse every non-fabrics command until the host\n"
               "  authenticates.  The key is the same string `nvme gen-dhchap-key`\n"
               "  prints and nvmet's dhchap_key attribute takes:\n"
               "    nvme connect -a <ip> -t rdma -n <subnqn> -S <key>\n"
               "\n"
               "  -nvmetiming splits one serial NVMe-oF round trip into its hops\n"
               "  (capsule sent, target saw it, reply sent, reply seen), on the\n"
               "  machine-wide QPC clock so two processes' stamps can be subtracted.\n"
               "  It is how DESIGN 8.67 found that the data moves in 31 us while the\n"
               "  completion takes 12-22 ms.\n"
               "\n"
               "  -reconnectwait is how long a target keeps listening for the NEXT\n"
               "  controller after one goes away.  Left at 20 s the self-tests behave\n"
               "  as they always have; a controller reset (`nvme reset`) reconnects on\n"
               "  the kernel's own schedule, so the interop battery raises it.\n",
               argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // These are REFERENCES into g_pl, not locals: the service thread runs the same
    // dispatch and has to see the same parsed values (see the Payload struct).
    const char*& subnqn = g_pl.subnqn;
    const char*& hostnqn = g_pl.hostnqn;
    int& wantQueues = g_pl.wantQueues;
    uint32_t& blocks = g_pl.blocks;
    bool& discover = g_pl.discover;
    // How many controllers a target run serves.  1 keeps every existing test's
    // behaviour - including F7's "the target exits when its host disappears" - while
    // a real host needs more: `nvme discover` and `nvme connect` are two separate
    // controllers, and the discovery one leaves before the I/O one arrives.
    int& serveControllers = g_pl.serveControllers;
    const char*& initiatorAuthKey = g_pl.initiatorAuthKey;
    const char*& initiatorCtrlKey = g_pl.initiatorCtrlKey;
    bool& authSkip = g_pl.authSkip;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-subnqn") == 0 && i + 1 < argc)  subnqn = argv[++i];
        else if (strcmp(argv[i], "-hostnqn") == 0 && i + 1 < argc) hostnqn = argv[++i];
        else if (strcmp(argv[i], "-queues") == 0 && i + 1 < argc)  wantQueues = atoi(argv[++i]);
        else if (strcmp(argv[i], "-blocks") == 0 && i + 1 < argc)  blocks = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-serve") == 0 && i + 1 < argc)   serveControllers = atoi(argv[++i]);
        else if (strcmp(argv[i], "-discover") == 0)                discover = true;
        // -authkey is the HOST key in both roles: the target requires it, the
        // initiator presents it.  One option, because it is one value - two
        // spellings would let a run configure the target with a key the host was
        // never told, and the symptom (Failure1 with rescode 0x01) says nothing
        // about which side is wrong.
        else if (strcmp(argv[i], "-authkey") == 0 && i + 1 < argc) {
            g_targetAuthKey = argv[++i];
            initiatorAuthKey = g_targetAuthKey;
        }
        // The controller key, for bidirectional authentication: the target holds it
        // (and must prove it) and the host verifies it.
        else if (strcmp(argv[i], "-authctrlkey") == 0 && i + 1 < argc) {
            g_targetCtrlKey = argv[++i];
            initiatorCtrlKey = g_targetCtrlKey;
        }
        else if (strcmp(argv[i], "-authskip") == 0)                authSkip = true;
        // -target only: back the namespace with a FILE instead of a pattern in memory.
        // The file is created at the namespace size if it does not exist, and FLUSH
        // writes it back, so a Linux host can mkfs/mount/keep things on it.
        else if (strcmp(argv[i], "-nsfile") == 0 && i + 1 < argc) {
            g_nsFile = argv[++i];
        }
        // -initiator only: move the WHOLE namespace to/from a local file, over
        // NVMe-oF/RDMA.  This is what a volume demo is built on (see
        // mount_nvmeof.ps1): fetch the namespace, mount it in Windows, write files
        // into it, push it back.
        else if (strcmp(argv[i], "-dump") == 0 && i + 1 < argc) {
            g_dumpFile = argv[++i];
        }
        else if (strcmp(argv[i], "-push") == 0 && i + 1 < argc) {
            g_pushFile = argv[++i];
        }
        // -initiator only: describe the namespace without writing anything to it.
        else if (strcmp(argv[i], "-survey") == 0) {
            g_survey = true;
        }
        // -initiator only: serve Windows' iSCSI initiator on <port>.  The namespace
        // becomes a live Windows disk with no local copy of it anywhere.
        else if (strcmp(argv[i], "-iscsi") == 0 && i + 1 < argc) {
            g_iscsiPort = atoi(argv[++i]);
            if (g_iscsiPort < 1 || g_iscsiPort > 65535) {
                printf("-iscsi <tcp-port>: 1..65535\n");
                return 2;
            }
        }
        // Opt IN to writes: the default is read-only, because the namespace behind
        // this bridge may be somebody's real disk and nvmet cannot mark it read-only.
        else if (strcmp(argv[i], "-iscsitrace") == 0) {
            g_iscsiTrace = true;
        }
        // Aggregate per-command data-path timing (total / NVMe / Data-In send).
        // One line per SCSI READ, so it can stay on during a throughput run.
        else if (strcmp(argv[i], "-iscsitime") == 0) {
            g_iscsiTime = true;
        }
        // Split one serial NVMe-oF round trip into "capsule sent" and "target
        // answered".  Needed because the serial path measured ~10 ms per command
        // while the same fabric's 64 KiB read latency is 28 us; the total says
        // nothing about which half is at fault.
        else if (strcmp(argv[i], "-nvmetiming") == 0) {
            g_nvmeTiming = true;
        }
        // One line per I/O command on the target.  Off by default because it costs
        // 52x on a serial workload (DESIGN 8.68) - turn it on when the question is
        // "which LBA did the host ask for", not when it is "how fast is this".
        else if (strcmp(argv[i], "-iotrace") == 0) {
            g_ioTrace = true;
        }
        // MaxBurstLength to advertise.  Default 65536, which is what makes writes
        // work: Windows picks its CDB size from this, and a CDB larger than the
        // first burst forces an R2T it then rejects.  262144 makes Windows choose
        // 256 KiB CDBs - faster for a lone reader, fatal for writes over 64 KiB.
        // R2T A/B probe: a file the target re-reads before every R2T and at every
        // login.  Diagnostic only; see the comment on g_r2tProbePath.
        else if (strcmp(argv[i], "-iscsir2tcfg") == 0 && i + 1 < argc) {
            g_r2tProbePath = argv[++i];
            printf("  iscsir2tcfg: R2T probe file '%s'\n", g_r2tProbePath.c_str());
        }
        // Run as a Windows service (started by the SCM, see install.ps1).
        else if (strcmp(argv[i], "-service") == 0) {
            g_serviceMode = true;
        }
        // -backendretry <seconds>: keep retrying the NVMe-oF peer instead of exiting
        // (a service that flaps every 5 s when its peer is down is not deployable).
        else if (strcmp(argv[i], "-backendretry") == 0 && i + 1 < argc) {
            int s = atoi(argv[++i]);
            if (s < 1 || s > 3600) { printf("-backendretry %d: 1..3600 seconds\n", s); return 2; }
            g_backendRetryMs = (uint32_t)s * 1000;
        }
        else if (strcmp(argv[i], "-svcname") == 0 && i + 1 < argc) {
            g_svcName = argv[++i];
        }
        // Where a service's output goes.  A service has no console, so without this
        // every printf is silently discarded.
        else if (strcmp(argv[i], "-log") == 0 && i + 1 < argc) {
            g_svcLog = argv[++i];
        }        else if (strcmp(argv[i], "-iscsimbl") == 0 && i + 1 < argc) {
            long v = atol(argv[++i]);
            if (v >= 512 && v <= 16777215) g_iscsiMaxBurst = (uint32_t)v;
            else printf("  -iscsimbl: %ld is outside 512..16777215, ignored\n", v);
        }
        // MaxRecvDataSegmentLength this target advertises, i.e. the largest single
        // Data-In PDU we send AND the largest Data-Out PDU we accept.  Default 65536
        // (one page of iSCSI), and raising it is worth real throughput: measured through
        // Windows' initiator, a 4 MiB write costs ~16 ms PER DATA-OUT PDU, so at 64 KiB
        // per PDU a 4 MiB write takes a second.  The initiator's own
        // MaxRecvDataSegmentLength (registry, under the Microsoft iSCSI Initiator class
        // key) caps the read direction, so raise that too - and note that its parameters
        // only reload when the ROOT\ISCSIPRT device instance is re-enabled, not on a
        // service restart (DESIGN 8.68).
        else if (strcmp(argv[i], "-iscsichunk") == 0 && i + 1 < argc) {
            long v = atol(argv[++i]);
            // Clamped to the staging buffer, not just to the wire format.  The PDU
            // size is min(staging unit, this value), so a larger value cannot take
            // effect - and it silently did not for as long as the buffer was 256 KiB
            // while this knob accepted 1 MiB without complaint (DESIGN 8.70).
            if (v > (long)kIscsiBytes) {
                printf("  -iscsichunk: %ld exceeds the %llu-byte staging buffer; "
                       "using %llu\n", v, (unsigned long long)kIscsiBytes,
                       (unsigned long long)kIscsiBytes);
                v = (long)kIscsiBytes;
            }
            if (v >= 512 && v <= 16777215) g_iscsiChunk = (uint32_t)v;
            else printf("  -iscsichunk: %ld is outside 512..16777215, ignored\n", v);
        }
        else if (strcmp(argv[i], "-iscsirw") == 0) {
            g_iscsiReadWrite = true;
        }
        // -initiator -iscsi only: bind the iSCSI listener to <ip> instead of
        // 127.0.0.1, and advertise the same address in SendTargets.
        else if (strcmp(argv[i], "-iscsiaddr") == 0 && i + 1 < argc) {
            g_iscsiBind = argv[++i];
        }
        // -target only: how long to keep listening for the next controller.  A
        // controller reset reconnects on the kernel's schedule, which is longer than
        // the 20 s the self-tests use.
        else if (strcmp(argv[i], "-reconnectwait") == 0 && i + 1 < argc) {
            int s = atoi(argv[++i]);
            if (s < 1 || s > 3600) { printf("-reconnectwait %d: 1..3600 seconds\n", s); return 2; }
            g_reconnectWaitMs = (ULONGLONG)s * 1000;
        }
        else if (strcmp(argv[i], "-authdhgroup") == 0 && i + 1 < argc) {
            int g = atoi(argv[++i]);
            if (g == 2048)      g_targetDhGroup = nvmeof_auth::DHGROUP_2048;
            else if (g == 3072) g_targetDhGroup = nvmeof_auth::DHGROUP_3072;
            else if (g == 4096) g_targetDhGroup = nvmeof_auth::DHGROUP_4096;
            else { printf("-authdhgroup %d: only 2048, 3072 and 4096 exist\n", g); return 2; }
        }
    }
    // A target that is about to require authentication should say so before it
    // accepts anything, and should fail fast on a key no host could match.
    if (g_targetAuthKey) {
        nvmeof_dhchap::Key probe;
        if (!nvmeof_dhchap::parseKey(g_targetAuthKey, probe)) {
            printf("-authkey is not a usable DHHC-1 key; refusing to start\n");
            return 2;
        }
        printf("target DH-HMAC-CHAP: hash 0x%02X, secret %zu bytes, dhgroup %u\n",
               probe.hashId, probe.secretLen, (unsigned)g_targetDhGroup);
    }

    if (g_serviceMode) {
        g_svcArgc = argc;
        g_svcArgv = argv;
        SERVICE_TABLE_ENTRYA table[2] = {};
        table[0].lpServiceName = (LPSTR)g_svcName.c_str();
        table[0].lpServiceProc = svcMain;
        // NOT redirected yet: if this fails because somebody ran -service from a
        // console, the explanation has to reach that console.  The log redirect
        // happens in svcMain, i.e. only once the SCM has actually connected.
        if (!StartServiceCtrlDispatcherA(table)) {
            DWORD e = GetLastError();
            if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                printf("-service is meant to be started by the service control manager,\n"
                       "not from a console.  Use install.ps1, or run the same command\n"
                       "line without -service.\n");
            } else {
                printf("StartServiceCtrlDispatcher failed (error %lu)\n", e);
            }
            return 3;
        }
        return g_svcRc;
    }
    return dispatchPayload(argc, argv);
}

// The payload, factored out so the service thread and the console path run exactly
// the same code.  Anything that behaves differently as a service is a bug waiting
// for a reboot.
static int dispatchPayload(int argc, char** argv) {
    HRESULT hr = NdStartup();
    if (FAILED(hr)) { printf("NdStartup 0x%08X\n", (unsigned)hr); WSACleanup(); return 1; }

    int rc = 2;
    if (strcmp(argv[1], "-target") == 0)
        rc = runTarget(argv[2], (uint16_t)atoi(argv[3]), g_pl.serveControllers);
    else if (strcmp(argv[1], "-initiator") == 0 && argc >= 5) {
        for (;;) {
            rc = runInitiator(argv[2], (uint16_t)atoi(argv[3]), argv[4], g_pl.subnqn,
                              g_pl.hostnqn, g_pl.wantQueues, g_pl.blocks, g_pl.discover,
                              g_pl.initiatorAuthKey, g_pl.initiatorCtrlKey, g_pl.authSkip);
            if (rc == 0 || g_backendRetryMs == 0) break;
            if (InterlockedCompareExchange(&g_svcStopRequested, 0, 0)) {
                printf("  [backendretry] stop requested while the peer was unreachable; "
                       "not retrying\n");
                break;
            }
            printf("  [backendretry] backend unreachable (rc %d); retrying in %u s "
                   "(Ctrl+C or Stop-Service to give up)\n", rc, g_backendRetryMs / 1000);
            Sleep(g_backendRetryMs);
        }
    }
    else printf("unknown mode %s\n", argv[1]);

    NdCleanup();
    WSACleanup();
    return rc;
}

