# F5 — 与独立对端（Linux）互操作

> ## ✅ 已经跑通了（本轮实测，2026-10-01）
>
> 对端是一台 EndeavourOS 笔记本，**普通千兆网卡 + 软件 RoCE（rxe）**，
> 和本机 CX3 口通过「LAN 网卡 + CX3 口 A 桥接」共处一个二层。
>
> | 方向 | 命令 | 结果 |
> |---|---|---|
> | A 我们的 host → Linux `nvmet` | `.\run_f5.ps1 -initiatorOnly -subnqn nqn.2024-01.local.rdma:linux-nvmet -serverIp 192.168.100.5 -clientLocalIp 192.168.100.2` | **`initiator failures: 0`**（36 条断言；含 32 条在飞的写/读流水线各 32/32、4096 B 逐字节一致；"Number of Queues 要 4 给 128，实际用满 4/4"） |
> | B Linux `nvme-cli` → 我们的 target | `f5_interop.exe -target 192.168.100.2 4420` + 笔记本 `nvme connect` | `rc=0`；出现 `/dev/nvme2n1 NDVMEOF0000000000001`；128 块写入→flush→读回 **`cmp` 逐字节相同** |
> | B+ **DH-HMAC-CHAP**：Linux 主机认证到我们的 target | `f5_interop.exe -target 192.168.100.2 4420 -authkey <DHHC-1:..>` + 笔记本 `nvme connect -S <key>` | `RESULT: PASS`；内核日志 `qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048`；单向 + **双向**（`-C <ctrl-key>`）都跑通，65536 B 逐字节一致；**错密钥被拒** |
>
> **三个必须记住的前提**（都是踩出来的，见 DESIGN §8.46 / §8.51）：
> 1. 本端 RoCE 口必须降到 **RoCE Max Frame 1024 / IP MTU 1500**（`interop_link.ps1 -Action LowMtu`）。
>    不降的话 1024 B 的单向读能过、**4096 B 的 Identify 数据永远到不了**，而且两边都不报错。
> 2. 本驱动的 rkey 在线上是**字节反转**的，补偿已经做进 `nvmeof_rdma.h` 的 `ndWireKey()`；
>    不要在任何调用点再手工 swap。
> 3. 桥窗口**结束后**要把 `以太网 24` 恢复到 **IP MTU 4074 / Jumbo 4088 / RoCE Max 2048**，
>    否则本机两口直连的套件会在 4096 字节传输上 `ND_IO_TIMEOUT`。`-Down` 会按记录恢复，
>    而记录本身曾经被 `LowMtu` 写成"降低后的值"——那个缺陷已修（§8.51），
>    但**收工后仍要看一眼实测值**：`Get-NetIPInterface -InterfaceAlias '以太网 24'` 应为 4074。
>
> 整场一条命令：`.\f5_session.ps1 -LaptopIp 192.168.1.9 -LaptopUser yinshibai`，
> 带认证的整场：`.\f5_session.ps1 -Auth -LaptopIp 192.168.1.9 -LaptopUser yinshibai`（见 §3）。

这份文档是**给未来的我/给你照做用的**：验收表第 5、7 项需要一台 Linux，
而本机现在开不了（BIOS 里 SVM 关着，WSL2/VM 全部起不来）。所以这里把
"Linux 一出现就能立刻跑完"需要的东西全部先做好：

| 文件 | 作用 |
|---|---|
| `src/f5_interop.cpp` | 一个二进制，两个角色：`-initiator`（我们的 host 打任何对端）、`-target`（我们的 target 给 Linux host 打） |
| `src/run_f5.ps1` | 构建 + 自测 + 打 Linux 的入口（`-initiatorOnly`） |
| `linux/nvmet_setup.sh` | 在 Linux 上把 nvmet 配起来（configfs，全部可回退）；**没有 RDMA 网卡时用 `RXE_NETDEV=<网卡> ` 走软件 RoCE** |
| `linux/nvmet_teardown.sh` | 拆掉 |
| `linux/probe_mtu.sh`、`linux/rxe_counts.sh` | 只读诊断：对端口的 MTU/能力；rxe 计数器的前后差值 |
| `ref/linux_nvme_rdma.h` | RDMA-CM 私有数据的**参考副本**，`run_xref.ps1` 会逐值比对 |

## 0. 先把笔记本接到 CX3 的二层上（本机实测拓扑）

本机现状（`Get-NetAdapter` / `Get-NetIPAddress` 实测）：

| 适配器 | 是什么 | 地址 | RDMA |
|---|---|---|---|
| 以太网 20 | Intel I211 千兆 | 192.168.1.2/24，网关 192.168.1.1 | 否 |
| 以太网 23 | CX3 口 A | 192.168.100.3/24 | 是 |
| 以太网 24 | CX3 口 B | 192.168.100.2/24 | **是 ← 留给 Windows 的 RoCE 端点** |

口 A ↔ 口 B 之间是直连线。目标：让笔记本和 **192.168.100.2** 处于同一个二层。
做法是把「以太网 20」与「以太网 23」桥接起来 —— **不碰留给 RoCE 的以太网 24**。

