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
| 6 | Throughput vs a raw RDMA baseline | ✅ ~1.17 GB/s ≈ **48%** of a 2.53 GB/s baseline (DESIGN §8.16) |
| 7 | **Our target ↔ Linux `nvme-cli`** | ✅ `nvme connect` rc=0, `nvme list` shows `NDVMEOF0000000000001`, 128 blocks written → flush → read back, `cmp` byte-identical; the host builds **8 I/O queues** and spreads commands over **6** of them (DESIGN §8.46, §8.49) |
| 8 | **In-band DH-HMAC-CHAP authentication** | ✅ Both directions against a real Linux peer: a Linux host authenticates to our target with `nvme connect -S <key>` (kernel log `qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048`; one-way and **bidirectional** both move data; a wrong key is refused; `authRefused=0`), and our host authenticates to an **authentication-requiring `nvmet`** (`Success2 sent (the controller's own response verified)`) (DESIGN §8.51, §8.52) |
| 9 | **A 1 TB real disk on Windows as an active drive** | ✅ Linux `/dev/nvme1n1` (CT1000P3PSSD8, 1953525168 × 512 B = 931.51 GiB) appears as `Get-Disk` #3, **Online**, GPT, NTFS `E:` readable; two byte-exact cross-checks against the Linux side; **zero writes** (DESIGN §8.55, §8.56, [EVIDENCE-1TB.md](EVIDENCE-1TB.md)) |

Items 5 and 7 are the only tests that can catch **both ends being wrong together** — and on
their first real run they found **11 defects**, 7 of them cases where our two ends shared a
misreading of the same field, while 8/8 self-tests had been green. **Self-consistency is not
correctness.**

The whole suite is one command — 10 of 10 suites, ~220 s:

```powershell
cd <repo>\src
.\run_all.ps1
```

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
  driver at all.

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
| `src/nvmeof_iscsi.h` | The user-mode iSCSI target (one object and one thread per connection; the backend is serialized behind a mutex because it is a single queue pair) |
| `src/f1_bringup.cpp` | F1 — connection and Identify |
| `src/f3_io.cpp` | F3 — read/write correctness and error paths (including the `0x4f` invalidate sub-type) |
| `src/f4_pipeline.cpp` | F4 — pipelined throughput (8 in flight) |
| `src/f5_interop.cpp` | **F5 interop**: `-initiator` against any peer, `-target` for real hosts (32-slot receive ring, sized to cover the window it advertises), `-iscsi` for the iSCSI bridge, `-nsfile` / `-serve` / `-genkey` and friends |
| `src/f6_lifecycle.cpp` | F6 — full host sequence plus target-side lifecycle (31 assertions) |
| `src/f7_faults.cpp` | F7 — fault injection (peer vanishes, and the reverse) |
| `src/wire_selftest.c` | Byte-level golden self-test, compiled as both C and C++ |
| `src/xref_constants.py` | Cross-checks every constant against the two Linux reference headers (97 pairs, 21 of them DH-HMAC-CHAP) |
| `src/run_all.ps1` | Runs all 10 suites and prints a verdict table (~220 s) |
| `src/run_f5_auth.ps1` | The six DH-HMAC-CHAP cases (two local ports, no Linux needed) |
| `src/interop_link.ps1` | Interop link: bridge + address/route move + MTU lowering, with automatic rollback |
| `src/f5_session.ps1` | **Whole interop session** in one command: link → peer → direction A → discovery → direction B → report file; `-Auth` adds authentication |
| `src/mount_nvmeof.ps1` | Namespace → fixed VHD → drive letter → push back on unmount |
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

All 10 suites in order, mutually isolated, ~6 minutes:

```powershell
cd <repo>\src
.\run_all.ps1
```

Individually: `run_xref.ps1`, `run_wire.ps1`, `run_f1.ps1`, `run_f3.ps1`, `run_f4.ps1`,
`run_f5.ps1`, `run_f5_auth.ps1`, `run_f6.ps1`, `run_f7.ps1`, `run_stag.ps1`.

Every suite follows the same rule: **delete the old exe → check the compiler's exit code →
compare source and header timestamps.** A binary that merely *looks* current is never run —
that rule came from a real incident (DESIGN §8.6).

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
| [COMMERCIAL.md](COMMERCIAL.md) | Commercial licensing (AGPL dual licensing) and license-compatibility notes |

These are written in Chinese; `README.md` (this file) is the English entry point.

## Hardware

- A Windows machine with a RoCE-capable NIC. Measured here: ConnectX-3 Pro (HP 544+FLR-QSFP,
  firmware 2.40.5000) with the WinOF ND provider.
- Self-testing needs only that card's two ports cabled to each other.
- Interop needs a **local** Linux peer — an ordinary machine with software RoCE (`rxe`) is
  enough. A cloud server will not work: RoCE does not cross routers (see `INTEROP_F5.md`).

## License

**GNU Affero General Public License v3.0 or later** ([LICENSE](LICENSE)) — the verbatim
official text, 34,523 bytes, sha256
`8486a10c4393cee1c25392769ddd3b2d6c242d6ec7928e1414efff7dfb2f07ef`.

```
Copyright (C) 2026 Dingtaiqi

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
```

**AGPL permits commercial use**; what it regulates is *closed source*:

- Using it inside a company, making money with it, running it as a service — all allowed,
  free of charge.
- The price is giving back: if you distribute this project or a derivative work — **including
  offering it over a network** — you must provide the complete corresponding source.
  Section 13 (`LICENSE` L540) is the network clause, and it is the one substantive difference
  from the GPL: it is what stops "take open code, run a closed cloud service".
- A company that genuinely needs to stay closed (embedding it in a proprietary product, or a
  closed-source SaaS) can take a commercial license instead — see [COMMERCIAL.md](COMMERCIAL.md).

Two compatibility traps:

1. The two Linux kernel headers under `ref/` are **GPL-2.0** and are read for comparison
   only; they are never compiled. **GPL-2.0-only and AGPL-3.0 are incompatible** — do not
   merge their code into this project. If you truly must, the whole project would have to
   become GPL-2.0.
2. Linking the vendor NetworkDirect library (`ndutil`/NDSPI) changes nothing: it is not a
   copyleft license.
