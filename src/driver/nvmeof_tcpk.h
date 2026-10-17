/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * nvmeof_tcpk.h - the public face of the kernel NVMe/TCP transport client.
 *
 * Four functions, deliberately: open a connection, close it, run one command,
 * run an Identify.  Everything about PDUs, handshakes, buffers and IRPs lives in
 * nvmeof_tcpk.c and is not a caller's business.
 *
 * The caller owns WSK registration.  This file never calls WskRegister,
 * WskCaptureProviderNPI or WskReleaseProviderNPI: those belong to whoever owns
 * the driver's lifetime (src/driver/nvmeofk.c shows the pattern that is proven
 * on this machine), and the captured WSK_PROVIDER_NPI is handed to NvtcpOpen.
 *
 * Calling contract, in the order the traps appear:
 *
 *   - NvtcpOpen may be called at IRQL <= DISPATCH_LEVEL and blocks (with a
 *     timeout) until connected and handshaked, or returns the failure.  *conn is
 *     only touched on success.
 *   - NvtcpCommand / NvtcpIdentify: ONE command at a time per connection.  A
 *     second concurrent caller on the same connection gets STATUS_DEVICE_BUSY
 *     rather than corrupting the shared capsule buffer.  Data moves in-capsule
 *     only, so inBytes and outBytes are both capped at NVMEOF_TCPK_MAX_DATA
 *     (4096); a larger transfer is refused with STATUS_INVALID_BUFFER_SIZE.
 *     A command does not move data in both directions at once.
 *   - On any NTSTATUS failure the NVMe status in *status is meaningless
 *     (0xFF).  On success it is the completion's status code: 0 = success.
 *   - NvtcpClose is idempotent: a second call finds nothing and returns success.
 *     It refuses (STATUS_DEVICE_BUSY) while a command is in flight on that
 *     connection.  It never waits forever.
 *   - On unload, close every connection BEFORE releasing the provider NPI: a
 *     socket whose provider has gone is a socket that cannot be closed.
 *
 * A time out poisons a connection.  A poisoned connection is logged and leaked
 * on purpose, never freed: the WSK request that timed out still owns its IRP,
 * and wsk.h forbids closing a socket while a WSK call has not returned control.
 * NvtcpClose on a poisoned connection returns success and releases nothing.
 */

#ifndef NVMEOF_TCPK_H
#define NVMEOF_TCPK_H

#include <ntddk.h>

/* The provider dispatch tables embed an anonymous WSK_PROVIDER_BASIC_DISPATCH
 * member; /W4 calls that a nonstandard extension at its definition site. */
#pragma warning(push)
#pragma warning(disable: 4201)
#pragma warning(disable: 4324)
#include <wsk.h>
#pragma warning(pop)

/* In-capsule data ceiling: one command moves at most this many bytes. */
#define NVMEOF_TCPK_MAX_DATA        4096u

/* The NVMe opcodes and Identify CNS values a caller needs, so the miniport does
 * not hand-copy magic numbers.  Same values the user-mode stack uses. */
#define NVMEOF_TCPK_OPC_FLUSH       0x00u
#define NVMEOF_TCPK_OPC_WRITE       0x01u
#define NVMEOF_TCPK_OPC_READ        0x02u
#define NVMEOF_TCPK_OPC_IDENTIFY    0x06u

#define NVMEOF_TCPK_CNS_NAMESPACE   0x00u
#define NVMEOF_TCPK_CNS_CONTROLLER  0x01u

/* Open a TCP connection to ip:port and do the ICReq/ICResp handshake.
 * ipv4NetworkOrder is the address in network byte order (the same value a
 * registry REG_DWORD holds in nvmeofk.c); port is HOST order (4420, not 0x4211).
 * Returns STATUS_SUCCESS and fills *conn on success. */
NTSTATUS NvtcpOpen(_In_ WSK_REGISTRATION* reg, _In_ WSK_PROVIDER_NPI* provider,
                   _In_ ULONG ipv4NetworkOrder, _In_ USHORT port,
                   _Out_ PVOID* conn);

/* Close the connection and release everything it owns.  Safe to call twice, and
 * safe to call on a NULL pointer. Bounded by a timeout. */
NTSTATUS NvtcpClose(_In_ PVOID conn);

/* One NVMe command.  inData/outData may be NULL and must not both be given.
 * bytes must be <= NVMEOF_TCPK_MAX_DATA (in-capsule). Fills *status with the
 * NVMe status code (0 = success). */
NTSTATUS NvtcpCommand(_In_ PVOID conn, _In_ UINT8 opcode, _In_ UINT32 nsid,
                      _In_ UINT32 cdw10, _In_ UINT32 cdw12,
                      _In_reads_bytes_opt_(inBytes) PVOID inData, _In_ ULONG inBytes,
                      _Out_writes_bytes_opt_(outBytes) PVOID outData, _In_ ULONG outBytes,
                      _Out_ UINT8* status);

/* Synchronous identify: 4096 bytes of controller (cns = 1, nsid = 0) or namespace
 * (cns = 0, nsid = 1) data.  Returns a translated NTSTATUS: a non-zero NVMe
 * status becomes STATUS_IO_DEVICE_ERROR and is logged with the status code. */
NTSTATUS NvtcpIdentify(_In_ PVOID conn, _In_ UINT8 cns, _In_ UINT32 nsid,
                       _Out_writes_bytes_(4096) PVOID out4096);

#endif /* NVMEOF_TCPK_H */
