# NVMe-oF over RDMA on Windows — a from-scratch NetworkDirect stack

**English** | [中文](README.zh-CN.md)

NVMe-oF/RDMA implemented on Windows against the **NetworkDirect (NDSPI)** API: fabrics
commands, 64-byte capsules, keyed SGL/STag, RDMA Read/Write, memory registration, and one
queue pair per I/O queue. **Both ends — initiator and target — are this repository's own
code.** No third-party NVMe-oF implementation is used anywhere.

Two things this gets you that Windows does not have on its own:

1. **A working NVMe-oF initiator on a client SKU.** Windows Server ships one; client
   editions do not, and no user-mode process can inject a volume into the storage stack.
2. **A remote disk as a real drive letter anyway** — via a user-mode **iSCSI target** that
   bridges Windows' built-in iSCSI initiator into this NVMe-oF stack. Measured end to end
   by mounting a **real 1 TB Linux NVMe SSD as `E:`** on Windows, read-only, with the
   drive's own SMART counters proving that not a single write reached it.

Start with the conclusions, not the code: **[DESIGN.md](DESIGN.md)** is the design and
measurement log (§8 is a section-by-section record of what broke and why), and
**[INTEROP_F5.md](INTEROP_F5.md)** is the runbook for using a separate Linux machine as an
independent peer. The deep docs are in Chinese; this file is the English entry point.

---

## Status

| # | Acceptance item | Result |
|---|---|---|
| 1 | Wire-format self-test (byte-exact golden vectors) | ✅ `run_wire.ps1` — compiles and passes as both C and C++ |
| 2 | Both ends agree on Identify | ✅ F1 / F6 |
| 3 | Write-then-read is byte-identical | ✅ F3 |
| 4 | Error paths are bounded (peer disappears, both directions) | ✅ F3 / F6 / F7 |
| 5 | **Our host ↔ Linux `nvmet`** | ✅ `initiator failures: 0` (36 checks): byte-identical WRITE/READ, a 32-deep in-flight pipeline, and **4 I/O queues each running a command concurrently** (DESIGN §8.46, §8.49) |
| 6 | Throughput vs a raw RDMA baseline | ✅ **1.81 GB/s** best (1722 MiB/s write, 8 in flight, 256 KiB commands) = **61%** of a freshly measured raw ceiling — the official NetworkDirect tools (same ND provider, same card, one QP, 10 s per size) do **2.97 GB/s = 59% of the 40G line rate**, and 2.42 GB/s at 8 MiB where the source buffer stops fitting in cache. Read the numbers with their conditions: 3 runs of each config give 1.45–1.60 GB/s mean, so this bridge's NVMe-oF layer costs roughly half the link, not 48% of a stale 2.53 GB/s figure (DESIGN §8.67, supersedes §8.16). **At depth 1 the same stack does 4.8 MiB/s**, and that is completion latency, not bandwidth: a 64 KiB payload moves and is answered in **31 µs** while the completion takes **12–22 ms** to reach the application (per-hop QPC trace, 768 commands, §8.67(3)) |
| 7 | **Our target ↔ Linux `nvme-cli`** | ✅ `nvme connect` rc=0, `nvme list` shows `NDVMEOF0000000000001`, 128 blocks written → flush → read back, `cmp` byte-identical; the host builds **8 I/O queues** and spreads commands over **6** of them (DESIGN §8.46, §8.49) |
| 8 | **In-band DH-HMAC-CHAP authentication** | ✅ Both directions against a real Linux peer: a Linux host authenticates to our target with `nvme connect -S <key>` (kernel log `qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048`; one-way and **bidirectional** both move data; a wrong key is refused; `authRefused=0`), and our host authenticates to an **authentication-requiring `nvmet`** (`Success2 sent (the controller's own response verified)`) (DESIGN §8.51, §8.52) |
| 9 | **A 1 TB real disk on Windows as an active drive** | ✅ Linux `/dev/nvme1n1` (CT1000P3PSSD8, 1953525168 × 512 B = 931.51 GiB) appears as `Get-Disk` #3, **Online**, GPT, NTFS `E:` readable; two byte-exact cross-checks against the Linux side; **zero writes** (DESIGN §8.55, §8.56, [EVIDENCE-1TB.md](EVIDENCE-1TB.md)) |
| 10 | **Sustained streaming** | ⚠️ Reads through the whole Windows path: **79 MB/s at queue depth 1** (64 KiB blocks, single synchronous reader — 0.755 ms per command, of which 0.635 ms is the Data-In socket write and 0.115 ms NVMe staging) rising to **230.6 MB/s with 8 concurrent readers**; 1 MiB reads reach **119.7 MB/s** at depth 1 (Windows escalates the CDB to 256 KiB, so one command is one 256 KiB NVMe read plus four Data-In PDUs — DESIGN §8.59). **Quote a bridge number only with its queue depth**: the earlier "73–102 MB/s" was the single-threaded fixture, not a ceiling. The per-command cost is the **initiator's** Data-In path, not this bridge: sending it 64 KiB measures 669 µs against 306 µs to receive the same 64 KiB from it, and two target-side optimizations (BHS+data in one `WSASend`, 1 MiB socket buffers) both measured as no-ops (§8.61). Zero errors throughout; data-path tracing is off by default because it costs ~20x — turn it on with `-iscsitrace` when debugging, or use `-iscsitime` for per-command timing. Bare stack: **1230 / 1179 MiB/s** (write/read, 20 rounds). **Writes through the bridge work at any size, including a 4 MiB write as a session's *first* write**, byte-exact through three independent witnesses — the initiator's read-back, the backend's namespace file after an explicit SYNCHRONIZE CACHE → NVMe FLUSH, and the PDU trace. Measured with the burst sizes tuned (see *Tuning the Windows side*): **4 MiB read 75–82 MB/s, 4 MiB write 69 ms, 16 MiB write 275 ms (61 MB/s)**, all byte-exact. Before that tuning the same bridge did 11.9 MB/s on a 4 MiB read, because Windows' initiator issues 256 KiB commands and charges a fixed ~16–20 ms per command and per Data-Out PDU (DESIGN §8.68). That is the current ceiling: not the fabric (2.97 GB/s idle), not the RDMA stack (1.5–1.8 GB/s pipelined), and not this bridge (it answers in 207 µs median and sits idle ≥94% of the time even with 8 concurrent readers). The bridge is read-only by default (a refused write drains the burst first, so the session survives), so the 1 TB result above is unaffected |

