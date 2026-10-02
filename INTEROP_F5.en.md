# F5 — interop with an independent peer (Linux)

> ## ✅ It already runs end to end (measured this round, 2026-10-01)
>
> The peer is an EndeavourOS laptop, **an ordinary gigabit NIC + software RoCE (rxe)**,
> sharing one L2 with this machine's CX3 port through "LAN NIC + CX3 port A bridged together".
>
> | Direction | Command | Result |
> |---|---|---|
> | A our host → Linux `nvmet` | `.\run_f5.ps1 -initiatorOnly -subnqn nqn.2024-01.local.rdma:linux-nvmet -serverIp 192.168.100.5 -clientLocalIp 192.168.100.2` | **`initiator failures: 0`** (36 assertions; including 32 in-flight write/read pipelines at 32/32 each, 4096 B byte-identical; "Number of Queues asked 4, granted 128, 4/4 actually used") |
> | B Linux `nvme-cli` → our target | `f5_interop.exe -target 192.168.100.2 4420` + the laptop's `nvme connect` | `rc=0`; `/dev/nvme2n1 NDVMEOF0000000000001` appears; 128 blocks written → flush → read back, **`cmp` byte-identical** |
> | B+ **DH-HMAC-CHAP**: a Linux host authenticates to our target | `f5_interop.exe -target 192.168.100.2 4420 -authkey <DHHC-1:..>` + the laptop's `nvme connect -S <key>` | `RESULT: PASS`; kernel log `qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048`; one-way + **bidirectional** (`-C <ctrl-key>`) both run through, 65536 B byte-identical; **a wrong key is refused** |
>
> **Three preconditions you have to remember** (all of them were learned the hard way, see DESIGN §8.46 / §8.51):
> 1. The local RoCE port must be lowered to **RoCE Max Frame 1024 / IP MTU 1500** (`interop_link.ps1 -Action LowMtu`).
>    Without lowering it, a 1024 B one-way read passes, but **the 4096 B Identify data never arrives**, and neither end reports an error.
> 2. This driver's rkey is **byte-reversed** on the wire; the compensation is already built into `ndWireKey()` in `nvmeof_rdma.h`;
>    do not swap it by hand at any call site.
> 3. **After** the bridge window ends, `以太网 24` must be restored to **IP MTU 4074 / Jumbo 4088 / RoCE Max 2048**,
>    otherwise the suites that run this machine's two directly cabled ports will `ND_IO_TIMEOUT` on 4096-byte transfers. `-Down` restores from the record,
>    but that record was itself once written by `LowMtu` as the "lowered value" — that defect is fixed (§8.51),
>    but **after finishing you still have to look at the measured value**: `Get-NetIPInterface -InterfaceAlias '以太网 24'` should be 4074.
>
> The whole session in one command: `.\f5_session.ps1 -LaptopIp 192.168.1.9 -LaptopUser yinshibai`,
> the whole session with authentication: `.\f5_session.ps1 -Auth -LaptopIp 192.168.1.9 -LaptopUser yinshibai` (see §3).

This document is **for the future me / for you to follow**: acceptance items 5 and 7 need a Linux box,
and this machine cannot start one right now (SVM is off in the BIOS, so WSL2 and every VM refuse to boot). So everything
needed to "finish the run the moment a Linux machine appears" is prepared up front here:

| File | What it does |
|---|---|
| `src/f5_interop.cpp` | one binary, two roles: `-initiator` (our host against any peer), `-target` (our target for a Linux host to hit) |
| `src/run_f5.ps1` | build + self-test + the entry point for hitting Linux (`-initiatorOnly`) |
| `linux/nvmet_setup.sh` | set up nvmet on Linux (configfs, everything reversible); **with no RDMA NIC, use `RXE_NETDEV=<nic> ` to go through software RoCE** |
| `linux/nvmet_teardown.sh` | tear it down |
| `linux/probe_mtu.sh`, `linux/rxe_counts.sh` | read-only diagnostics: the peer port's MTU/capabilities; the before/after delta of the rxe counters |
| `ref/linux_nvme_rdma.h` | the **reference copy** of the RDMA-CM private data; `run_xref.ps1` compares it value by value |

## 0. First get the laptop onto the CX3's L2 (this machine's measured topology)

The state of this machine (`Get-NetAdapter` / `Get-NetIPAddress`, measured):

| Adapter | What it is | Address | RDMA |
|---|---|---|---|
| 以太网 20 | Intel I211 gigabit | 192.168.1.2/24, gateway 192.168.1.1 | No |
| 以太网 23 | CX3 port A | 192.168.100.3/24 | Yes |
| 以太网 24 | CX3 port B | 192.168.100.2/24 | **Yes ← kept for the Windows RoCE endpoint** |

There is a direct cable between port A ↔ port B. The goal: put the laptop and **192.168.100.2** on the same L2.
The way to do it is to bridge "以太网 20" and "以太网 23" together — **without touching 以太网 24, which is reserved for RoCE**.