> ⚠️ 本机 `以太网 20` 是**手工地址、没写网关**（DHCP 关闭）：IPv6 靠路由器 RA
> 一直能出去，IPv4 却没有默认路由，于是"机器能上网、有线口上不了网"，
> 全靠手机 USB 共享（`以太网 25`，10.246.250.74）顶着。修法与验证见 DESIGN §8.48：
> 加一条 `New-NetRoute ... -NextHop 192.168.1.1 -RouteMetric 50` 的**备份**路由
> （手机在时仍优先，拔掉即接管）。桥接实验前后都**按接口**回读这张表，
> 不要只看"能不能上网"。

### Windows 侧（图形界面，唯一可靠的方式）

1. `Win+R` → `ncpa.cpl`
2. 按住 Ctrl 选中 **以太网 20** 和 **以太网 23** → 右键 → **桥接**
3. 桥接会自己拿一个 IP。**先记下当前状态**再动手：

```powershell
Get-NetAdapter | Format-Table ifIndex, Name, Status, LinkSpeed
Get-NetIPAddress -AddressFamily IPv4 | Format-Table InterfaceAlias, IPAddress, PrefixLength
Get-NetRoute -DestinationPrefix 0.0.0.0/0 | Format-Table InterfaceAlias, NextHop
```

4. 桥接完成后，把局域网地址放回桥上（否则这台机器会掉网）：

```powershell
New-NetIPAddress -InterfaceAlias "网桥" -IPAddress 192.168.1.2 -PrefixLength 24
Set-NetIPInterface -InterfaceAlias "网桥" -Dhcp Disabled
New-NetRoute -InterfaceAlias "网桥" -DestinationPrefix 0.0.0.0/0 -NextHop 192.168.1.1
```

5. 确认给 RoCE 那两口加过防火墙放行（见 §1），并且**不要**把以太网 24 桥进去。

### 回滚

`ncpa.cpl` → 右键「网桥」→ **删除**；然后确认以太网 20 拿回 `192.168.1.2/24` 与默认路由
（上一步记下的值）。以太网 23/24 的地址如果丢了，重新加上：

```powershell
New-NetIPAddress -InterfaceAlias "以太网 23" -IPAddress 192.168.100.3 -PrefixLength 24
New-NetIPAddress -InterfaceAlias "以太网 24" -IPAddress 192.168.100.2 -PrefixLength 24
```

> 注意：**不要用 `New-NetSwitchTeam`** —— 那是网卡组合（teaming），不是桥接，
> 我上一版文档写错了，在此更正。

### 笔记本侧（Arch）

```sh
pacman -S --needed iproute2 rdma-core nvme-cli
# 有线口名（enp*/eth*），必须与「以太网 20」在同一台路由器/交换机下
ip -brief link

# 给笔记本加一个与 RoCE 端口同网段的地址：不需要改网关，只是多一个 IP
sudo ip addr add 192.168.100.5/24 dev enp3s0
ping -c2 192.168.100.2          # 必须通；不通就别往下走

# 软件 RoCE
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev enp3s0
rdma link show
```

然后按 §1 跑 `nvmet_setup.sh`（`RXE_NETDEV=enp3s0 ./nvmet_setup.sh 192.168.100.5`）。

**为什么笔记本也要 `192.168.100.x` 的地址**：rxe 靠 ARP 直接解析对端，
如果笔记本只有 `192.168.1.x`，去 `192.168.100.2` 的包会被送到网关（192.168.1.1），
而路由器没有到 `192.168.100.0/24` 的路由 —— 包就没了。
加一个同网段的次级地址，Linux 就会走直连路由、在本二层的 ARP 里找到对端。

**笔记本必须是网线**：rxe 跑在 wifi 上不是标准配置（AP 的客户端隔离、
组播处理和时延都会让结果不可信），会变成"测的是无线网卡而不是我们的协议"。



---

### 为什么这一步非做不可（以及为什么自测不够）

项目里所有其它套件都是**我们的 host 打我们的 target**。
这是最弱的组合：**两边同时写错就永远看不出来**，
而本项目已经因此出货过 4 个"只有我们自己认"的常量（DESIGN §8.29）。

F5 之前能做的、也已经做了的：把 Linux 的 `nvme_rdma` / `nvmet_rdma` /
`core.c` / `nvme-rdma.h` **当参考实现读**，然后拿它们的规则反过来检查我们。
这一步抓到了三个真实缺陷（DESIGN §8.40），但**读源码不等于跑起来**：
F5 仍然是唯一能证明"能互联"的东西。

---

## 1. 方向 A：我们的 initiator ↔ Linux `nvmet`（验收第 5 项）

### 先解决"两台机器怎么在同一网段"

rxe（软件 RoCE）和真 RoCE 一样走 **UDP 4791**，**要求对端在同一子网、ARP 能直接解析**——
中间有网关/路由是不行的。所以需要的不是"直连"，而是"同一二层"。三种接法：

| 接法 | 需要什么 | 说明 |
|---|---|---|
| **CX3 口 2 ↔ Linux 服务器的 SFP+/10G 口**（推荐） | 一根 QSFP→SFP+ 线或模块 | CX3 双口正好一口一个端点，最接近真实场景 |
| CX3 口 2 ↔ 交换机，Linux 服务器也在这个交换机上 | 交换机上有对应口（或 QSFP 转 RJ45 模块） | 同样要求两台机器同网段 |
| CX3 口 2 与 Windows 主板网卡**桥接** | 不需要新硬件 | 在 Windows 里做"桥接连接"，Linux 走普通 LAN 即可同段；会改动本机网络配置，回滚步骤见 §0 |

