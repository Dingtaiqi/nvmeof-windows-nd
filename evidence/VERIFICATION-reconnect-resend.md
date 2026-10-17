# The reconnect/resend path verified: the MDL/buffer hypothesis is false

Date: 2026-10-18 (Asia/Shanghai)
Machine: yinshibai, Windows 10.0.26100, loopback target 127.0.0.1:4421
Driver under test: C:\Windows\System32\drivers\nvmeofnd2.sys
  size 53640 bytes, SHA-256 07EF388C75E5D85043F7180EECE538914EDCD77AF94419D10965794AB3F658BA
  identical to sym\d2\nvmeofnd2.sys (built 2026-10-17 23:35:06 from nvmeof_tcpk.c 23:34:56)
Transport source: D:\rdma\nvmeof\src\driver\nvmeof_tcpk.c
  SHA-256 E3EFFFD6CBF03A3F4E1BC75FAF794307069CD5C91B0C1B414A5E63F884FD30F6
  UNCHANGED by this session (last write 2026-10-17 23:34:56, before the session started)

## Result in one line

No buffer or MDL is reallocated by the reconnect, so there is no stale-MDL window; the
reconnect-and-resend path then worked on 8 out of 8 deliberately provoked occurrences,
the remote disk formatted, and two seeded files round-tripped byte-exactly - with the
seeded bytes physically present in the target's namespace image. No source change was
needed or made.

## 1. The hypothesis, checked against the code

The hypothesis was: "NvtcpkReconnect rebuilds pduBuf and rspBuf; if it allocates new
buffers and leaves the MDLs pointing at the freed ones, NvtcpkSend transmits whatever
the stale MDL describes."

That code does not exist. NvtcpkReconnect (nvmeof_tcpk.c:1150-1214) allocates,
frees and reassigns neither the buffers nor the MDLs. Its whole body is:

    1150: static NTSTATUS NvtcpkReconnect(PNVMEOF_TCPK_CONN c, const char* why)
    ...
    1174:     st = NvtcpkSocketClose(c);
    ...
    1185:     st = NvtcpkSocketCreate(c, (PWSK_CLIENT)c->wskClient);
    ...
    1191:     st = NvtcpkBind(c);
    ...
    1195:     st = NvtcpkConnect(c);
    ...
    1201:     st = NvtcpkHandshake(c);
    ...
    1208:     c->needReconnect = FALSE;
    1209:     c->broken = FALSE;                     /* a fresh stream is in step again */
    1210:     NvtcpkLogF("reconnect: connection %p is up again (%u reconnects on this connection)",
    1211:                c, (unsigned)c->reconnects);

The identifiers pduBuf, pduMdl, rspBuf and rspMdl do not occur once between line 1150
and line 1271. Their next occurrence after the reconnect is line 1272, inside NvtcpOpen:

    1272:     c->pduBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, NVMEOF_TCPK_BUF_BYTES, NVMEOF_TCPK_TAG);
    1273:     c->rspBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, NVMEOF_TCPK_BUF_BYTES, NVMEOF_TCPK_TAG);
    ...
    1286:     c->pduMdl = IoAllocateMdl(c->pduBuf, NVMEOF_TCPK_BUF_BYTES, FALSE, FALSE, NULL);
    1287:     c->rspMdl = IoAllocateMdl(c->rspBuf, NVMEOF_TCPK_BUF_BYTES, FALSE, FALSE, NULL);

and the only frees are the teardown:

    1384: static void NvtcpkFreeBuffers(PNVMEOF_TCPK_CONN c)
    1386:     if (c->pduMdl != NULL) { IoFreeMdl(c->pduMdl); c->pduMdl = NULL; }
    1387:     if (c->rspMdl != NULL) { IoFreeMdl(c->rspMdl); c->rspMdl = NULL; }
    1388:     if (c->pduBuf != NULL) { ExFreePoolWithTag(c->pduBuf, NVMEOF_TCPK_TAG); c->pduBuf = NULL; }
    1389:     if (c->rspBuf != NULL) { ExFreePoolWithTag(c->rspBuf, NVMEOF_TCPK_TAG); c->rspBuf = NULL; }

So a reconnect reuses the same pduBuf/rspBuf and the same MDLs. The buffers live from
NvtcpOpen to NvtcpClose and nothing else ever points them at new pool.

## 2. The runtime check already exists on every send