Items 5 and 7 are the only tests that can catch **both ends being wrong together** — and on
their first real run they found **11 defects**, 7 of them cases where our two ends shared a
misreading of the same field, while 8/8 self-tests had been green. **Self-consistency is not
correctness.**

The whole suite is one command — 11 of 11 suites, ~250 s:

```powershell
cd <repo>\src
.\run_all.ps1
```

## The Windows kernel driver

The user-mode stack above is the mature half of this project. The other half is a real
Windows kernel driver, because a user-mode process cannot put a volume into the Windows
storage stack — only a driver can, and that is what a drive letter in Explorer ultimately
requires.

| Stage | What it does | State |
|---|---|---|
| **D0** | Kernel driver that opens the NDK provider, registers a memory region and completes an **RDMA read against this hardware** | ✅ byte-identical, 4096 bytes |
| **D1** | **StorPort miniport** presenting a virtual LUN, so Windows sees a real disk | ✅ `Disk 4 NVMeoFND RAM LUN 0001`, BusType Fibre Channel, GPT + NTFS, **drive letter `X:`** with files written and read back through the volume stack |
| **D2** | The **NVMe-oF initiator in kernel mode**: adapter → PD → CQ → QP → connector → MR → registered region (rkey/lkey), all `STATUS_SUCCESS` | ✅ bring-up green and reproducible; the fabrics exchange is blocked on a live RDMA target |

Three things from that work are worth knowing even if you never load the driver:

- **A hypervisor blocks the Mellanox NDK provider.** With Hyper-V active, `NdkOpenAdapter`
  returns `0xC0010011` and anything that reaches the connection path bugchecks with `0x139`
  (`P1 = 0xa`, incorrect stack). With `bcdedit /set hypervisorlaunchtype off` the adapter
  opens and the whole path behaves. The hypervisor takes over DMA remapping; a 2020 WinOF
  driver on this card cannot establish itself underneath it. It blocks the **user-mode**
  NetworkDirect API too, not just the kernel one.
- **A failed RDMA connection must be closed, and the close callback takes one argument.**
  `NDK_FN_CLOSE_COMPLETION` is `VOID (PVOID Context)` — no status — while
  `NdkDeregisterMr` takes `(Context, Status)`. Using one two-parameter callback for both is
  a type mismatch, and a connection attempt that fails leaves state inside `mlx4eth63` that
  trips `KERNEL_SECURITY_CHECK_FAILURE` roughly **ninety seconds later**. That delay, and a
  reported stack that cannot be unwound, is why those crashes resisted a minidump for so
  long. See [TEARDOWN-FIX-PROVEN.md](TEARDOWN-FIX-PROVEN.md).
- **A single HCA cannot connect to itself.** `NdkConnect` from `192.168.100.2` to
  `192.168.100.2` — and the same with the user-mode initiator — is refused immediately with
  `0xC0000236 ND_CONNECTION_REFUSED`, while the target never sees the request. A second
  machine, or a second RDMA-capable port with its own address, is required.

The recorded state of that work, including what is still open, is in
[KERNEL-NVMEOF-STATUS.md](KERNEL-NVMEOF-STATUS.md) and
[FACTS-2026-10-05-1200.md](FACTS-2026-10-05-1200.md).
## What is implemented