**口 1 留给 Windows 侧**（现在 `192.168.100.2`），口 2 给 Linux。

### Linux 侧

```sh
# 以 root 运行；<linux-ip> 必须与 Windows 的 CX3 口同网段（例：192.168.100.2 / .3）
chmod +x nvmet_setup.sh nvmet_teardown.sh

# 有 RDMA 网卡：
./nvmet_setup.sh 192.168.100.3

# 没有 RDMA 网卡 —— 用软件 RoCE（rxe），任意普通网卡都行：
RXE_NETDEV=ens18 ./nvmet_setup.sh 192.168.100.3
```

脚本会：加载 `nvmet`/`nvmet-rdma`（rxe 模式下还加载 `rdma_rxe` 并 `rdma link add rxe0`）、
建一个 ramdisk 命名空间、在 `<ip>:4420` 上开一个 rdma port、
**放行防火墙的 UDP 4791**，并打印 Windows 侧该跑的命令。

> **rxe 的强度**：rxe 是内核里另一套完整的 RoCEv2 实现，和硬件 RoCE 互通是被支持的，
> 作为**独立对端**完全够用（协议层不同实现）。它**不能**复现硬件的时序与吞吐，
> 所以这条路上的数字只当**正确性**证据，不当性能证据。

### Windows 侧

先做一次（RoCEv2 是 UDP 4791，入站被拦的现象和"对端不响应"一模一样）：

```powershell
New-NetFirewallRule -DisplayName 'NVMe-oF RoCEv2' -Direction Inbound `
    -Protocol UDP -LocalPort 4791 -Action Allow
Test-NetConnection 192.168.100.5 -InformationLevel Quiet   # 必须 True：同二层、不过网关
```

然后（**注意 `-clientLocalIp` 必须写本机的 RoCE 端点地址**，默认值 `.3` 是口 A，
桥接期间口 A 没有 IP，写错的表现是 `NdOpenAdapter failed 0xC0000141`）：

```powershell
cd D:\rdma\nvmeof\src
.\run_f5.ps1 -initiatorOnly -subnqn nqn.2024-01.local.rdma:linux-nvmet `
             -serverIp 192.168.100.5 -port 4420 -clientLocalIp 192.168.100.2
```

### 期望看到什么

```
[PASS] admin RDMA-CM connection to the target
[PASS] fabrics Connect qid 0 (the target's NQN was found) - cntlid=1   <- 第一条命令就是 Connect
[PASS] Property Get CAP - CAP=0x8200F00007F MQES=127 TO=15             <- 目标自己的 CAP
[PASS] Property Set CC (entry sizes, still disabled)
[cap]  CRMS.CRWMS=1 CRMS.CRIMS=0 -> CRTO is not read (as the reference host)
[PASS] Property Set CC.EN=1
[PASS] controller reports ready (CSTS.RDY)
[PASS] Property Get VS - VS=0x00020100
[PASS] inbound RDMA Write, 1024 B (Get Log Page LID 1)
[PASS] inbound RDMA Write, 4096 B (Get Log Page LID 5 effects)        <- 单向写：MTU 不匹配时死在这里
[PASS] probe: an Identify with an unknown CNS is answered (no data transfer)
[PASS] Identify Controller            <- vid/ssvid/sn/mn/fr/subnqn 全是 Linux 的值
[PASS] the target passes the host's fabrics checklist
[PASS] the target advertises the keyed SGLs this host sends
[PASS] Identify CNS 2 (active namespace list)
[PASS] Identify CNS 0 (namespace geometry)   <- 块大小由对端决定（ramdisk 上是 512）
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

> 注意"离开"的方式：**没有 fabrics Disconnect 这种命令**（§8.44）。
> 真实 host 收尾是 `Property Set CC.EN=0`，然后丢队列对。

同时 Linux 侧 `dmesg` 会打印 `nvmet_rdma: enabling port 1 (...)` 之类；
控制器建起来时会打印 `nvmet: Created nvm controller 1 for subsystem ...`。

### 出问题时看哪里

| 症状 | 先看 |
|---|---|
| Connect 直接被拒 | `dmesg` 里的 `nvmet_rdma: rejecting connect request: status N (xxx)`；N 对应 `ref/linux_nvme_rdma.h` 里的 `NVME_RDMA_CM_*`（1=无私有数据、2=recfmt、4=hsqsize、5=hrqsize） |
| **所有**命令都回 `sct=0 sc=0x02` | SQE 的 **byte 1 不是 `0x40`**（nvmet 解析前就查 PSDT，§8.46(1)） |
| 连接到 CAP 那一步就失败 | 命令顺序：Connect 必须是第一条（§8.46(2)） |
| `nvmet_rdma: invalid SGL subtype: 0x0` | 无数据命令也要带一个**长度 0 的 keyed dptr**（§8.46(3)） |
| 成功的 Connect 被判为失败 | 状态字的 **bit 0 不是成功位**，Linux 读的是 `status >> 1`（§8.46(4)） |
| Identify 无响应、缓冲全 0 | **MTU 不一致**：本端要降到 RoCE Max Frame 1024（§8.46(7)） |
| 单向 RDMA 一律 `remote access error` / `ND_ACCESS_VIOLATION` | rkey 字节序补偿是否被绕过（`ndWireKey`，§8.46(6)） |
| 数据不对但报成功 | **最值得记的一类**：说明 SGL 地址/长度/key 有一边理解错了 |
| `NdOpenAdapter 0xC0000141` | 本地 IP 指到了**被桥接的那个口**（它的 TCP/IP 已交给桥）：方向 A 要显式给 `-clientLocalIp <留给 RoCE 的口>`（§8.47(13)） |
| 脚本说"两端不在同一二层"，但对端明明在 | 探测 `192.168.100.x` 的时机早于 `f5_linux_up.sh` 加地址那一步（§8.47(14)） |
| `bash: -c: line N: syntax error near unexpected token '('` | 远程命令串穿过 PowerShell→ssh→bash 时**双引号丢了**：把序列写成 `.sh` 文件传过去（§8.47(15)） |
| `rm: cannot remove '/tmp/...': Operation not permitted` | 那个文件是上一次 `sudo nvme read -d` 建的、属于 root；数据文件应放在 `mktemp -d` 的私有目录里（§8.47(16)） |

---

## 2. 方向 B：Linux `nvme-cli` ↔ 我们的 target（验收第 7 项）

### Windows 侧

```powershell
D:\rdma\nvmeof\src\f5_interop.exe -target 192.168.100.2 4420
```

它打印自己支持的 subnqn（`nqn.2024-01.local.rdma:windows-nd`），
并且**逐条记录外来的 host 发了什么** —— 这就是这一步的产出。

### Linux 侧

> ⛔ **绝对不要用 `/dev/nvme0n1` 这种"第一个 nvme 设备"来读写。**
> 在实测的那台笔记本上，`/dev/nvme0n1` 是**它自己的 512 GB 系统盘**。
> 下面的序列**按序列号**找我们的设备（序列号以 `NDVMEOF` 开头），
> 找不到就报错退出、什么都不碰。

**推荐做法：跑仓库里的 `linux/f5_dirb_check.sh`**（`f5_session.ps1` 第 4 步就是把它
scp 到对端家目录再执行；`~/nvmeof/linux` 是 root 所有，普通用户写不进去）：

```sh
scp -i ~/.ssh/nvmeof_f5_ed25519 linux/f5_dirb_check.sh yinshibai@<laptop>:/home/yinshibai/
ssh yinshibai@<laptop> "sh ~/f5_dirb_check.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd"
```

它内部就是下面这一串，退出码 0 只代表 **65536 字节写出去、flush、读回来逐字节相同**：

```sh
sudo nvme disconnect -n nqn.2024-01.local.rdma:windows-nd 2>/dev/null || true   # 清掉上次残留
sudo nvme connect -t rdma -a 192.168.100.2 -s 4420 \
                  -n nqn.2024-01.local.rdma:windows-nd
