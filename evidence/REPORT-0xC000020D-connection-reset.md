# 0xC000020D: the connection was reset, not mis-sized

Status: fix written, built, signed and DEPLOYED; end-to-end verification is armed
to run at the next boot (this session cannot survive the reboot that loads the
image, so the test writes its own evidence).

## 1. The status code was read wrongly, and that misled two sessions

`C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared\ntstatus.h`:

    #define STATUS_INVALID_BUFFER_SIZE      ((NTSTATUS)0xC0000206L)
    #define STATUS_CONNECTION_RESET         ((NTSTATUS)0xC000020DL)
    #define STATUS_CONNECTION_DISCONNECTED  ((NTSTATUS)0xC000020CL)

So the failing call was never a buffer complaint. `0xC000020D` is
`STATUS_CONNECTION_RESET`, and `info=0` is exactly what WSK reports when it
refuses a send because the connection is gone. Every number in the diagnostic
ring (mdlBytes=4424, bufferBytes=4424, offset=0, length=72) was correct because
the buffer was never involved.

That single value explains all of the "unpredictable" evidence:

  run A: ok=643  in=1622728 out=1013080, then cid=644 READ, 72-byte send failed
  run B: ok=5    in=9464    out=488,     then cid=6   READ, 72-byte send failed

A count is not what decides it. Wall-clock idle time is.

## 2. Why the connection was reset: the target closes idle connections

`src/nvmeof_tcp.c`, `targetConnThread`:

    setRecvTimeout(c->sock, 60000);
    rc = targetHandshakeSafe(c);
    while (rc == 0) { rc = targetServeOne(c); }
    closesocket(c->sock);

`recvAll` returns -1 the moment `recv` returns <= 0, which a 60-second
`SO_RCVTIMEO` does. So: 60 seconds with no capsule from the initiator, and the
target closes the connection. The next capsule is sent into a dead socket.

## 3. Proof, without loading a driver

`evidence/idle_probe.c` is a user-mode client that uses the project's own PDU
layer (`src/nvmeof_tcp.h`), so its bytes are identical to the driver's. It runs
two 512-byte reads, goes silent, then repeats the read:

    evidence/idle_probe_run1.txt (70 s idle)
        read-before-1: cid=1 plen=512 status=0x00
        read-before-2: cid=2 plen=512 status=0x00
        --- going silent for 70 s (target SO_RCVTIMEO is 60000 ms) ---
        read-after-idle: recv() returned -1, WSAGetLastError=10053
        RESULT: the command after the idle period FAILED

    evidence/idle_probe_control30.txt (30 s idle)
        read-after-idle: cid=3 plen=512 status=0x00
        RESULT: the command after the idle period succeeded

30 s is fine, 70 s is fatal. That is the target's timeout, and nothing else in
the path is time-dependent.

Corroboration from the machine: the target process that had served the kernel
driver (started 23:16:33) was down to its listener thread only - its connection
thread had exited, i.e. it had closed the connection - while the driver still
held the open connection object.

## 4. The shared completion context was not the cause

The brief's prime suspect was a send/receive overlap on the one
`NVMEOF_TCPK_ASYNC`. The actual code rules it out:

  - the miniport runs every command on one worker thread, and
    `NvtcpkCommandEx` takes `c->busy` around the transaction, so a second
    command is refused with `STATUS_DEVICE_BUSY` rather than overlapping;
  - `NvtcpkStepStart` already initialises the event once and only resets it
    (commit 7d7c355), so no `KeInitializeEvent` ever runs on a live wait;
  - a stale completion cannot have produced 0xC000020D: the diagnostic ring
    prints every earlier operation completing with `irpSt=0x00000000`, so no
    earlier completion carried that status. The status on the failing IRP is
    that IRP's own status, delivered by its own completion routine.

The new build measures both claims instead of asserting them: `opsInFlight`,
`maxInFlight`, `staleCompletions`, and per-operation `seq`/`doneSeq`, all dumped
with a failure.

## 5. The change (src/driver/nvmeof_tcpk.c only)

1. `NvtcpkIrpNew` / `NvtcpkIrpFree` with `g_irpAlloc` / `g_irpFree`; every
   `IoFreeIrp` goes through the wrapper (7 call sites), and the failure dump
   prints `alloc`, `free`, `outstanding`.