- **Fabrics**: Connect (with the private-data checks Linux performs), Property Get/Set,
  discovery log, Authentication Send/Receive.
- **I/O**: 64-byte capsules, keyed SGL/STag, RDMA Read/Write, WRITE, READ, FLUSH, Compare,
  Write Zeroes, Dataset Management (deallocate), Keep Alive, Async Events, Identify (CNS
  0/1/2), Get/Set Features (including KATO unit handling), namespace attach.
- **Backends**: a RAM namespace, a **file-backed namespace** (`-nsfile`), and a real block
  device when used as `nvmet`'s consumer.
- **Multi-queue**: the target owns 8 queue pairs, each with its own capsule ring and
  in-flight table; the initiator creates as many as the peer grants.
- **Authentication**: DH-HMAC-CHAP (RFC 7919 `ffdhe2048` + `hmac(sha256)`), one-way and
  bidirectional, both as host and as target. The modular exponentiation is this repo's own
  fixed-width Montgomery implementation, because Windows CNG **measurably** refuses custom
  DH groups (`STATUS_NOT_SUPPORTED`) and refuses mismatched private keys
  (`STATUS_INVALID_PARAMETER`).
- **iSCSI bridge**: a minimal user-mode iSCSI target (login in three stages, SendTargets,
  NOP, Logout, Task Management, SCSI INQUIRY/VPD, TEST UNIT READY, REQUEST SENSE, MODE
  SENSE(6/10), READ CAPACITY(10/16), REPORT LUNS, READ/WRITE(10/16) via R2T, SYNCHRONIZE
  CACHE), so Windows' built-in initiator can mount a namespace as a disk with no kernel
  driver at all. A write too large for the command PDU is held as **state** (keyed by ITT,
  with 8 commands in flight) rather than waited for in a blocking read loop, so an initiator
  that answers an R2T with something else — or with nothing — no longer kills the session
  (DESIGN §8.64). Keep-alives go to the NVMe target's **admin** queue, where opcode 0x18
  belongs, and they go out every 20 s **even while the session is idle** — an idle mounted
  disk used to send none at all, which a KATO-enforcing nvmet answers by disabling the
  controller (§8.65, §8.66). A backend that dies ends the session immediately (hard errors,
  never silent success) and then the process exits non-zero so a service restart can
  reconnect (§8.66).

## The drive-letter path

### Option A — iSCSI bridge (live block device, what the 1 TB demo uses)

Our NVMe-oF initiator runs inside a user-mode iSCSI target; Windows' own initiator connects
to it over loopback.

```powershell
# 1) our NVMe-oF initiator reaches the Linux nvmet export
.\f5_interop.exe -initiator 192.168.100.5 4420 192.168.100.2 `
                 -subnqn nqn.2024-01.local.rdma:linux-nvmet -iscsi 3260

# 2) Windows' built-in initiator logs in (no kernel driver involved)
iscsicli AddTargetPortal 127.0.0.1 3260
Connect-IscsiTarget -NodeAddress iqn.2024-01.com.nvmeof:bridge0 -IsPersistent $false