sleep 1
sudo nvme list                        # 应该出现 NDVMEOF... 的 32 MiB 命名空间

DEV=$(sudo nvme list | awk '/NDVMEOF/ {print $1}' | head -1)
[ -n "$DEV" ] || { echo 'FAIL: no NDVMEOF device - refusing to touch anything else'; exit 1; }
echo "using $DEV"

sudo nvme id-ctrl "$DEV" | head -20    # 逐字段对照我们的 Identify

# 数据面：写 → flush → 读回 → 逐字节比。用 nvme-cli 自己的 write/read，
# 不用 dd——dd 那条路要 root 直接开块设备，而这条路上我们只要命令本身。
dd if=/dev/urandom of=/tmp/f5pat bs=4096 count=16 status=none
sudo nvme write "$DEV" -s 0 -c 127 -b 512 -z 65536 -d /tmp/f5pat
sudo nvme flush "$DEV"
rm -f /tmp/f5back
sudo nvme read  "$DEV" -s 0 -c 127 -b 512 -z 65536 -d /tmp/f5back
cmp /tmp/f5pat /tmp/f5back && echo 'CMP: identical (65536 bytes written, flushed, read back)'

sudo nvme disconnect -n nqn.2024-01.local.rdma:windows-nd
```

> **为什么是一整个 `.sh` 文件，而不是一条 ssh 命令串**：命令串要穿过
> PowerShell → `ssh.exe` → bash 三层，**双引号在途中会丢**，于是第一对括号就变成
> `bash: -c: line 12: syntax error near unexpected token '('`——实测中这一条
> 正好死在序列中间，还在对端留下一个活着的控制器。文件没有引号可丢。

> 每一条 `nvme` 都需要 root（否则是 `Failed to open /dev/nvme-fabrics: Permission denied`），
> 用 `sudo -n` 免得卡在没人能回答的密码提示上。**脚本本身不需要 root**，
> 除非 `/etc/sudoers.d/` 里正好写了它的绝对路径。

**长度单位是这一路上最容易搞错的地方**：`-c` 是**设备逻辑块**数（0 基），
而这台设备对外是 **512 B 块**，所以 65536 B 要写 `-c 127 -b 512`。
写 `-c 15 -b 4096` 只会传 16×512 = **8192 B**，然后 `cmp` 会在第 8193 字节开始
报"不一致"——那不是数据面坏了，是命令只要了 8 KiB。

注意：**`nvme connect` 用的也是 rxe 设备**。Linux 侧 `rdma link show` 必须能看到 rxe0，
`nvme connect` 才能绑到那个地址上。如果 Linux 用 rxe、Windows 用真 RoCE，
这个方向同样成立（两边都是 RoCEv2）。

### 期望看到什么

Windows 侧 target 日志里（**顺序就是真实 Linux host 的顺序**：Connect 在最前面）：

```
[conn] private data: ... recfmt=0 qid=0 hrqsize=32 hsqsize=31
[conn] accepted; advertised crqsize=32
[admin] fabrics Connect qid=0 sqsize=31 kato=5000
      connect data: RDMA Read posted (addr=0x.. key=0x.. len=1024)
