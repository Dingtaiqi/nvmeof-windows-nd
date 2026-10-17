# D1.2 first-I/O fix session, 2026-10-17

## Starting state (before any change)
- Service nvmeofnd2 RUNNING; device root\nvmeofndi2 present; Disk5 = 16 MiB GPT Online,
  one 16646144-byte basic partition at offset 65536, no drive letter, no volume.
- Target already listening: PID 34160 "nvmeof_tcp.exe -target 127.0.0.1 4421
  -nsfile D:\nvmeof\bridge-ns.img -blocks 32768 -serve", started 19:10:22.
- Existing registry evidence (nvmeofnd2_log present, nvtcpk_log ABSENT at read time):
  first failing line is
    !! READ lba=128 blocks=1 bytes=512: transport 0xC000020D nvme_status=0xFF (done 0 bytes)
  followed by 0xC00000A3 for every later command (connection marked broken).
- evidence/kernel-drive-first-run.txt records the transport log ending with
    command 0x02 cid=228: send of the capsule failed 0xC000020D
  i.e. cid 228 means 227 transactions were issued on that connection: the two
  Identifies plus ~225 commands that succeeded silently (success logs nothing).
  So the connection was NOT failing on the very first I/O.

## Code review of the send path as found
- src/driver/nvmeof_tcpk.c NvtcpkSend(): WSK_BUF{Mdl=c->pduMdl, Offset=0, Length=len}
  with the MDL built once over the whole NVMEOF_TCPK_BUF_BYTES (4424) buffer.
  Identical shape for the ICReq (128 bytes) and both Identify capsules (72 bytes),
  which all succeeded, so Length < MDL ByteCount is not by itself rejected.
- NvtcpkTransaction(): pdo = align4(72) = 72, total = pdo + inBytes; a Read has
  inBytes = 0, so its capsule is exactly 72 bytes, the same length that succeeded twice.
- Therefore the documented hypothesis (a length that is only right at 4096) cannot
  explain the evidence; the failure needs measurement, not another guess.