> ⚠️ On this machine `以太网 20` has a **manual address and no gateway written** (DHCP off): IPv6 keeps
> getting out through the router's RA, but IPv4 has no default route, so "the machine has internet, the wired port does not",
> and the whole thing is held up by phone USB tethering (`以太网 25`, 10.246.250.74). The fix and how to verify it are in DESIGN §8.48:
> add a **backup** route with `New-NetRoute ... -NextHop 192.168.1.1 -RouteMetric 50`
> (still preferred while the phone is there, taking over the moment it is unplugged). Read this table back **per interface**
> both before and after the bridging experiment; do not only look at "can it get online".

### Windows side (GUI, the only reliable way)

1. `Win+R` → `ncpa.cpl`
2. Hold Ctrl and select **以太网 20** and **以太网 23** → right-click → **Bridge**
3. The bridge takes an IP for itself. **Write down the current state first** before touching anything:

```powershell
Get-NetAdapter | Format-Table ifIndex, Name, Status, LinkSpeed
Get-NetIPAddress -AddressFamily IPv4 | Format-Table InterfaceAlias, IPAddress, PrefixLength
Get-NetRoute -DestinationPrefix 0.0.0.0/0 | Format-Table InterfaceAlias, NextHop
```

4. Once the bridge is up, put the LAN address back onto the bridge (otherwise this machine drops off the network):

```powershell
New-NetIPAddress -InterfaceAlias "网桥" -IPAddress 192.168.1.2 -PrefixLength 24
Set-NetIPInterface -InterfaceAlias "网桥" -Dhcp Disabled
New-NetRoute -InterfaceAlias "网桥" -DestinationPrefix 0.0.0.0/0 -NextHop 192.168.1.1
```

5. Confirm the firewall has been opened for the two RoCE ports (see §1), and **do not** bridge 以太网 24 into it.

### Rollback

`ncpa.cpl` → right-click "网桥" → **Delete**; then confirm 以太网 20 gets `192.168.1.2/24` and the default route back
(the values written down in the previous step). If 以太网 23/24 lost their addresses, add them again:

```powershell
New-NetIPAddress -InterfaceAlias "以太网 23" -IPAddress 192.168.100.3 -PrefixLength 24
New-NetIPAddress -InterfaceAlias "以太网 24" -IPAddress 192.168.100.2 -PrefixLength 24
```

> Note: **do not use `New-NetSwitchTeam`** — that is NIC teaming, not bridging;
> I got it wrong in the previous version of this document, corrected here.

### Laptop side (Arch)

```sh
pacman -S --needed iproute2 rdma-core nvme-cli
# wired port name (enp*/eth*), it must be under the same router/switch as "以太网 20"
ip -brief link

# give the laptop an address in the same subnet as the RoCE port: no gateway change needed, just one more IP
sudo ip addr add 192.168.100.5/24 dev enp3s0
ping -c2 192.168.100.2          # must work; if it does not, do not go any further

# software RoCE
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev enp3s0
rdma link show
```

Then run `nvmet_setup.sh` as in §1 (`RXE_NETDEV=enp3s0 ./nvmet_setup.sh 192.168.100.5`).

**Why the laptop also needs a `192.168.100.x` address**: rxe resolves the peer directly through ARP,
so if the laptop only has `192.168.1.x`, packets to `192.168.100.2` are sent to the gateway (192.168.1.1),
and the router has no route to `192.168.100.0/24` — the packets are simply gone.
Add a secondary address in the same subnet and Linux will take the direct route and find the peer in this L2's ARP.

**The laptop must be on a cable**: rxe over wifi is not a standard configuration (the AP's client isolation,
multicast handling and latency all make the result untrustworthy), and it turns into "what is being measured is the wireless NIC, not our protocol".



---

### Why this step is unavoidable (and why the self-test is not enough)

Every other suite in this project is **our host hitting our target**.
That is the weakest combination: **if both ends are wrong in the same way you will never see it**,
and this project has already shipped 4 constants that only we ourselves accept because of it (DESIGN §8.29).

What could be done before F5, and was done: read Linux's `nvme_rdma` / `nvmet_rdma` /
`core.c` / `nvme-rdma.h` **as a reference implementation**, and then use their rules to check us in turn.
That step caught three real defects (DESIGN §8.40), but **reading source code is not the same as running it**:
F5 is still the only thing that can prove "it interops".

---

## 1. Direction A: our initiator ↔ Linux `nvmet` (acceptance item 5)

### First solve "how do the two machines get into the same subnet"

rxe (software RoCE) uses **UDP 4791** just like real RoCE, and it **requires the peer to be in the same subnet with ARP able to resolve it directly** —
a gateway/router in between will not do. So what is needed is not "a direct cable", but "one L2". Three ways to connect it:

| Way to connect | What it needs | Notes |
|---|---|---|
| **CX3 port 2 ↔ the Linux server's SFP+/10G port** (recommended) | one QSFP→SFP+ cable or module | The CX3's two ports are exactly one endpoint each, closest to the real scenario |
| CX3 port 2 ↔ a switch, with the Linux server on that switch too | a matching port on the switch (or a QSFP-to-RJ45 module) | Still requires both machines to be in the same subnet |
| CX3 port 2 **bridged** with the Windows motherboard NIC | no new hardware | Do "Bridge Connections" in Windows and Linux shares the segment over an ordinary LAN; it changes this machine's network configuration, rollback steps in §0 |

**Port 1 is kept for the Windows side** (now `192.168.100.2`), port 2 goes to Linux.

### Linux side