[data]  completion for the transfer: status=0x00000000 ND_SUCCESS bytes=1024
      hostnqn='nqn.2014-08.org.nvmexpress:uuid:...'   <- host 自己读来的，能打印出来就说明单向读通了
[admin] Property Get off=0x0 -> ...      <- CAP，host 连问两次
[admin] Property Set off=0x14 val=0x460000   <- CC：IOSQES=6 / IOCQES=4
[admin] Property Set off=0x14 val=0x460001   <- CC.EN=1
[admin] Property Get off=0x1C -> 0x1     <- CSTS.RDY
[admin] Property Get off=0x8 -> 0x10400  <- VS
[admin] Identify cns=1 nsid=0
[admin] Get Log Page lid=2 numd=127 -> 512 bytes (zero-filled)
[admin] Set Number of Queues -> 1        <- 我们只承诺 1 条 I/O 队列，就必须只创建 1 对
[conn] I/O queue pair connected (one at a time)
[io]    fabrics Connect on the I/O queue (fctype=0x01 qid=1)
[io]    cmd 1 WRITE ... sgl(... INVALIDATE)   <- 见下
[admin] Keep Alive #1 (kato=5000 ms)          <- kato 来自 Connect 命令，不是 Set Features
...
target summary: ... unknownAdmin=0 unknownIo=0
```

**`unknownAdmin=0 unknownIo=0` 是这一步的核心指标**：
它意味着"外来 host 发的每一条命令，这个 target 都认得"。
非零就说明有一条真实命令我们没实现，日志里会带 `UNHANDLED opcode=0x..`。

**另外三个也是判据**（这一轮就是在这三处翻过车，§8.46）：

1. `Keep Alive #N` 必须出现。KATO 是从 **Connect 命令**里来的（毫秒），
   如果 target 没存它，`katoMs` 就是 0，而 KATO=0 时**规范要求**把每条 Keep Alive
   回成 `KA_TIMEOUT_INVALID` —— 主机会认为 keep-alive 失败，进入 error recovery，
   **把 I/O 队列冻住**，症状是"一堆读命令永远完不成"。
2. `[io] fabrics Connect on the I/O queue` 之后要有正常的 I/O；如果主机在建完 I/O 队列后
   立刻拆连接，检查 I/O 队列的 Accept **有没有带 CM 回复私有数据**（§8.46(8)）。
3. `Set Number of Queues -> N` 的 N 必须是**我们真的能服务的条数**（现在是 1）：
   回给主机的数字是契约，回了 8 它就会建 8 条并在第二条上失败（§8.46(9)）。

### 两个已经知道、也要在日志里确认的点

1. **INVALIDATE 子类型**：Linux host 默认对**每一条读写**都打 `0x4f`
   （keyed + invalidate，见 `nvme_rdma_map_sg_fr`）。我们的 target 现在
   **照做传输 + 普通 SEND 应答**，因为 Linux host 的完成路径在没有收到
   invalidation 时会**自己本地作废**它的 key。所以日志里应看到 `INVALIDATE`
   且命令成功——如果看到 `SGL_INVALID_TYPE` 之类的拒绝，就是回归了。
2. **Flush**：我们 advertise `vwc=0`，所以正常路径上 host 不会发 Flush；
   真发了也必须成功（`flushes=` 计数）。
3. **`read=` 会大于 65536，这是正常的**：真主机会读命名空间去探测几何信息，
   所以 `written=65536`（我们写多少就是多少）可以断言相等，`read` 只能断言
   `>= 65536`。第一版写成 `read == 65536` 会误报"数据没回来"（§8.47）。
4. **上一次没跑完留下的控制器会自己重连**：此时再 `nvme connect` 同一个 NQN，
   内核回 `could not add new controller: failed to write to nvme-fabrics device`，
   而真正握手的是旧控制器的重连 —— 看起来像"我们的 target 拒绝新连接"，
   其实是对端状态脏了。`linux/f5_dirb_check.sh` 会先断开并**等到设备真的消失**
   再连（§8.47(16)）。手工排查：`sudo nvme list | grep NDVMEOF` 应该为空。

### 带 DH-HMAC-CHAP 的方向 B（§8.51）

同一条链路上再加一层在带内认证。**证书就是那一串 `DHHC-1:..`**，两端必须是同一个字符串：

```powershell
# Windows 侧：生成密钥。第一行 = 主机密钥，第二行 = 控制器密钥（双向用），第三行 = 一个"错的"密钥
.\f5_interop.exe -genkey        # 多跑几次，或直接用 f5_authkey.txt 的三行格式
# 让我们的 target 要求认证
.\f5_interop.exe -target 192.168.100.2 4420 -serve 3 `
    -authkey <host-key> -authctrlkey <ctrl-key>