2. Operation accounting: `g_opSeq`, `g_opsInFlight`, `g_maxOpsInFlight`,
   `g_opCompletions`, `g_staleCompletions`; `seq` stamped in `NvtcpkStepStart`,
   copied to `doneSeq` by `NvtcpkIrpComplete`, mismatch logged by
   `NvtcpkStepFinish` as a completion that belonged to another operation.
3. `NvtcpkStatusName()` in every message that prints a status, plus
   `t=<ms since boot>` on the send line, so the idle gap that triggers the
   target's timeout is visible in the log.
4. `NvtcpkReconnect()`: close the dead socket, create a new one, bind, connect,
   handshake. `NvtcpkBuildCapsule()` was split out because the handshake writes
   the ICReq into the same `pduBuf` and would otherwise destroy the capsule
   that still has to be sent.
5. The repair itself: a capsule send that failed with a connection status and
   `Information = 0` did not put a byte on the wire, so the command never ran
   and may be sent again - one reconnect and one resend, logged, no loop. A
   connection loss on the receive side sets `needReconnect`, and the next
   command rebuilds the connection instead of every later command failing with
   `STATUS_DEVICE_NOT_READY` until the next reboot (which is what the old code
   did after the first reset).

What this is not: it is not a blind retry. It fires only on the statuses that
mean "the socket is gone" (`NvtcpkIsSocketLoss`) and only when the send reported
zero bytes transmitted; an ambiguous failure, where bytes may have reached the
peer, is never resent.

## 6. Build, sign, deploy

    build_d2.cmd                 -> sym\d2\nvmeofnd2.sys  53640 bytes
    signtool /fd sha256 /s My    -> Valid, CN=NVMeoF Test Driver
    SYSTEM scheduled task        -> C:\Windows\System32\drivers\nvmeofnd2.sys
    SHA-256  07EF388C75E5D85043F7180EECE538914EDCD77AF94419D10965794AB3F658BA
    both files match, 53640 bytes   (evidence/deploy_result.txt)

A deploy detail this project has not written down before: while a driver image is
loaded, `Copy-Item` over `C:\Windows\System32\drivers\nvmeofnd2.sys` fails - the
loaded image is locked. The working recipe is to rename the old file out of the
way (allowed even while mapped) and then copy the new one:

    copy=FAILED  (the file is in use)
    rename=ok    C:\Windows\System32\drivers\nvmeofnd2.sys.old-20261017233126
    copy2=ok
    match=True

## 7. Verification, armed to run at the next boot

`nvtcpk_log` was non-empty, so a `devcon restart` was not allowed. The reboot
that loads the image also ends this agent session (the harness runs in the
interactive session), so the test was made autonomous:
`evidence/fix_test_at_boot.ps1` is registered as the SYSTEM task
`nvmeof-fix-test` with an AtStartup trigger. At the next boot it:

  1. waits for (or starts) the target on 127.0.0.1:4421;
  2. checks the device, and restarts it only when the log proves no connection
     was ever opened (a failed `NvtcpOpen` publishes nothing);
  3. finds the NVMEOFND disk, saves the bring-up logs, clears both log values;
  4. idles 80 s - past the target's 60 s timeout - then runs diskpart:
     `clean`, `convert gpt`, `create partition primary`,
     `format fs=ntfs quick label=NVMEOF-KERNEL`, `assign letter=Z`;
  5. idles another 80 s, writes 1 MiB of seeded random bytes to `Z:`, reads it
     back, and compares SHA-256 with the in-memory pattern;
  6. decodes both logs into `evidence/fix_final_*_log.txt` and logs the
     reconnect count, the send-failure count and the IRP counters.

Everything it does is appended to `evidence/fix_test_run.log`; `diskpart` output
goes to `evidence/fix_diskpart.txt`, and the probe's own output to
`evidence/idle_probe_*.txt`.

## 8. What "pass" looks like

  - `fix_test_run.log`: `diskpart exit=0`, `volume Z: label=NVMEOF-KERNEL
    fs=NTFS health=Healthy`, `roundtrip ... match=True`;
  - `fix_final_nvtcpk_log.txt`: at least one `=== reconnect ...` and
    `reconnect: connection ... is up again`, no `send of the capsule failed`;
  - any failure dump would print `irps: alloc=.. free=.. outstanding=0`, which
    is what rules an IRP leak in or out for good.