```sh
# run as root; <linux-ip> must be in the same subnet as the Windows CX3 port (e.g. 192.168.100.2 / .3)
chmod +x nvmet_setup.sh nvmet_teardown.sh

# with an RDMA NIC:
./nvmet_setup.sh 192.168.100.3

# with no RDMA NIC — use software RoCE (rxe), any ordinary NIC will do:
RXE_NETDEV=ens18 ./nvmet_setup.sh 192.168.100.3
```

The script will: load `nvmet`/`nvmet-rdma` (in rxe mode it also loads `rdma_rxe` and does `rdma link add rxe0`),
create a ramdisk namespace, open an rdma port on `<ip>:4420`,
**open UDP 4791 in the firewall**, and print the commands the Windows side should run.

> **How strong rxe is**: rxe is a second, complete RoCEv2 implementation inside the kernel, interop with hardware RoCE is supported,
> and as an **independent peer** it is entirely sufficient (a different implementation of the protocol layer). It **cannot** reproduce the timing and throughput of hardware,
> so the numbers on this path count as **correctness** evidence only, never as performance evidence.

### Windows side

Do this once first (RoCEv2 is UDP 4791, and the symptom of inbound traffic being blocked is exactly the same as "the peer does not respond"):

```powershell
New-NetFirewallRule -DisplayName 'NVMe-oF RoCEv2' -Direction Inbound `
    -Protocol UDP -LocalPort 4791 -Action Allow
Test-NetConnection 192.168.100.5 -InformationLevel Quiet   # must be True: same L2, no gateway in between
```

Then (**note that `-clientLocalIp` must carry this machine's RoCE endpoint address**; the default value `.3` is port A,
and while the bridge is up port A has no IP, so getting it wrong shows up as `NdOpenAdapter failed 0xC0000141`):

```powershell
cd D:\rdma\nvmeof\src
.\run_f5.ps1 -initiatorOnly -subnqn nqn.2024-01.local.rdma:linux-nvmet `
             -serverIp 192.168.100.5 -port 4420 -clientLocalIp 192.168.100.2
```

### What you should see

```
[PASS] admin RDMA-CM connection to the target
[PASS] fabrics Connect qid 0 (the target's NQN was found) - cntlid=1   <- the very first command is Connect
[PASS] Property Get CAP - CAP=0x8200F00007F MQES=127 TO=15             <- the target's own CAP
[PASS] Property Set CC (entry sizes, still disabled)
[cap]  CRMS.CRWMS=1 CRMS.CRIMS=0 -> CRTO is not read (as the reference host)
[PASS] Property Set CC.EN=1
[PASS] controller reports ready (CSTS.RDY)
[PASS] Property Get VS - VS=0x00020100
[PASS] inbound RDMA Write, 1024 B (Get Log Page LID 1)
[PASS] inbound RDMA Write, 4096 B (Get Log Page LID 5 effects)        <- one-way write: it dies here when the MTU does not match
[PASS] probe: an Identify with an unknown CNS is answered (no data transfer)
[PASS] Identify Controller            <- vid/ssvid/sn/mn/fr/subnqn are all Linux's values
[PASS] the target passes the host's fabrics checklist
[PASS] the target advertises the keyed SGLs this host sends
[PASS] Identify CNS 2 (active namespace list)
[PASS] Identify CNS 0 (namespace geometry)   <- the block size is decided by the peer (512 on a ramdisk)
[PASS] Set Features: Number of Queues
[PASS] Set Features: Keep Alive Timer is accepted
[PASS] Keep Alive
[PASS] four accepted Async Events are held, not answered
[PASS] the fifth outstanding Async Event is refused with ASYNC_LIMIT
[PASS] I/O queue pair connected
[PASS] fabrics Connect qid 1 on its own queue pair
[PASS] WRITE accepted
[PASS] READ returned exactly the bytes WRITE sent
[PASS] FLUSH is answered
[PASS] an out-of-range LBA is refused with a status, not served
[PASS] a pipelined I/O burst is answered in full (target receive ring)  <- 32/32
[PASS] a pipelined read burst returns every command's own data
[PASS] an fctype that does not exist (0x08) is refused, not answered
[PASS] Property Set CC.EN=0 disables the controller (how a host really leaves)
initiator failures: 0
```

> Note how "leaving" works: **there is no such command as a fabrics Disconnect** (§8.44).
> A real host finishes with `Property Set CC.EN=0` and then drops the queue pairs.

On the Linux side `dmesg` will meanwhile print things like `nvmet_rdma: enabling port 1 (...)`;
when the controller is created it prints `nvmet: Created nvm controller 1 for subsystem ...`.

### Where to look when something goes wrong