```

```sh
# Linux 侧（nvme-cli 3.x：-S / -C；2.x 是 --dhchap-secret / --dhchap-ctrl-secret）
sudo nvme connect -t rdma -a 192.168.100.2 -s 4420 -n nqn.2024-01.local.rdma:windows-nd \
     -S "$HOSTKEY" --nr-io-queues=8
sudo nvme connect -t rdma -a 192.168.100.2 -s 4420 -n nqn.2024-01.local.rdma:windows-nd \
     -S "$HOSTKEY" -C "$CTRLKEY"          # 双向：主机也会要求 target 证明自己
```

一条命令跑完（上传脚本 + 密钥文件，四个用例，含"错密钥必须被拒"）：

```powershell
.\f5_session.ps1 -Auth -Direction b -LaptopIp 192.168.1.9 -LaptopUser yinshibai
```

**判据（缺一不可）**：

* Linux 内核日志里出现 `qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048`
  —— 这是**对端自己的话**，不是我们的日志。
* `nvme list` 里出现 `NDVMEOF...` 设备，并且写入→flush→读回逐字节相同
  （认证过了但数据面不通是另一个问题，不能只看"连上了"）。
* 用**另一个合法密钥**连接必须失败（我们是 Failure1 + `resctrl_exp=0x01`）。
  只测成功路径的认证实现，等于没测：一个从不校验响应的 target 也能让第一、二条全绿。
* target 侧 `authSends/authReceives` 各 ≥2，`authRefused=0`（有认证的控制器不该有任何命令
  因为"没认证"被拒），`authFatal=` 恰好等于**故意失败**的次数。

### 反向认证：我们的 host → 要求认证的 nvmet（已完成，§8.52）

nvmet 的认证只能写 configfs，而笔记本上的免密白名单是**逐条精确路径**的，所以这一步当初卡在
"要装一个新版 `nvmet_setup.sh` 到白名单路径上，而 install 需要一次密码"。
内核支持没问题（`CONFIG_NVME_TARGET_AUTH=y`）。**现在已经装好并跑通**，留下来是因为
"谁需要动手、为什么"是以后再加特权步骤时要照抄的形状：

```sh
# 一次性（需要密码）：把带 AUTH_KEY_FILE 支持的版本装到白名单路径
sudo install -m 755 -o root -g root ~/nvmet_setup.sh.new ~/nvmeof/linux/nvmet_setup.sh
```

装好之后全是免密命令：

```sh
# 重建带认证的 target（密钥文件里第一行主机密钥、第二行控制器密钥）
# 注意：重启会清空 /tmp，并丢掉 ip addr add 的地址与 rxe0 —— peer_auth_up.sh 两件都做
sh ~/peer_auth_up.sh
```

```powershell
# Windows 侧：我们的 host 用同一个密钥去认证
#   密钥从文件读，不走命令行（进程表和 history 里都不要出现它）
scp <对端key文件> .\peer_authkey.txt
.\run_f5.ps1 -initiatorOnly -subnqn nqn.2024-01.local.rdma:linux-nvmet `
    -serverIp 192.168.100.5 -clientLocalIp 192.168.100.2 -AuthKeyFile .\peer_authkey.txt
```

期望（实测就是这样）：

```
[auth] the Connect result asks for authentication (ATR set, result=0x20001)
[auth] Challenge: hash=0x01 dhgroup=1 seqnum=...
[PASS] the target accepted the host's response (Success1)
[PASS] Success2 sent (the controller's own response verified)     <- 双向：我们验了对端的 R2
initiator failures: 0
```

对端内核会同时留下它那份：`nvmet: Created nvm controller 1 … with DH-HMAC-CHAP.`

### 方向 B 的 admin 扫描与控制器复位（§8.52）

`linux/f5_dirb_admin.sh` 是方向 B 的第二个脚本：16 条 `nvme-cli` admin 命令 + 可选的一次
`nvme reset`。**判据是 target 自己的 `unknownAdmin` 计数，不是 host 的返回码**——
host 的 rc=1 可能只是 nvme-cli 自己拒绝（`effects-log` 要 `-c <csi>`、`set-feature` 的值是 `-V`），
那样的"失败"一次都没发到对端。

```sh
sh ~/f5_dirb_admin.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd        # 不含复位
sh ~/f5_dirb_admin.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd -r     # 含复位
```

`-r` 是**故意 opt-in** 的：控制器复位期间如果 target 不在，host 会卡到 Ctrl Loss Timeout，
并把一个任务留在 D 状态、连同 nvme-core 的控制器锁——这台机器上实测过一次，
`nvme list`/`nvme disconnect` 一起卡死，只能重启。target 端要配 `-reconnectwait 180`。

### discovery：先问"这里有什么"，再决定连谁（§8.50）

真实部署的第一步是 `nvme discover`，它是**另一个控制器**（自己的 admin queue pair），
所以 **target 要用 `-serve 2` 启动**：discovery 那条连接读完日志页就消失，之后 I/O 那条
才来。默认 `-serve 1` 是为了保持 F7"主机消失即退出"的断言。

```sh
# Linux 侧：列出我们的 target 提供了什么
sudo nvme discover -t rdma -a 192.168.100.2 -s 4420
```

期望看到（我们自己跑出来的样子）：

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

本机 target 侧要看到 `controller 1 summary: ... discovery=1 discLogReads=N`，
然后 `controller 2 summary: ... ioQueuesGranted=8`：