Get-Disk | Where-Object BusType -eq 'iSCSI'     # -> NVMEOF iSCSI-NVMeoF, Online, 931.51 GB, GPT
Get-ChildItem E:\
```

Measured on that path (DESIGN §8.56, [EVIDENCE-1TB.md](EVIDENCE-1TB.md)):

- `Get-Disk`: 931.51 GB, GPT, **Online**, `IsReadOnly = True`; partitions read correctly
  (300 MB ESP + 16 MB MSR + 931.2 GB NTFS); the NTFS volume mounts and lists **60 top-level
  items** with real timestamps.
- **Byte-exact content check, twice.** A file read through `E:` (`fsutil file queryextents`
  → LCN → partition offset → absolute LBA 1073520) and the same LBA read directly on Linux
  with `nvme read` produce identical SHA-256
  (`dec615ae97b8b81cd5e395fd4f9b1fd379332c3516a16ab122c1c1a57a5dd397`); the ESP boot sector
  (LBA 40) matches too (`802b1462…a2452293`).
- **Zero writes, proven by the drive itself.** SMART `Data Units Written` was
  **32884752 before and 32884752 after** the whole session, while `Data Units Read` rose
  (144240893 → 144241018). Of 747 SCSI CDBs, exactly **one** was a write — Windows trying to
  set the NTFS dirty bit at LBA 651264 — and the bridge refused it: `WRITE … REFUSED
  (read-only bridge)` → `CHECK CONDITION` with 18 bytes of sense. A shell write attempt is
  refused by Windows itself: *"The media is write protected."*
- **Read-only is a rail, not a default you hope for**: `-iscsirw` is off unless asked, MODE
  SENSE reports the WP bit, and WRITE is refused at the SCSI layer. `nvmet` has no
  read-only namespace attribute, so the host side is the only place this can be enforced.

A pure-RDMA peer-to-peer path needs the peer on the same L2 as a RoCE port;
`src/interop_link.ps1 -Action Up` builds that (it bridges one CX3 port into the LAN,
moves the address and default route onto the bridge, lowers MTU to 1500/1024, verifies the
LAN still works, and rolls back if it does not). `-Action Down` undoes all of it.

### Option B — mount a namespace as a VHD volume (`mount_nvmeof.ps1`)

Pulls a namespace through our own initiator, wraps it in a fixed VHD and mounts it:

```powershell
.\mount_nvmeof.ps1                       # 64 MiB namespace from a Linux nvmet -> X:
.\f5_interop.exe -target 192.168.100.2 4420 -serve 0 -nsfile F:\nvmeof\target-ns.img
.\mount_nvmeof.ps1 -TargetIp 192.168.100.2 -Subnqn nqn.2024-01.local.rdma:windows-nd
.\mount_nvmeof.ps1 -Unmount              # write files in X:, push back on unmount
```

This one **is** a real Windows volume (NTFS, visible in Explorer, read/write) but it is **not
a live block device** — writes are pushed back at `-Unmount`. The full loop was verified: the
64 MiB image's SHA-256 read back on Linux matches the local image byte for byte, and a 2 MiB
random file keeps its checksum across a remount.

## Layout

| Path | What it is |
|---|---|
| `src/nvmeof_wire.h` | On-the-wire formats: capsule/CQE/SGL/Identify/status codes, each layout pinned by `NVMEOF_STATIC_ASSERT` |
| `src/nvmeof_rdma.h` | Transport: `Device` / `Queue` / `acceptChecked()` (validates the Connect private data the way Linux does) |
| `src/nvmeof_auth.h` | DH-HMAC-CHAP crypto primitives: CNG SHA/HMAC + this repo's Montgomery modexp (`nvmeof_bignum.h`) + RFC 7919 ffdhe groups (`nvmeof_dhgroups.h`) |
| `src/nvmeof_dhchap.h` | DH-HMAC-CHAP protocol: key parsing (`DHHC-1` + CRC32), `Kt` transform, target half, host half, loopback self-test |
| `src/nvmeof_iscsi.h`, `src/nvmeof_iscsi_pending.h` | **The user-mode iSCSI target**: three-stage login, SendTargets, NOP/Logout/TaskMgmt, and the SCSI command set (INQUIRY/VPD, MODE SENSE, READ CAPACITY, REPORT LUNS, READ/WRITE(10/16) over R2T, SYNCHRONIZE CACHE). One object and one thread per connection; the backend (a single queue pair) is serialized behind a mutex. A WRITE that does not fit in the command PDU is **parked as state by ITT** (`PendingWrite`, window 8) instead of blocking in `handleScsi()` waiting for Data-Out, and the parked table has three rules: one entry per ITT (a duplicate is refused with CHECK CONDITION `0x0B/0x00`), the table is **bounded** by the command window (over it is refused with `0x04/0x44`), and a write that receives no Data-Out for 60 s ends the session — longer than the initiator's own 60 s request hold plus 15 s SRB timeout, so it only ever fires on a peer that is really gone (DESIGN §8.72) |
| `src/f1_bringup.cpp` | F1 — connection and Identify |
| `src/f3_io.cpp` | F3 — read/write correctness and error paths (including the `0x4f` invalidate sub-type) |
| `src/f4_pipeline.cpp` | F4 — pipelined throughput (8 in flight) |
| `src/f5_interop.cpp` | **F5 interop**: `-initiator` against any peer, `-target` for real hosts (32-slot receive ring, sized to cover the window it advertises), `-iscsi` for the iSCSI bridge, `-nsfile` / `-serve` / `-genkey` and friends |
| `src/f6_lifecycle.cpp` | F6 — full host sequence plus target-side lifecycle (31 assertions) |
| `src/f7_faults.cpp` | F7 — fault injection (peer vanishes, and the reverse) |
| `src/wire_selftest.c` | Byte-level golden self-test, compiled as both C and C++ |
| `src/xref_constants.py` | Cross-checks every constant against the two Linux reference headers (104 checked pairs, 21 of them DH-HMAC-CHAP). The count is the one the script prints; it grows as the target advertises more |
| `src/run_all.ps1` | Runs all 14 suites and prints a verdict table (~250 s) |
| `src/run_f5_auth.ps1` | The six DH-HMAC-CHAP cases (two local ports, no Linux needed) |
| `src/nvmeof_config.h`, `src/bridge.conf.example` | The bridge's configuration file: one command-line token per line, without the leading dash, comments and trailing comments included. The file supplies OPTIONS, the command line supplies the MODE and its (positional) addresses, and a flag given on the command line wins. `-checkconfig <path>` validates a file and exits without opening anything |
| `src/run_fuzz.ps1` | The iSCSI wire fuzzer, built and run **twice**: 200k mutations under AddressSanitizer (the PDU builders, login text, the negotiation helpers, and the parked-write table driven as an operation sequence), then a second build with a deliberate read one byte past a heap buffer that ASAN **must** catch — if it does not, the suite fails, because a fuzzer that cannot fail looks exactly like one that is not looking. It found a real defect on its first run: a malformed `"12abc"` offer made the number-key negotiation hand back 12 bytes for FirstBurstLength/MaxRecvDataSegmentLength, where RFC 7143 §13.14 puts the floor at 512 (DESIGN §8.75) |
| `src/run_iscsi.ps1` | The iSCSI layer's own tests with **no hardware and no NIC**: 4-byte padding, the BHS accessors, every PDU builder against exact bytes, login-text parsing, the negotiation rules (InitialR2T is OR, ImmediateData is AND, numbers take the smaller value), the R2T burst sizing that protects the backend's staging buffer, and the parked-write table. `-AllToolsets` compiles it with every MSVC toolset on the machine |
| `src/hygiene.ps1` | The three repository-wide checks CI runs — SPDX headers on every source file, a UTF-8 BOM on every `.ps1`, no mojibake in the three docs — as a script, so the same code gates a push **and** a local run (`run_all.ps1` calls it). The rules used to live only in `ci.yml`, and a push then went red on a BOM its author had no way to check first |
| `src/tune_initiator.ps1` | Reads, applies or restores Windows' iSCSI initiator tuning (`MaxTransferLength` / `MaxBurstLength` / `MaxRecvDataSegmentLength`), and reloads the driver the way that actually works. `-Show` / `-Apply` / `-Restore` |
| `src/interop_link.ps1` | Interop link: bridge + address/route move + MTU lowering, with automatic rollback |
| `src/f5_session.ps1` | **Whole interop session** in one command: link → peer → direction A → discovery → direction B → report file; `-Auth` adds authentication |
| `src/mount_nvmeof.ps1` | Namespace → fixed VHD → drive letter → push back on unmount |
| `src/tools_bridge_status.ps1` | Reads the status file a running bridge writes (`-statusfile`, passed by `install.ps1` by default) and returns monitor-friendly exit codes: 0 fresh, 2 missing, **3 STALE** (the file stopped moving - the process died or hung, and the message says how to tell those apart), 4 unreadable. This exists because the bridge's own log is held open while the service runs |
| `src/tools_iscsi_write.ps1` | End-to-end byte check of the write path: deterministic pattern in, initiator read-back, a self-built SCSI pass-through SYNCHRONIZE CACHE, then a comparison against the backend's namespace file — three independent witnesses |
| `src/tools_iscsi_perf.ps1` | Bridge throughput through Windows' own initiator: starts the backend and the bridge, logs in, sweeps block sizes at queue depth 1 and with N concurrent readers, and prints the bridge's own per-command breakdown |
| `src/tools_nd_bw.ps1` | Pure-link baseline: drives the official NetworkDirect `nd_write_bw` / `nd_read_bw` / `nd_send_bw` / `nd_*_lat` tools across message sizes and parses the data row (the tools change column order between versions, and grepping for "Gb/s" picks up the header) |
| `src/tools_login_probe.ps1` | Replays the real Windows iSCSI login byte for byte at any target (used for the LIO A/B comparison) |
| `ref/linux_nvme.h`, `ref/linux_nvme_rdma.h` | Reference copies of the Linux headers, for comparison only |
| `linux/` | Peer-side scripts: `nvmet_setup.sh` (software RoCE `rxe` + optional `AUTH_KEY=`), `nvmet_teardown.sh`, `f5_linux_up.sh`, `f5_dirb_check.sh` (direction B), `f5_dirb_auth.sh` (direction B + auth), `f5_dirb_demo.sh`, `f5_nvmet_ref*.sh` (measure `nvmet` line by line as the specification), `f5_dsm_check.sh`, `lio_proxy.py` (byte tap in front of the LIO reference target) |

## Build prerequisites

1. **Visual Studio** with the C++ desktop workload (its `VsDevCmd.bat` provides `cl.exe`).
2. **NetworkDirect / NDSPI headers and library**: `ndspi.h`, `ndutil.h`/`ndutil.lib`.
   **These are not in this repository** — they are not this project's code. They come from
   the NetworkDirect parts of the WDK/Windows SDK or from a NIC vendor's ND provider package.
   The measured environment is an HP/Mellanox ConnectX-3 Pro with the WinOF ND provider.
   Vendors' NDv2 headers (e.g. `…\MLNX_VPI\IB\SDK\inc\ndv2`) are added to the include path if
   present.
3. Paths are overridden by **environment variables**; the defaults are the measured local
   paths, so **no script editing is needed**:

   | Variable | Meaning | Default when unset |
   |---|---|---|
   | `ND_VS_DIR` | Visual Studio install directory | `F:\Microsoft Visual Studio\18\Community` |
   | `ND_NDUTIL_INC` | NetworkDirect include directory | `D:\rdma\NetworkDirect\src\ndutil` |
   | `ND_NDUTIL_LIB` | Directory holding `ndutil.lib` | `D:\rdma\NetworkDirect\src\x64\Release` |
   | `ND_MLNX_INC` | Vendor NDv2 include directory (optional) | `C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2` |

   Every script locates itself through `$PSScriptRoot`, so the repository builds from
   wherever it is cloned:

   ```powershell
   $env:ND_VS_DIR     = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
   $env:ND_NDUTIL_INC = 'C:\ndsdk\src\ndutil'
   $env:ND_NDUTIL_LIB = 'C:\ndsdk\src\x64\Release'
   cd <repo>\src ; .\run_wire.ps1        # start here: needs no NIC
   ```

4. The two files under `ref/` are Linux kernel headers (GPL-2.0) kept **for reading and
   comparison only — they are not compiled**.

## Running

All 11 suites in order, mutually isolated, ~6 minutes:

```powershell
cd <repo>\src
.\run_all.ps1
```

Individually: `run_xref.ps1`, `run_wire.ps1`, `run_auth.ps1`, `run_f1.ps1`, `run_f3.ps1`,
`run_f4.ps1`, `run_f5.ps1`, `run_f5_auth.ps1`, `run_iscsi.ps1`, `run_f6.ps1`, `run_f7.ps1`,
`run_stag.ps1`.

`run_xref.ps1`, `run_wire.ps1`, `run_auth.ps1`, `run_iscsi.ps1` and `run_fuzz.ps1` need no NIC
and no NetworkDirect SDK, which is why CI runs exactly those five (plus `hygiene.ps1`, the three
repository-wide checks: SPDX headers, UTF-8 BOMs on every `.ps1`, and no mojibake in the docs —
the same script `run_all.ps1` runs first). `run_fuzz.ps1` builds the wire fuzzer twice: once to
fuzz 200k mutations under AddressSanitizer, and once with a deliberate out-of-bounds read that
the sanitizer **must** catch — if that second build passes, the suite fails, because a fuzzer
that cannot find anything is indistinguishable from one that is not looking. `run_auth.ps1` builds
`test_auth.cpp` — the DH-HMAC-CHAP primitives and protocol pieces against published vectors — and
then hands the same exe to `run_authselftest.ps1`, which recomputes the Diffie-Hellman values with
`System.Numerics.BigInteger` as an independent cross-check. The crypto is therefore verified on
every push, not only on a machine with the whole stack installed. `run_iscsi.ps1` does the same
for the iSCSI layer: no target, no initiator, no RDMA — it drives the PDU builders, the login
text, the negotiation rules and the parked-write table directly, so a change to any of them is
caught before it can reach a session.

Every suite follows the same rule: **delete the old exe → check the compiler's exit code →
compare source and header timestamps.** A binary that merely *looks* current is never run —
that rule came from a real incident (DESIGN §8.6).

## Running the bridge as a Windows service

The bridge is the piece meant to stay running: Windows' own iSCSI initiator connects to it and
the remote disk appears. `install.ps1` copies the exe, registers the service (automatic start,
restart on failure) and starts it; `uninstall.ps1` stops it, logs the iSCSI session out and
removes both directories:

```powershell
cd <repo>\src
# read-only by default; add -ReadWrite to allow writes
.\install.ps1 -Target 192.168.100.5 -Subnqn nqn.2024-01.local.rdma:linux-nvmet `
              -RdmaLocal 192.168.100.3 -IscsiPort 3260

iscsicli AddTargetPortal 127.0.0.1 3260
# -IsPersistent $true is the one to use with the service: the bridge exits (and is
# restarted) when its backend goes away, and a persistent session is what makes
# Windows log back in on its own once the bridge is listening again.
Connect-IscsiTarget -NodeAddress iqn.2024-01.com.nvmeof:bridge0 -IsPersistent $true
Get-Disk | Where-Object BusType -eq 'iSCSI'

.\uninstall.ps1
```