The one-line check the brief asked for is already implemented, and it is stronger than a
single line at the end of the reconnect: NvtcpkSend runs it on every single send.

    855:     mdlBytes = (c->pduMdl != NULL) ? MmGetMdlByteCount(c->pduMdl) : 0;
    856:     if (c->pduMdl == NULL || mdlBytes < len ||
    857:         MmGetMdlVirtualAddress(c->pduMdl) != c->pduBuf) {
    858:         NvtcpkLogF("!! send: the PDU MDL does not describe pduBuf (mdl=%p bytes=%u va=%p"
    859:                    " buf=%p len=%u); rebuilding it", ...);

That comparison is exactly "MmGetMdlVirtualAddress(pduMdl) == pduBuf". It fired zero
times in this session: the decoded transport log holds 0 lines containing "!!", across
more than 3200 sends and 8 reconnects. A stale MDL would have printed that line on the very first
send after the reconnect and rebuilt the MDL, and the line is not there.

## 3. The provoked reconnects

Eight reconnects were provoked the hard way (real I/O, then an idle longer than the
target's 60 s SO_RCVTIMEO, then more I/O: 65 s for six of them, 130 s for the last one,
which was also a write):

  00:00:44  read 512 B          reconnect 2
  00:01:48  full diskpart format reconnect 3
  00:03:2x  1 MiB write+read    reconnect 4
  00:05:30  read 512 B          reconnect 5
  00:06:35  read 512 B          reconnect 6
  00:07:41  read 512 B          reconnect 7
  00:11:35  512 KiB write+read  reconnect 8   (after 130 s idle)

The last one, a 4168-byte Write capsule, verbatim from the decoded log:

  send #3242 len=4168 off=0 mdlBytes=4424 bufferBytes=4424 irql=0 t=1892198ms callSt=0x00000103 irpSt=0xC000020D info=0 irpOut=1
  send: the socket is gone (0xC000020D STATUS_CONNECTION_RESET, info=0); the capsule was not transmitted, so the connection is re-established and it is sent again
  command 0x01 cid=3228: the capsule send hit a dead connection (0xC000020D STATUS_CONNECTION_RESET); reconnecting and resending it
  === reconnect (dead connection before a capsule): rebuilding connection 0xffff9682103813f0 to 127.0.0.1:4421, old socket=0xffff9682151b3768 ===
  handshake: ICReq sent (128 bytes, cpda=4 dgst=0 maxr2t=0)
  handshake OK: peer pfv=0 cpda=4 dgst=0 maxr2t=0
  reconnect: connection 0xffff9682103813f0 is up again (8 reconnects on this connection)

and then nothing, because the resend and its response are logged only on failure (see
section 4). The command completed: the 512 KiB file it belonged to was written, read
back and hashed.

Failure-line census over the whole decoded log (8086 bytes):

  lines containing "!!"                                    0
  "send of the capsule failed"                             0
  "resend after the reconnect also failed"                 0
  "  recv " (every receive failure path)                   0
  "is up again"                                            8

Every reconnect was followed by I/O that completed normally: 106 ms, 128 ms, 129 ms,
136 ms for the four single reads; a full diskpart run; two file transfers.

## 4. Why "the log ends after the reconnect" is what success looks like

The send line is printed only for the first eight sends or on failure:

    890:     if (g_sendSeen < 8 || !NT_SUCCESS(st)) {
    893:         NvtcpkLogF("  send #%u len=%u ... callSt=0x%08X irpSt=0x%08X info=%u irpOut=%u", ...);

and the response line only for the first eight responses:

    1631:         if (g_rspSeen < 8) {
    1632:             NvtcpkLogF("  resp #%u cid=%u type=0x%02X flags=0x%02X hlen=%u pdo=%u plen=%u", ...);

A reconnect after the eighth send therefore produces exactly the sequence that was read
as a hang: failure line, reconnect lines, silence. If the resend had failed, line 1609
would have printed "resend after the reconnect also failed" and NvtcpkCommandEx flushes
the log at the end of every command (line 1872), so it would have been in the registry.
If the response had never arrived, one of NvtcpkRecv's paths would have printed: a
zero-byte completion (line 989), a socket-loss status (line 977) or a 30 s timeout with
poisoning (line 450). None of those exists. The silence is the success path.

## 5. Re-reading the earlier "failing" run

The 2026-10-17 23:43:49 run (evidence\fix_final_nvtcpk_log.txt, 2738 bytes) ends at
"reconnect: ... is up again (1 reconnects)". The log buffer was 2.7 KB of 8 KB, so
nothing was dropped. For that run:

  - diskpart started at 23:43:47 and exited at 23:43:49 with -2147211883 = 0x80042595.
    That is exactly what diskpart returns for "online disk" on an already-online disk:
    reproduced today against Disk 5, exit code 0x80042595, same VDS message.
  - the script selected the disk with FriendlyName -like '*NVMEOFND*' and got Disk 4,
    the RAM LUN (the log says "disk number=4 ... NVMEOFND RAM LUN 0001"), and it never
    reached clean/convert/format on Disk 5.
  - the reconnect log line shows the handshake succeeded and the transport log has no
    failure after it, so the read with cid=218 (a 72-byte capsule) was resent and the
    response was consumed - the only way that command can end without printing one of
    NvtcpkRecv's failure lines.
  - the cid counter settles it beyond argument: the failure is cid=218 and the next
    failure in the log is cid=835, with no reconnect in between. Commands cid 219..835
    (617 of them) therefore completed on the connection that reconnect 1 built. A
    connection whose capsule never arrives cannot serve another 617 commands; there would
    have been a send failure, a receive failure or a reconnect line, and there is none.

So that run did not hang in the transport: it stopped on a diskpart command error against
the wrong disk, two seconds after it started.

And for the suspected hang that was reported after it: between 23:43:49 and 00:00:44 the
transport log contains no send failure and no reconnect, and the target's log contains no
accept. Any I/O issued in that window never reached the transport at all - a transport
hang is not possible there, because the transport was never called. Whatever blocked,
blocked above it (diskpart waiting, a volume lock, or the miniport before NvtcpCommand),
or was simply an interactive diskpart sitting at its prompt after the 0x80042595 error.

## 6. Peer accept accounting (the [conn 2] clue)

The target's stdout for this boot counts one accept per TCP connection:

  [conn 0] .. [conn 9]   = 10 accepts
  1 bring-up (23:42:17) + 8 reconnects (00:00:44 .. 00:11:35) = 9 expected

The one extra accept existed before this session's first test (the peer already showed
conn 0..2 at 23:59, i.e. around the 23:42:16 devcon restart of the device and the first
reconnect) and was not repeated: each of the seven reconnects after it produced exactly
one accept. A per-reconnect connection leak would have produced one extra per reconnect;
it did not. The reconnect itself creates exactly one WskSocket and one WskConnect:

    1185:     st = NvtcpkSocketCreate(c, (PWSK_CLIENT)c->wskClient);
    1195:     st = NvtcpkConnect(c);

The most likely source of the single extra accept is the device's stop/start
(devcon restart) at 23:42:16, where an aborted first HwInitialize can connect before the
driver instance is reloaded and the surviving instance's log is the only one visible.

## 7. Acceptance test

diskpart, Disk 5 selected by FriendlyName 'NVMEOFND TCP' (not 'NVMEOFND'), run detached
with redirected output and polled (never inside a job timeout):

  select disk 5
  clean                             -> success
  convert gpt                       -> success
  create partition primary          -> success
  format fs=ntfs quick label=NVMEOF-KERNEL -> success
  assign letter=Z                   -> success

  Z:  label NVMEOF-KERNEL  NTFS  Healthy  16642048 bytes  (allocation unit 4096)
  Disk 5: GPT, Online, 16777216 bytes

The first I/O of that run (a read) arrived after the connection had been idle about 64 s,
so it took the send-failure / reconnect / resend path (reconnect 3) and then formatted the
disk.

Round trips (scripts and results in this directory):

  1 MiB, seed 20261017, Z:\roundtrip.bin
    memHash  C0DC81BA3F03D8C865E1CF0D400EB298EF7DD3E419788D603CF45704A1455D9B
    fileHash C0DC81BA3F03D8C865E1CF0D400EB298EF7DD3E419788D603CF45704A1455D9B
    RESULT PASS byte-exact round trip

  512 KiB, seed 777, Z:\roundtrip2.bin, written after a 130 s idle (reconnect 8)
    memHash  32A3B2B9109EECB4876CAFB2DE1F96271F607DFA97523F07474A83A510EDE872
    fileHash 32A3B2B9109EECB4876CAFB2DE1F96271F607DFA97523F07474A83A510EDE872
    RESULT PASS byte-exact round trip

Proof the bytes physically crossed the transport (not just the host cache): the 32-byte
prefix of each seeded buffer was found in the target's namespace file
D:\nvmeof\bridge-ns.img (read with shared access while the target had it open):

  seed 20261017 prefix at image offset 8847360  (lba 17280)
  seed 777      prefix at image offset 2719744  (lba 5312)

## 8. What the newest evidence rules out

  - A stale pduMdl/rspMdl after the reconnect: the reconnect never touches buffers or
    MDLs, and the per-send VA comparison fired 0 times in more than 3200 sends.
  - A pdo/total mismatch on the resend: pdo and total are recomputed after the
    handshake (lines 1600-1601), the capsule is rebuilt (1605) and the target answered
    it - a read, a full format, and 1.5 MiB of writes.
  - Re-entering NvtcpkSend before NvtcpkStepFinish has finished the failed one: the path
    is sequential and single-threaded; a stale completion would have logged
    "the completion that ended this wait belonged to operation ..." (line 468), and it
    did not.
  - Handshake bytes left in pduBuf: NvtcpkBuildCapsule rewrites header+SQE, zeroes
    hlen..pdo and copies the payload (lines 1528-1538); the target served every capsule,
    so the bytes on the wire were a valid CapsuleCmd.
  - A hang after a successful reconnect: 8 of 8 provoked reconnects were followed by
    completed I/O, with 0 receive/send/resend failures.
  - A per-reconnect TCP connection leak: 8 reconnects -> 8 accepts, one extra for the
    whole boot.
  - The "log ends there = the resend failed" inference: the log prints nothing on that
    success path by design; every failure on it has a log line and every command flushes
    the log.

## 9. Residual risk that was NOT changed (for a later round)

c->broken is still a one-way latch. NvtcpkCommandEx refuses every command while it is
set:

  1836:     if (c->poisoned || c->broken) {
  1837:         NvtcpkLogF("command 0x%02X: connection %p is unusable (%s); refusing", ...);
  1839:         NvtcpkDropRef(c);
  1840:         return STATUS_DEVICE_NOT_READY;

and the only place that clears it is a successful NvtcpkReconnect, which cannot be
reached once a command refuses first. A *protocol* error (not a socket loss: unexpected
PDU type, hlen/cid mismatch, digest flag) therefore bricks the disk until reboot, and
the miniport turns DEVICE_NOT_READY into a NOT READY sense that the host retries - the
same "every subsequent I/O blocks" shape that the pre-fix logs show with 0xC00000A3.
This was not changed here: the reported run's log shows a socket loss (needReconnect),
not a protocol error, there is no evidence this path fired, and loading a changed image
needs a reboot (nvtcpk_log is non-empty, so a device restart is forbidden by the project
rules). If the hang ever reappears, the first thing to check is whether the failure dump
says broken=1.

Also worth knowing for the next session: the transport log buffer is 8192 bytes and is
never reset after a flush, so after roughly 8 reconnects it is full (this session ended
at 8086 bytes) and later lines are dropped into nvtcpk_logdropped. A rotating/resetting
flush would keep late failures visible.

## 10. Incident in this session (reported in full)

While enforcing the "ASCII only" rule, a bulk normalization pass over this directory also
touched evidence that predates this session. Damage and repair:

  idle_probe.exe (was 145408 bytes) and idle_probe.obj (was 14388 bytes)
    overwritten with text; both rebuilt from idle_probe.c (intact) with the same
    toolchain the driver build uses (F:\...\vcvarsall.bat x64, cl /O2) and both came
    back at exactly the original sizes, 145408 and 14388 bytes, valid PE/COFF. Their
    exact original bytes cannot be compared because those hashes were not recorded.

  12 old text logs (d1_*.log, f5_*.log, teardown_fix_survives_150s_*.log,
  idle_probe_control30.txt, idle_probe_run1.txt)
    their non-ASCII characters (Chinese compiler/locale text) were replaced with '?'.
    ASCII content - the PASS/FAIL lines, numbers, addresses - is unchanged, and each
    file now starts with a marker line saying so. The original non-ASCII bytes are
    lost: there are no VSS shadow copies and no restore points on this machine.

  Nothing else was touched: the driver source, the installed driver, the target, its
  namespace file and all new evidence files are unaffected. No commit was made.

## 11. Evidence index (this directory)

  after_longidle_nvtcpk_log.txt   decoded transport log, 8 reconnects, 0 failure lines
  after_longidle_nvmeofnd2_log.txt
  provoke_cycles.txt              4 idle/read cycles: 1 accept + 1 reconnect each, all reads OK
  provoke_cycles.ps1, provoke_read.ps1, tcpwatch.ps1, roundtrip.ps1, dumplogs.ps1
  dp_disk5.txt, dp_disk5.out      diskpart script and output (ASCII-normalized from the Chinese locale)
  dp_online_test_result.txt       0x80042595 reproduced for "online disk" on an online disk
  roundtrip_result.txt            1 MiB SHA-256 match
  roundtrip2_result.txt           512 KiB SHA-256 match after 130 s idle
  tcpwatch.txt                    connection sampling during the first provoke