* `discLogReads` 通常 **大于 1**：内核先读头、再按 offset 分块读记录，
  **忽略 log page offset 的 target 会把第一块反复发回去**。
* 反过来，我们的 host 用 `.\run_f5.ps1 -initiatorOnly -Discover` 读 Linux 的发现日志，
  日志里会打印 `[disc] #0/#1 ...`，可以逐字段和 nvmet 对：

```
[disc] header: genctr=2 numrec=2 recfmt=0
[disc] #0 trtype=1 adrfam=1 subtype=3 cntlid=65535 trsvcid='4420' subnqn='nqn.2014-08.org.nvmexpress.discovery'
[disc] #1 trtype=1 adrfam=1 subtype=2 cntlid=65535 trsvcid='4420' subnqn='nqn.2024-01.local.rdma:linux-nvmet'
```

* **subtype 的含义别记错**：1 = 指到另一个 discovery 控制器（referral）、
  2 = 一个 NVMe 子系统、**3 = 就是当前这个 discovery 子系统**（nvmet 用它，
  我们照抄）。`cntlid` 在记录里是 `0xffff`（dynamic）：控制器要等主机连上才存在。
* **discovery 控制器上 LID 1/5 会被拒绝**（`sct=0 sc=0x02`）：它只有发现日志页。
  这是实测出来的 nvmet 行为，我们的 target 现在也一样拒绝。

### 流水线：这一步真正的门槛
Linux host 的队列深度是 32，**它按设计流水线**（不会发一条等一条）。
我们的 target 为此做了两件事，日志里可以核对：

* 连上时会打印 `[conn] 32 admin capsule receives armed` —— admin 与 I/O 各有
  32 个槽的 receive ring，而且**宣称的 `crqsize` 必须 ≤ 实际挂上的 Receive 数**
  （§8.43）。只有一个 Receive 时，一次连发 4 条命令只有 1 条被应答
  （§8.41 的实测），而且丢的那几条在 target 侧完全没有痕迹。
* I/O 命令打印顺序可能**与到达顺序不同**：数据传输是"启动后立即返回、
  完成时才应答"，这样 target 才能在传输进行中继续收下一批 capsule。
  这是正常的，不是乱序 bug——`ioCommands=` 的计数才是判据。
* `unknownAdmin=0 unknownIo=0` 仍是核心指标。
* 如果 Linux 侧看到的是超时而不是错误，**先数 target 的 `ioCommands`**：
  少于 host 发出的条数就说明 capsule 丢了（ring 太浅或没挂接收），
  而不是协议解错。
* 实测一次 `nvme list` + 分区扫描就会让 host 连发几十条读（本条日志里是 28 条），
  所以 ring 深度和"不等自己的数据传输"这两件事是必须的，不是优化。

### 多 I/O 队列（§8.49）

真主机会**按 CPU 数建队列**：`nvme connect` 不带参数时由内核决定，也可以显式写
`--nr-io-queues=8`（`linux/f5_dirb_check.sh` 就是这么发的）。我们的 target 拥有 8 个
queue pair，`Set Features Number of Queues` 回的就是这个数。

日志里要看到三样东西，缺一样就说明多队列没真正成立：

```
[admin] Set Number of Queues -> 8
[conn] I/O 1 private data: len=56 recfmt=0 qid=1 cntlid=1     ← 每条队列各自的 CM 私有数据
[conn] I/O queue 1 connected (1 of 8 granted)                  ← ...一直到 8
[io]    q1 fabrics Connect (fctype=0x01 qid=1)                  ← 每条队列各自的 Connect 命令
...
target io per queue: q1=1 q2=1 q3=0 q4=2 q5=11 q6=16 q7=0 q8=4 ← 命令真的分散在多条队列
```

**最后一行才是判据**："连了 8 条但全走第一条"在连接日志里看不出任何区别，
所以断言必须落在**每队列的命令分布**上，而不是"连上了几条"。

两个相反方向的失败，都在本工程真实发生过：

| 症状 | 原因 |
|---|---|
| 主机第二条队列起就报 `could not add new controller` | target 在 Set Features 里**回了一个自己没有的数**（我们曾经回 8 却只有 1 个 queue pair） |
| 8 条都连上，但 `q1=N` 其余全 0 | 主机连了 8 条却只用一条（我们自己的 initiator 曾经如此：申请了 N 条，I/O 只走第一条） |
| `[conn] rejecting I/O queue N with nvme_rdma status 9` | 该队列的 CM 私有数据里 `cntlid` 与 admin 队列不一致（0x09 = INVALID_CNTLID）。**注意 cntlid 在私有数据里，不在 Connect 命令里**——Connect 命令根本没有这个字段。 |


---

## 3. 自测（不需要 Linux，随时可跑）与"一条命令跑完整场"

```powershell
.\run_f5.ps1                    # 我们的 host 打我们的 target，端口 4420
.\run_f5_auth.ps1               # DH-HMAC-CHAP 六个用例（含"错密钥必须被拒"和 refusal gate）
.\run_all.ps1                   # 9 套全跑，约 140 秒，结论表在最后
```

### 整场互操作只有一条命令（实测可用）