| Symptom | Look first at |
|---|---|
| Connect is refused outright | the `nvmet_rdma: rejecting connect request: status N (xxx)` in `dmesg`; N corresponds to `NVME_RDMA_CM_*` in `ref/linux_nvme_rdma.h` (1=no private data, 2=recfmt, 4=hsqsize, 5=hrqsize) |
| **Every** command answers `sct=0 sc=0x02` | **byte 1 of the SQE is not `0x40`** (nvmet checks PSDT before it parses anything, §8.46(1)) |
| It fails right at the step from connect to CAP | command order: Connect must be the first command (§8.46(2)) |
| `nvmet_rdma: invalid SGL subtype: 0x0` | a no-data command must still carry a **zero-length keyed dptr** (§8.46(3)) |
| A successful Connect is judged a failure | **bit 0 of the status word is not the success bit**; Linux reads `status >> 1` (§8.46(4)) |
| Identify gets no response and the buffer is all zeros | **MTU mismatch**: the local end must be lowered to RoCE Max Frame 1024 (§8.46(7)) |
| One-way RDMA is always `remote access error` / `ND_ACCESS_VIOLATION` | whether the rkey byte-order compensation is being bypassed (`ndWireKey`, §8.46(6)) |
| The data is wrong but success is reported | **the most memorable class of all**: it means one side misread the SGL address/length/key |
| `NdOpenAdapter 0xC0000141` | the local IP points at **the port that was bridged** (its TCP/IP has been handed to the bridge): direction A must be given `-clientLocalIp <the port reserved for RoCE>` explicitly (§8.47(13)) |
| The script says "the two ends are not on the same L2", but the peer clearly is | the probe of `192.168.100.x` happens earlier than the step where `f5_linux_up.sh` adds the address (§8.47(14)) |
| `bash: -c: line N: syntax error near unexpected token '('` | the **double quotes were lost** while the remote command string crossed PowerShell→ssh→bash: write the sequence as a `.sh` file and copy it over (§8.47(15)) |
| `rm: cannot remove '/tmp/...': Operation not permitted` | that file was created by the previous `sudo nvme read -d` and belongs to root; data files belong in the private directory made by `mktemp -d` (§8.47(16)) |

---

## 2. Direction B: Linux `nvme-cli` ↔ our target (acceptance item 7)

### Windows side

```powershell
D:\rdma\nvmeof\src\f5_interop.exe -target 192.168.100.2 4420
```

It prints the subnqn it supports (`nqn.2024-01.local.rdma:windows-nd`),
and **records, command by command, what the foreign host sent** — that is this step's output.

### Linux side

> ⛔ **Never read or write `/dev/nvme0n1`, that is, "the first nvme device".**
> On the laptop that was measured, `/dev/nvme0n1` is **its own 512 GB system disk**.
> The sequence below finds our device **by serial number** (the serial starts with `NDVMEOF`),
> and if it cannot find it, it errors out and touches nothing.