| | |
|---|---|
| installed to | `%ProgramFiles%\nvmeof-windows-nd\` (exe + `bridge.conf`, one argument per line) |
| log | `%ProgramData%\nvmeof-windows-nd\bridge.log` |
| service name | `nvmeofNdBridge` (`-ServiceName` to change it) |
| command line | the whole bridge command sits in the service's ImagePath; `sc qc nvmeofNdBridge` shows it |

Three things worth knowing before you deploy it:

- **Tune both sides or the bridge looks slow for reasons that are not its fault.** Windows' initiator
  charges a fixed **~16–20 ms per Data-Out PDU** (write direction), so throughput is bought by
  putting more data behind each one, and both ends must agree or the negotiation takes the smaller
  value:
  1. the initiator's registry — under
     `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E97B-E325-11CE-BFC1-08002BE10318}\0009\Parameters`,
     set `MaxTransferLength=4194304`, `MaxBurstLength=4194304`, `MaxRecvDataSegmentLength=1048576`.
     **These only reload when the device instance is re-enabled**, not on a service restart:
     `Disable-PnpDevice -InstanceId ROOT\ISCSIPRT\0000 -Confirm:$false` then `Enable-PnpDevice …`.
     `src/tune_initiator.ps1` does exactly this — `-Show` to see the current values, `-Apply` to set
     them, `-Restore` to put the originals back. It finds the device instance itself (by driver
     description, not by guessing the `0009` suffix), **keeps a one-time backup of the originals in
     `%ProgramData%\nvmeof-windows-nd\initiator-tuning.json`** that it never overwrites, and
     `install.ps1` calls it for you (`-NoTuneInitiator` to skip). `uninstall.ps1` restores from that
     backup, so an uninstall leaves the machine as it found it;
  2. the bridge's burst sizes: `-iscsimbl 4194304 -iscsichunk 1048576` (`install.ps1` passes both,
     `-MaxBurstLength` / `-MaxSegmentLength`). Measured effect: a 4 MiB read **11.9 → 81.9 MB/s**,
     a 4 MiB write **~900 → 69 ms**, all byte-exact (DESIGN §8.68).
  Note what the second number is *not*: the read direction does **not** get faster with larger
  Data-In PDUs. Measured across 64 KiB … 1 MiB (DESIGN §8.70), the bridge's Data-In time is flat
  (~27–34 ms per 4 MiB) because the cost is per byte, and 1 MiB PDUs are the **slowest** of the set
  (they add 17–25 ms single-PDU window stalls). 256 KiB is the sweet spot and is what Windows offers
  by default. A single session through this initiator tops out at ~70–80 MB/s whatever the shape,
  and the queue depth is **always 1** — the bridge's `-iscsitime` prints the measured high-water
  mark, so "more threads" can be checked rather than assumed (8 concurrent readers measured
  *slower*, 17.3 MB/s against 77.7).
- **The service retries instead of dying when the peer is missing.** `install.ps1` passes
  `-backendretry 15`, so if the NVMe-oF target is not up yet the service stays `Running` and
  connects on its own once it appears, rather than exiting and being restarted by the SCM every
  five seconds. Verified: three failed attempts (each one now also releases the device, queues
  and registered region it had taken), then the peer appeared and the bridge connected without a
  restart.
- **A backend that goes away mid-session ends the bridge, on purpose.** Measured: with the
  NVMe-oF target process gone, every later submit returns `ND_CANCELED`, so the queue pairs are
  dead for good — restarting the target changed nothing and the disk kept failing (`Data error
  (cyclic redundancy check)` on writes, `fatal device hardware error` on reads) while the bridge
  looked healthy. The bridge now counts three consecutive NVMe failures, ends the session and
  **exits non-zero**, which is what makes the SCM restart it into `-backendretry` and reconnect
  (DESIGN §8.66). Combine that with a persistent iSCSI session and the disk comes back by itself.
- **The log is locked while the service runs** (`Get-Content` reports "used by another
  process"). Stop the service to read it, or point `-log` at a path you can copy afterwards.
  This is a recorded limitation, not an oversight — see DESIGN §8.62(2).
- The service runs the **bridge** only. It still needs an NVMe-oF target to talk to: the Linux
  box, or a second `-target` run of the same exe (which is how the service was verified
  end-to-end: install → serve a known pattern → `0 / 65536 bytes differ` → `Stop-Service` →
  clean `exiting (rc 0)`).


## Which binary to use against a real host

| Scenario | Use |
|---|---|
| Linux `nvme-cli` (or any foreign host) talks to us | **`f5_interop.exe -target <ip> <port>`** |
| Our host talks to Linux `nvmet` (or any peer) | `f5_interop.exe -initiator <serverIp> <port> <localIp> [-subnqn <nqn>]` |
| Self-test (both ends ours) | any `run_f*.ps1` |

**F6's target is a lifecycle test double, not an interop target**: its host is strictly
one-request-one-response, so it posts a single Receive and will drop capsules from a real
pipelining host (DESIGN §8.41). F5's target has an 8-slot receive ring and delayed
completions and survives a queue depth of 32.

## Interop with a real Linux host

```powershell
.\f5_session.ps1 -LaptopIp <peer> -LaptopUser <user>         # link → peer → direction A → discovery → direction B
.\f5_session.ps1 -Auth -LaptopIp <peer> -LaptopUser <user>   # same link, direction B with DH-HMAC-CHAP
.\f5_session.ps1 -Down                                       # tear the bridge down, verified
```

Raw output lands in `src/f5_session_<timestamp>.txt`. Turning the manual steps into a script
found **4 more defects** — a missing local IP in direction A, probing the peer address at the
wrong moment, quotes being lost as a command string crosses PowerShell → ssh → bash (that
one aborted mid-sequence and left a live controller on the peer), and three in the direction-B
script itself (DESIGN §8.47).

**Multi-queue (§8.49)**: the target really owns 8 queue pairs, each with its own capsule ring
and in-flight table; the initiator creates as many as the peer grants. Peer-measured:
direction A connects 4/4 queues and runs one command on each simultaneously; direction B has
the Linux host create 8 and spread commands over 6. Self-test F5 uses 4 queues by default.

**Discovery (§8.50)**: `nvme discover` lists us (peer-measured `DISC: the discovery log names
nqn.2024-01.local.rdma:windows-nd`; local target counters `discLogReads=3`, `controllers=2`),
and our own host can read Linux nvmet's discovery log with `-discover`. Discovery and I/O are
**two controllers**, so the target serves continuously with `-serve N` (default 1, preserving
F7's "exit when the host disappears" assertion).

**DH-HMAC-CHAP (§8.51)**: `-authkey <DHHC-1:..>` makes the target require authentication
(Connect result sets ATR bit 17); before authentication every non-fabrics command is answered
`0x4191`. Seeing ATR, our host runs Negotiate → Challenge → Reply → Success1 automatically
(plus Success2 when a controller key is configured). `.\f5_interop.exe -genkey` generates
keys; `.\run_f5_auth.ps1` runs the six cases, including "a wrong key must be refused" and
"ignoring ATR must be gated".

**A namespace you can actually look at (§8.53)**: `-nsfile F:\x.img` turns the namespace into
that file (geometry = file size, write-back on FLUSH), so bytes a Linux host writes are
directly visible on the Windows side:

```powershell
# Windows: the target uses a file as its namespace (-serve 0 = unlimited controllers)
.\f5_interop.exe -target 192.168.100.2 4420 -serve 0 -nsfile F:\nvmeof-ns.img
# Peer (inside the bridge window):
~/f5_dirb_demo.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd
# Then read that file on Windows: the text the host wrote is at offset 4096, and
# LBAs 3000 / 3100 are the zeros produced by write-zeroes and dsm deallocate.
```

In the same round `linux/f5_nvmet_ref*.sh` treated `nvmet` as the specification and measured
reference answers line by line (discovery-log LID policy, Get Features with a data buffer
always `SGL_INVALID_DATA`, Set VWC must be refused, DSM's AD bit in CDW11), which fixed three
differences our own two-ended self-tests could never show — including one that **hangs the
controller**: the zero-length RDMA Write triggered by `nvme persistent-event-log` never
completes, stalling the admin queue until the host's Keep Alive times out 7.6 s later and
drops the link.

## Documentation

| File | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | Design and measurement log; §8 is the defect-by-defect history |
| [INTEROP_F5.md](INTEROP_F5.md) | How to use a separate Linux machine as an independent peer (link script, steps, rollback) |
| [EVIDENCE-1TB.md](EVIDENCE-1TB.md) | Raw evidence for the 1 TB disk on Windows, including the teardown record |
| [DRIVER-D0.md](DRIVER-D0.md) | Preparation for the kernel-mode route (D0/D1): what is already proven about this machine (the WDK is installed, ndkpi.h and storport.h are present), what the probe must prove, how to build and load a driver here, and the risks in the order that decides whether the route exists at all |
| [ROADMAP.md](ROADMAP.md) | What is still missing for a complete NVMe-oF, as a work plan: per-item scope, dependencies, effort estimate and — for every item — an acceptance criterion, plus milestones, the critical path and the decisions that change the ordering |
| [COMMERCIAL.md](COMMERCIAL.md) | Commercial licensing (Apache-2.0 dual licensing) and license-compatibility notes |

These are written in Chinese; `README.md` (this file) is the English entry point.

## Hardware

- A Windows machine with a RoCE-capable NIC. Measured here: ConnectX-3 Pro (HP 544+FLR-QSFP,
  firmware 2.40.5000) with the WinOF ND provider.
- Self-testing needs only that card's two ports cabled to each other.
- Interop needs a **local** Linux peer — an ordinary machine with software RoCE (`rxe`) is
  enough. A cloud server will not work: RoCE does not cross routers (see `INTEROP_F5.md`).

## License

**Apache License 2.0** ([LICENSE](LICENSE)) - the verbatim official text, with the
copyright in [NOTICE](NOTICE).

Apache 2.0 rather than a copyleft licence, and deliberately so. This is a Windows driver
and protocol stack: it leans on WDK and NDIS sample material, on the NVMe and RDMA
specifications' reference code, and on the surrounding ecosystem's conventions, all of
which sit under Apache 2.0 or a compatible permissive licence. Its **explicit patent
grant** is the clause that actually matters for an implementation of somebody else's
protocol, and it permits commercial and closed-source use without a separate agreement.

Material that is read for reference but never compiled or linked is listed in
[THIRD-PARTY.md](THIRD-PARTY.md) with its own licence. Where that material is
**GPL-2.0-only it is incompatible** with what this repository ships and stays
reference-only.

   copyleft license.