```powershell
cd D:\rdma\nvmeof\src

# 先看它打算做什么（不改任何东西）
.\f5_session.ps1 -Check -LaptopIp 192.168.1.9 -LaptopUser yinshibai

# 真跑：链路(桥+降到1500/1024) → 对端(rxe+nvmet) → 方向 A → discovery → 方向 B → 一份报告文件
.\f5_session.ps1 -LaptopIp 192.168.1.9 -LaptopUser yinshibai

# 带认证的那一版（方向 B 用 -authkey/-authctrlkey 起 target，对端用 -S/-C 连）
.\f5_session.ps1 -Auth -LaptopIp 192.168.1.9 -LaptopUser yinshibai

# 只跑一个方向
.\f5_session.ps1 -LaptopIp 192.168.1.9 -LaptopUser yinshibai -Direction a

# 收尾：拆掉桥接（按 GUID 销毁并回读验证）、恢复 MTU
.\f5_session.ps1 -Down
.\interop_link.ps1 -Action RestoreMtu
# 收工后确认本机两口直连的 MTU 回到了 4074/4088/2048：
Get-NetIPInterface -InterfaceAlias '以太网 23','以太网 24' -AddressFamily IPv4 |
    Select-Object InterfaceAlias, NlMtu
```

它会 SSH 到笔记本（用 `-KeyFile` 指定的私钥，默认 `~\.ssh\nvmeof_f5_ed25519`），
在对端执行 `f5_linux_up.sh`，再把两个方向都跑一遍，最后把**每一步的原始输出**
写进 `src/f5_session_<时间戳>.txt`。

脚本的判定是"必须拿到正面证据"，不是"没看见错误就算过"：

| 步骤 | 通过的判据 |
|---|---|
| 方向 A | 同时满足：`[FAIL]` 条数为 0、出现 `initiator failures: 0`、`[PASS]` 条数 ≥ 10。少任何一条都报 **INCONCLUSIVE**——因为"一条命令都没发就崩了"的日志里同样没有 `[FAIL]`，光看"没有失败"证明不了任何事 |
| 方向 B | 笔记本侧必须出现 `CMP: identical`；本机 target 的 summary 行必须满足 `ioCommands ≥ 2`、`written = 65536`、`read >= 65536`、`keepAlives ≥ 1`、`unknownAdmin = 0`、`unknownIo = 0`。`keepAlives` 那一条是 KATO 的哨兵：KATO 若没从 Connect 里存下来就是 0，每条 Keep Alive 都答 `KA_TIMEOUT_INVALID`，真机会直接进错误恢复 |
| 方向 B + 认证 | 额外要求 `authSends ≥ 2`、`authReceives ≥ 2`、`authRefused = 0`，并且 `authFatal` **恰好等于故意失败的次数**（错密钥那一次）。写成 `authFatal = 0` 是错的：那会把"错密钥被拒"这条最重要的断言判成失败 |

方向 B 按**序列号**找设备（`sudo nvme list | awk '/NDVMEOF/'`），找不到就 `exit 1` 拒绝继续——
**不会去写 `/dev/nvme0n1` 这种真盘。**

两个"只有踩过才知道"的脚本细节（都是本轮修掉的）：

* 方向 A 必须显式给 `-clientLocalIp 192.168.100.2`。`run_f5.ps1` 默认是 `192.168.100.3`，
  而那正是被桥接的口 A —— 它的 TCP/IP 已交给桥，打开适配器会得到 `NdOpenAdapter 0xC0000141`。
  这是链路的副作用，不是传输层的缺陷，也是"桥在的时候自测 F1/F7 报错"的唯一原因。
* 探测对端 `192.168.100.5` 必须放在第 2 步**之后**：这个地址是 `f5_linux_up.sh` 加上的。
  放在前面之所以一直能过，只是因为上一次会话把地址留在了笔记本上——换台新机器就会假报"不在同一二层"。

> **桥接窗口期的副作用（正常，别去"修"）**：桥存在时，两块成员网卡（LAN 口与 CX3 口 A）
> 的 TCP/IP 会被移交给桥，Windows 自己的网络检查器会报"该适配器没有启用 TCP/IP 服务"。
> 拆桥后自动恢复，`interop_link.ps1 -Action Down` 会**回读验证**这一点（§8.45）。

### 自测证明了什么、没证明什么

自测（两端都是我们）**不能证明互操作**——它只能证明这套夹具本身能跑、断言能失败。

**最近一次自测：`run_all.ps1` 8/8 通过，`initiator failures: 0 / target failures: 0`。**

它已经抓到过三个真 bug，而且每一个都是"只有换对端才会暴露"的那一类：

| 抓到的 | 怎么抓到的 |
|---|---|
| F5 target 忘了写 Identify 的 `cntlid`（Linux host 会因此直接拒绝控制器） | 照着 `nvme_check_ctrl_fabric_info()` 写的第一条断言，当天就报 `cntlid=0 (Connect said 1)` |
| target 只有一个 Receive，流水线一连发就丢命令（4 条里只答 1 条） | 连发 5 条 Async Event，target 只收到 2 条 |
| target 等自己的数据传输时，`reap()` 把别的 capsule 完成**扔掉** | 同上：修了 ring 之后仍然只答 1 条，才发现第二层原因 |

**但真正和 Linux 对接的那一轮（§8.46）又抓出 11 个缺陷，其中 7 个是我们两端
对同一字段的理解一起错**——这 11 个在 8/8 全绿的自检里一个都没露头。
所以顺序永远是：自检保证夹具可用，**独立对端才是判据**。