**Recommended: run `linux/f5_dirb_check.sh` from the repository** (`f5_session.ps1`'s step 4 is exactly this: scp it
to the peer's home directory and execute it there; `~/nvmeof/linux` is owned by root, so an ordinary user cannot write into it):

```sh
scp -i ~/.ssh/nvmeof_f5_ed25519 linux/f5_dirb_check.sh yinshibai@<laptop>:/home/yinshibai/
ssh yinshibai@<laptop> "sh ~/f5_dirb_check.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd"
```

Internally it is exactly the sequence below, and exit code 0 means only that **65536 bytes went out, were flushed, and came back byte-identical**:

```sh
sudo nvme disconnect -n nqn.2024-01.local.rdma:windows-nd 2>/dev/null || true   # clear leftovers from last time
sudo nvme connect -t rdma -a 192.168.100.2 -s 4420 \
                  -n nqn.2024-01.local.rdma:windows-nd
sleep 1
sudo nvme list                        # a 32 MiB NDVMEOF... namespace should appear

DEV=$(sudo nvme list | awk '/NDVMEOF/ {print $1}' | head -1)
[ -n "$DEV" ] || { echo 'FAIL: no NDVMEOF device - refusing to touch anything else'; exit 1; }
echo "using $DEV"

sudo nvme id-ctrl "$DEV" | head -20    # compare field by field against our Identify

# data path: write → flush → read back → compare byte by byte. Use nvme-cli's own write/read,
# not dd — the dd route needs root to open the block device directly, and on this route we only want the commands themselves.
dd if=/dev/urandom of=/tmp/f5pat bs=4096 count=16 status=none
sudo nvme write "$DEV" -s 0 -c 127 -b 512 -z 65536 -d /tmp/f5pat
sudo nvme flush "$DEV"
rm -f /tmp/f5back
sudo nvme read  "$DEV" -s 0 -c 127 -b 512 -z 65536 -d /tmp/f5back
cmp /tmp/f5pat /tmp/f5back && echo 'CMP: identical (65536 bytes written, flushed, read back)'

sudo nvme disconnect -n nqn.2024-01.local.rdma:windows-nd
```

> **Why a whole `.sh` file rather than a single ssh command string**: the string has to cross
> PowerShell → `ssh.exe` → bash, three layers, and **the double quotes get lost on the way**, so the first pair of parentheses becomes
> `bash: -c: line 12: syntax error near unexpected token '('` — in the measured run this one
> died right in the middle of the sequence and left a live controller behind on the peer. A file has no quotes to lose.

> Every `nvme` command needs root (otherwise it is `Failed to open /dev/nvme-fabrics: Permission denied`),
> so use `sudo -n` to avoid getting stuck on a password prompt nobody can answer. **The script itself does not need root**,
> unless `/etc/sudoers.d/` happens to list its absolute path.

**Length units are the easiest thing to get wrong on this route**: `-c` is a count of **device logical blocks** (0-based),
and this device presents itself as **512 B blocks**, so 65536 B means writing `-c 127 -b 512`.
Writing `-c 15 -b 4096` transfers only 16×512 = **8192 B**, and then `cmp` starts reporting
"differ" at byte 8193 — that is not the data path broken, it is the command having asked for only 8 KiB.

Note: **`nvme connect` uses the rxe device too**. On the Linux side `rdma link show` must show rxe0 for
`nvme connect` to bind to that address. If Linux uses rxe and Windows uses real RoCE,
this direction holds just the same (both ends are RoCEv2).

### What you should see

In the Windows-side target log (**the order is the real Linux host's order**: Connect comes first):

```
[conn] private data: ... recfmt=0 qid=0 hrqsize=32 hsqsize=31
[conn] accepted; advertised crqsize=32
[admin] fabrics Connect qid=0 sqsize=31 kato=5000
      connect data: RDMA Read posted (addr=0x.. key=0x.. len=1024)
[data]  completion for the transfer: status=0x00000000 ND_SUCCESS bytes=1024
      hostnqn='nqn.2014-08.org.nvmexpress:uuid:...'   <- the host read it itself; being able to print it proves the one-way read worked
[admin] Property Get off=0x0 -> ...      <- CAP; the host asks twice in a row
[admin] Property Set off=0x14 val=0x460000   <- CC: IOSQES=6 / IOCQES=4
[admin] Property Set off=0x14 val=0x460001   <- CC.EN=1
[admin] Property Get off=0x1C -> 0x1     <- CSTS.RDY
[admin] Property Get off=0x8 -> 0x10400  <- VS
[admin] Identify cns=1 nsid=0
[admin] Get Log Page lid=2 numd=127 -> 512 bytes (zero-filled)
[admin] Set Number of Queues -> 1        <- we promise only 1 I/O queue, so we must create exactly 1 pair
[conn] I/O queue pair connected (one at a time)
[io]    fabrics Connect on the I/O queue (fctype=0x01 qid=1)
[io]    cmd 1 WRITE ... sgl(... INVALIDATE)   <- see below
[admin] Keep Alive #1 (kato=5000 ms)          <- kato comes from the Connect command, not from Set Features
...
target summary: ... unknownAdmin=0 unknownIo=0
```

**`unknownAdmin=0 unknownIo=0` is the core metric of this step**:
it means "every command the foreign host sent, this target recognizes".
Non-zero means there is a real command we have not implemented, and the log will carry `UNHANDLED opcode=0x..`.

**Three more are also criteria** (this round went wrong in exactly these three places, §8.46):

1. `Keep Alive #N` must appear. KATO comes from the **Connect command** (milliseconds);
   if the target does not store it, `katoMs` is 0, and with KATO=0 **the specification requires** answering every Keep Alive
   with `KA_TIMEOUT_INVALID` — the host then considers keep-alive failed, enters error recovery,
   and **freezes the I/O queues**, with the symptom "a pile of read commands never complete".
2. After `[io] fabrics Connect on the I/O queue` there must be normal I/O; if the host tears the connection down
   immediately after building the I/O queue, check whether the I/O queue's Accept **carried CM reply private data** (§8.46(8)).
3. The N in `Set Number of Queues -> N` must be **a number we can really serve** (it is 1 now):
   the number returned to the host is a contract; answer 8 and it will create 8 and fail on the second (§8.46(9)).

### Two things already known that also have to be confirmed in the log

1. **The INVALIDATE sub-type**: a Linux host by default marks **every read and write** with `0x4f`
   (keyed + invalidate, see `nvme_rdma_map_sg_fr`). Our target now
   **transfers accordingly + answers with an ordinary SEND**, because the Linux host's completion path
   **invalidates its key locally by itself** when no invalidation arrives. So the log should show `INVALIDATE`
   and the command succeeding — if you see a rejection such as `SGL_INVALID_TYPE`, that is a regression.
2. **Flush**: we advertise `vwc=0`, so on the normal path the host will not send Flush;
   if it really does send one it must still succeed (the `flushes=` counter).
3. **`read=` being larger than 65536 is normal**: a real host reads the namespace to probe its geometry,
   so `written=65536` (whatever we wrote is what it is) can be asserted equal, while `read` can only be asserted
   `>= 65536`. The first version wrote `read == 65536` and falsely reported "the data did not come back" (§8.47).
4. **A controller left behind by an unfinished run reconnects by itself**: connecting the same NQN again with `nvme connect`
   then makes the kernel answer `could not add new controller: failed to write to nvme-fabrics device`,
   while what really shakes hands is the old controller's reconnect — it looks like "our target refuses the new connection",
   but in fact the peer's state is dirty. `linux/f5_dirb_check.sh` disconnects first and **waits until the device really disappears**
   before connecting (§8.47(16)). To check by hand: `sudo nvme list | grep NDVMEOF` should be empty.

### Direction B with DH-HMAC-CHAP (§8.51)

One more layer, in-band authentication, on the same link. **The credential is that `DHHC-1:..` string**, and both ends must have the same string:

```powershell
# Windows side: generate keys. Line 1 = host key, line 2 = controller key (for bidirectional), line 3 = a "wrong" key
.\f5_interop.exe -genkey        # run it a few times, or just use the three-line format of f5_authkey.txt
# make our target require authentication
.\f5_interop.exe -target 192.168.100.2 4420 -serve 3 `
    -authkey <host-key> -authctrlkey <ctrl-key>
```

```sh
# Linux side (nvme-cli 3.x: -S / -C; 2.x is --dhchap-secret / --dhchap-ctrl-secret)
sudo nvme connect -t rdma -a 192.168.100.2 -s 4420 -n nqn.2024-01.local.rdma:windows-nd \
     -S "$HOSTKEY" --nr-io-queues=8
sudo nvme connect -t rdma -a 192.168.100.2 -s 4420 -n nqn.2024-01.local.rdma:windows-nd \
     -S "$HOSTKEY" -C "$CTRLKEY"          # bidirectional: the host also requires the target to prove itself
```

One command runs the whole thing (upload the scripts + key files, four cases, including "a wrong key must be refused"):

```powershell
.\f5_session.ps1 -Auth -Direction b -LaptopIp 192.168.1.9 -LaptopUser yinshibai
```

**Criteria (all of them are required)**:

* The Linux kernel log shows `qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048`
  — these are **the peer's own words**, not our log.
* `nvme list` shows an `NDVMEOF...` device, and write→flush→read-back is byte-identical
  (authenticated but with a dead data path is a different problem, and "it connected" is not enough).
* Connecting with **another valid key** must fail (we are Failure1 + `resctrl_exp=0x01`).
  An authentication implementation tested only on the success path is not tested at all: a target that never verifies the response also makes the first two bullets all green.
* On the target side `authSends/authReceives` are each ≥2 and `authRefused=0` (an authenticated controller should not have any command
  refused for "not authenticated"), and `authFatal=` equals exactly the number of **deliberate failures**.

### Reverse authentication: our host → an authentication-requiring nvmet (done, §8.52)

nvmet's authentication can only be written through configfs, and the passwordless whitelist on the laptop is **one exact path per entry**, so this step was
initially stuck at "a new version of `nvmet_setup.sh` has to be installed at a whitelisted path, and the install needs one password".
Kernel support was never the problem (`CONFIG_NVME_TARGET_AUTH=y`). **It is now installed and running**, and it is kept here because
"who has to do it by hand, and why" is the shape to copy the next time a privileged step is added:

```sh
# one-off (needs a password): install the version with AUTH_KEY_FILE support at the whitelisted path
sudo install -m 755 -o root -g root ~/nvmet_setup.sh.new ~/nvmeof/linux/nvmet_setup.sh
```

Once installed, everything is passwordless:

```sh
# rebuild the authentication-enabled target (line 1 of the key file is the host key, line 2 the controller key)
# note: a reboot clears /tmp and loses the address added by ip addr add and rxe0 — peer_auth_up.sh does both
sh ~/peer_auth_up.sh
```

```powershell
# Windows side: our host authenticates with the same key
#   the key is read from a file, never from the command line (it must not appear in the process table or in history)
scp <peer-key-file> .\peer_authkey.txt
.\run_f5.ps1 -initiatorOnly -subnqn nqn.2024-01.local.rdma:linux-nvmet `
    -serverIp 192.168.100.5 -clientLocalIp 192.168.100.2 -AuthKeyFile .\peer_authkey.txt
```

Expected (this is what was measured):

```
[auth] the Connect result asks for authentication (ATR set, result=0x20001)
[auth] Challenge: hash=0x01 dhgroup=1 seqnum=...
[PASS] the target accepted the host's response (Success1)
[PASS] Success2 sent (the controller's own response verified)     <- bidirectional: we verified the peer's R2
initiator failures: 0
```

The peer's kernel keeps its own copy as well: `nvmet: Created nvm controller 1 … with DH-HMAC-CHAP.`

### Direction B's admin scan and controller reset (§8.52)

`linux/f5_dirb_admin.sh` is direction B's second script: 16 `nvme-cli` admin commands + an optional
`nvme reset`. **The criterion is the target's own `unknownAdmin` counter, not the host's return code** —
the host's rc=1 may just be nvme-cli refusing on its own (`effects-log` wants `-c <csi>`, `set-feature`'s value is `-V`),
and such a "failure" never reached the peer at all.

```sh
sh ~/f5_dirb_admin.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd        # without the reset
sh ~/f5_dirb_admin.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd -r     # with the reset
```

`-r` is **deliberately opt-in**: if the target is absent during a controller reset, the host hangs until Ctrl Loss Timeout
and leaves one task in D state, along with nvme-core's controller lock — measured once on this machine, when
`nvme list`/`nvme disconnect` hung together and only a reboot fixed it. The target side must be configured with `-reconnectwait 180`.

### discovery: first ask "what is here", then decide what to connect to (§8.50)

The first step of a real deployment is `nvme discover`; it is **another controller** (with its own admin queue pair),
so **the target must be started with `-serve 2`**: the discovery connection disappears after reading the log page, and only then does the I/O one
arrive. The default `-serve 1` exists to preserve F7's "exit when the host disappears" assertion.

```sh
# Linux side: list what our target offers
sudo nvme discover -t rdma -a 192.168.100.2 -s 4420
```

Expected (the way our own run looks):

```
Discovery Log Number of Records 2, Generation counter 1
=====Discovery Log Entry 0======
trtype:  rdma
adrfam:  ipv4
subtype: current discovery subsystem
treq:    not specified
portid:  1
trsvcid: 4420
subnqn:  nqn.2014-08.org.nvmexpress.discovery
traddr:  192.168.100.2
=====Discovery Log Entry 1======
subtype: nvme subsystem
subnqn:  nqn.2024-01.local.rdma:windows-nd
```

On this machine's target side you want to see `controller 1 summary: ... discovery=1 discLogReads=N`,
then `controller 2 summary: ... ioQueuesGranted=8`:

* `discLogReads` is usually **greater than 1**: the kernel reads the header first and then reads the records in chunks by offset,
  so **a target that ignores the log page offset sends the first chunk back over and over**.
* Conversely, our host reads Linux's discovery log with `.\run_f5.ps1 -initiatorOnly -Discover`,
  and the log prints `[disc] #0/#1 ...`, which can be compared field by field against nvmet:

```
[disc] header: genctr=2 numrec=2 recfmt=0
[disc] #0 trtype=1 adrfam=1 subtype=3 cntlid=65535 trsvcid='4420' subnqn='nqn.2014-08.org.nvmexpress.discovery'
[disc] #1 trtype=1 adrfam=1 subtype=2 cntlid=65535 trsvcid='4420' subnqn='nqn.2024-01.local.rdma:linux-nvmet'
```

* **Do not misremember what the subtypes mean**: 1 = points at another discovery controller (referral),
  2 = an NVMe subsystem, **3 = this very discovery subsystem** (nvmet uses it,
  we copy it). `cntlid` in a record is `0xffff` (dynamic): the controller does not exist until a host connects.
* **LID 1/5 are refused on a discovery controller** (`sct=0 sc=0x02`): it only has the discovery log page.
  This is nvmet's measured behaviour, and our target refuses them the same way now.

### Pipelining: the real threshold of this step
A Linux host's queue depth is 32, and **it pipelines by design** (it does not send one command and wait for the answer).
Our target does two things for this, and both can be checked in the log:

* On connect it prints `[conn] 32 admin capsule receives armed` — admin and I/O each have a
  receive ring of 32 slots, and **the advertised `crqsize` must be ≤ the number of Receives actually posted**
  (§8.43). With only one Receive, four commands fired in a row get only 1 answered
  (measured in §8.41), and the ones that were lost leave no trace at all on the target side.
* The print order of I/O commands may **differ from the arrival order**: a data transfer is "return immediately after starting,
  answer at completion", so the target can keep taking the next batch of capsules while the transfer is in progress.
  That is normal, not an out-of-order bug — the `ioCommands=` count is the criterion.
* `unknownAdmin=0 unknownIo=0` is still the core metric.
* If what you see on the Linux side is a timeout rather than an error, **count the target's `ioCommands` first**:
  fewer than the host sent means capsules were lost (the ring is too shallow or no Receive was posted),
  not that the protocol was parsed wrong.
* Measured: one `nvme list` plus a partition scan makes the host fire off dozens of reads in a row (28 in this log),
  so ring depth and "do not wait for your own data transfer" are both requirements, not optimizations.

### Multiple I/O queues (§8.49)

A real host **builds queues according to the CPU count**: with a bare `nvme connect` the kernel decides, and you can also write
`--nr-io-queues=8` explicitly (`linux/f5_dirb_check.sh` sends exactly that). Our target owns 8
queue pairs, and `Set Features Number of Queues` answers with that number.

Three things have to be visible in the log; if one of them is missing, multi-queue did not really happen:

```
[admin] Set Number of Queues -> 8
[conn] I/O 1 private data: len=56 recfmt=0 qid=1 cntlid=1     ← each queue's own CM private data
[conn] I/O queue 1 connected (1 of 8 granted)                  ← ...and so on all the way to 8
[io]    q1 fabrics Connect (fctype=0x01 qid=1)                  ← each queue's own Connect command
...
target io per queue: q1=1 q2=1 q3=0 q4=2 q5=11 q6=16 q7=0 q8=4 ← the commands really are spread over several queues
```

**The last line is the criterion**: "connected 8 but everything goes over the first" looks no different in the connection log,
so the assertion must land on **the per-queue command distribution**, not on "how many got connected".

Two failures in opposite directions, both of which really happened in this project:

| Symptom | Cause |
|---|---|
| From the second queue on, the host reports `could not add new controller` | the target **returned a number it does not have** in Set Features (we once answered 8 while owning only 1 queue pair) |
| All 8 connect, but `q1=N` and the rest are all 0 | the host connected 8 but used only one (our own initiator once did this: it asked for N and sent I/O over the first only) |
| `[conn] rejecting I/O queue N with nvme_rdma status 9` | that queue's CM private data has a `cntlid` inconsistent with the admin queue (0x09 = INVALID_CNTLID). **Note that cntlid is in the private data, not in the Connect command** — the Connect command does not have that field at all. |


---

## 3. Self-test (no Linux needed, runnable at any time) and "one command for the whole session"

```powershell
.\run_f5.ps1                    # our host against our target, port 4420
.\run_f5_auth.ps1               # the six DH-HMAC-CHAP cases (including "a wrong key must be refused" and the refusal gate)
.\run_all.ps1                   # all 9 suites, about 140 seconds, verdict table at the end
```

### The whole interop session is one command (measured to work)

```powershell
cd D:\rdma\nvmeof\src

# first see what it intends to do (changes nothing)
.\f5_session.ps1 -Check -LaptopIp 192.168.1.9 -LaptopUser yinshibai

# the real run: link (bridge + lower to 1500/1024) → peer (rxe+nvmet) → direction A → discovery → direction B → one report file
.\f5_session.ps1 -LaptopIp 192.168.1.9 -LaptopUser yinshibai

# the authenticated version (direction B starts the target with -authkey/-authctrlkey, the peer connects with -S/-C)
.\f5_session.ps1 -Auth -LaptopIp 192.168.1.9 -LaptopUser yinshibai

# only one direction
.\f5_session.ps1 -LaptopIp 192.168.1.9 -LaptopUser yinshibai -Direction a

# finish: tear the bridge down (destroy by GUID and read back to verify), restore the MTU
.\f5_session.ps1 -Down
.\interop_link.ps1 -Action RestoreMtu
# after finishing, confirm the MTU of this machine's two directly cabled ports is back to 4074/4088/2048:
Get-NetIPInterface -InterfaceAlias '以太网 23','以太网 24' -AddressFamily IPv4 |
    Select-Object InterfaceAlias, NlMtu
```

It SSHes to the laptop (with the private key given by `-KeyFile`, default `~\.ssh\nvmeof_f5_ed25519`),
runs `f5_linux_up.sh` on the peer, then runs both directions, and finally writes **the raw output of every step**
into `src/f5_session_<timestamp>.txt`.

The script's verdict is "positive evidence is required", not "if no error was seen it passed":

| Step | Criterion for passing |
|---|---|
| Direction A | all of: the number of `[FAIL]` lines is 0, `initiator failures: 0` appears, and the number of `[PASS]` lines is ≥ 10. Missing any one of them reports **INCONCLUSIVE** — because a log where "it crashed without sending a single command" also has no `[FAIL]`, and looking only at "there was no failure" proves nothing |
| Direction B | `CMP: identical` must appear on the laptop side; this machine's target summary line must satisfy `ioCommands ≥ 2`, `written = 65536`, `read >= 65536`, `keepAlives ≥ 1`, `unknownAdmin = 0`, `unknownIo = 0`. The `keepAlives` one is KATO's sentinel: if KATO was not stored from the Connect command it is 0, every Keep Alive is answered `KA_TIMEOUT_INVALID`, and a real host goes straight into error recovery |
| Direction B + authentication | additionally requires `authSends ≥ 2`, `authReceives ≥ 2`, `authRefused = 0`, and `authFatal` **exactly equal to the number of deliberate failures** (the wrong-key one). Writing `authFatal = 0` is wrong: that would fail the single most important assertion, "a wrong key is refused" |

Direction B finds the device **by serial number** (`sudo nvme list | awk '/NDVMEOF/'`), and refuses to continue with `exit 1` if it cannot find it —
**it will not go and write to a real disk such as `/dev/nvme0n1`.**

Two script details that "you only know after being burned" (both fixed this round):

* Direction A must be given `-clientLocalIp 192.168.100.2` explicitly. `run_f5.ps1` defaults to `192.168.100.3`,
  and that is exactly the bridged port A — its TCP/IP has been handed to the bridge, so opening the adapter gives `NdOpenAdapter 0xC0000141`.
  This is a side effect of the link, not a transport-layer defect, and it is the only reason "self-test F1/F7 errors out while the bridge is up".
* Probing the peer's `192.168.100.5` must come **after** step 2: that address is added by `f5_linux_up.sh`.
  The only reason putting it earlier always passed was that the previous session had left the address on the laptop — a new machine will falsely report "not on the same L2".

> **Side effect during the bridging window (normal, do not go and "fix" it)**: while the bridge exists, the TCP/IP of the two member NICs (the LAN port and CX3 port A)
> is handed over to the bridge, and Windows' own network checker reports "this adapter does not have TCP/IP services enabled".
> It recovers by itself once the bridge is removed, and `interop_link.ps1 -Action Down` **reads it back to verify** that (§8.45).

### What the self-test proves and what it does not

The self-test (both ends ours) **cannot prove interop** — it can only prove that this fixture itself runs and that the assertions can fail.

**Most recent self-test: `run_all.ps1` 8/8 pass, `initiator failures: 0 / target failures: 0`.**

It has already caught three real bugs, and every one of them is the kind that "only shows up when you change the peer":

| Caught | How it was caught |
|---|---|
| The F5 target forgot to write Identify's `cntlid` (a Linux host would refuse the controller outright because of it) | the first assertion, written after `nvme_check_ctrl_fabric_info()`, reported `cntlid=0 (Connect said 1)` the same day |
| The target had only one Receive, so a pipeline lost commands the moment several were fired in a row (only 1 of 4 answered) | fired 5 Async Events in a row and the target received only 2 |
| While the target waited for its own data transfer, `reap()` **threw away** other capsules' completions | same as above: after fixing the ring it still answered only 1, which is how the second-layer cause was found |

**But the round that actually talked to Linux (§8.46) caught 11 more defects, 7 of them cases where our two ends
misread the same field together** — and not one of those 11 showed its head in the 8/8 green self-test.
So the order is always: the self-test keeps the fixture usable, **an independent peer is the criterion.**


