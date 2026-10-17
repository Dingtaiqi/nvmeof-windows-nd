# Root cause found: 0xC000020D is STATUS_CONNECTION_RESET, not a buffer-size error

Date: 2026-10-17 23:35 (Asia/Shanghai)
State: new image built, signed, DEPLOYED to C:\Windows\System32\drivers\nvmeofnd2.sys
       (SHA-256 07EF388C75E5D85043F7180EECE538914EDCD77AF94419D10965794AB3F658BA,
       53640 bytes, matches sym\d2). The loaded image is still the old 19:42 one.
       A REBOOT is required to load it: nvtcpk_log is non-empty, so `devcon restart`
       is forbidden by the project's own hard rule (0xCE).

## The misreading that cost the previous sessions

C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared\ntstatus.h:

    #define STATUS_INVALID_BUFFER_SIZE    ((NTSTATUS)0xC0000206L)
    #define STATUS_CONNECTION_RESET       ((NTSTATUS)0xC000020DL)

0xC000020D is STATUS_CONNECTION_RESET. The buffer was never the suspect; the
connection was reset. That also explains info=0: WSK reports nothing transmitted
when it refuses a send on a dead connection.

## Why the connection was reset (proved, not guessed)

src/nvmeof_tcp.c, targetConnThread():

    setRecvTimeout(c->sock, 60000);
    rc = targetHandshakeSafe(c);
    while (rc == 0) { rc = targetServeOne(c); }
    closesocket(c->sock);

and recvAll() returns -1 as soon as recv() returns <= 0, which a 60-second
SO_RCVTIMEO does. So the target closes any connection that is idle for 60 s.

Reproduced from user mode, without touching the driver, with evidence/idle_probe.c
(same PDU layer as the driver, src/nvmeof_tcp.h):

  evidence/idle_probe_run1.txt      two reads succeed, 70 s idle, next command
                                    FAILS: recv() -> WSAECONNABORTED 10053
  evidence/idle_probe_control30.txt same test with a 30 s idle: SUCCEEDS

So it is wall-clock idle time, not a command count, not a size and not an IRP
leak. Six commands once and 643 another time is exactly what an idle timeout
looks like.

## Why the shared completion context is not the cause

The transport is strictly serialized: the miniport runs every command on one
worker thread, and NvtcpkCommandEx takes c->busy around the transaction, so a
send and a receive cannot overlap. StepStart already initializes the event once
and only resets it (the fix from commit 7d7c355). And the failing status cannot
be a stale completion: the diagnostic ring shows every earlier operation
completing with 0x00000000, so no earlier completion carried 0xC000020D. The
status on the failing IRP is that IRP's own status.

## The change (src/driver/nvmeof_tcpk.c only)

1. NvtcpkIrpNew/NvtcpkIrpFree with g_irpAlloc/g_irpFree, every IoFreeIrp routed
   through the wrapper; the failure dump prints alloc/free/outstanding.
2. Real overlap/staleness measurement: g_opSeq/g_opsInFlight/g_maxOpsInFlight/
   g_opCompletions/g_staleCompletions, seq stamped in StepStart, copied to
   doneSeq by the completion routine, mismatch logged.
3. Status names in the log (NvtcpkStatusName) so 0xC000020D can never again be
   read as a buffer error, plus t=<ms since boot> on every send line so an idle
   gap is visible.
4. NvtcpkReconnect(): close the dead socket, new socket, bind, connect,
   handshake (pduBuf is rebuilt afterwards because the handshake overwrites it).
5. A capsule send that failed with a connection status and Information = 0 means
   the command never reached the peer, so NvtcpkTransaction reconnects once and
   resends the same capsule. A recv-side connection loss sets needReconnect and
   the next command rebuilds the connection instead of failing until reboot.

## Next steps after the reboot

1. Check the target is listening (127.0.0.1:4421) and the device is OK:
   Get-PnpDevice -InstanceId 'root\nvmeofndi2'
   If the device failed because the boot task raced it, start the target and do
   one devcon restart (no connection is open after a failed open: the transport
   only publishes a connection after a successful handshake).
2. Clear HKLM\Software\NVMeoFProbe values nvmeofnd2_log and nvtcpk_log.
3. Sleep 75 s (past the target's 60 s timeout), then run diskpart on the
   NVMEOFND disk: clean, convert gpt, create partition primary,
   format fs=ntfs quick label=NVMEOF-KERNEL, assign letter=Z.
4. Get-Volume -DriveLetter Z; write a file to Z:, read it back, compare SHA-256.
5. Decode nvtcpk_log (the REG_SZ holds two ASCII bytes per UTF-16 code unit:
   emit low byte then high byte) and look for "reconnect" plus
   "irps: alloc=... free=... outstanding=0".
