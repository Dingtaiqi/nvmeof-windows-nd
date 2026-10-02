# Windows 上的 NVMe-oF over RDMA（自研 NetworkDirect 栈）

**中文** | [English](README.md)

这个工程用**我们自己的 NetworkDirect/NDSPI 代码**在 Windows 上实现 NVMe-oF/RDMA
传输：fabrics 命令、64 字节 capsule、keyed SGL/STag、RDMA Read/Write、
内存注册、每个 I/O 队列独立 queue pair，**initiator 与 target 两端都是我们的代码**，
没有使用任何第三方 NVMe-oF 实现。

先读结论再读代码：**[DESIGN.md](DESIGN.md)** 是设计与实测记录（§8 是逐条踩坑史），
**[INTEROP_F5.md](INTEROP_F5.md)** 是"如何用一台 Linux 机器做独立对端"的操作手册。

---

## 目录

| 路径 | 是什么 |
|---|---|
| `src/nvmeof_wire.h` | 线上格式：capsule/CQE/SGL/Identify/状态码，每个布局都有 `NVMEOF_STATIC_ASSERT` 钉住 |
| `src/nvmeof_rdma.h` | 传输层：`Device` / `Queue` / `acceptChecked()`（按 Linux 的规则校验 Connect 私有数据） |
| `src/f1_bringup.cpp` | F1 连接与 Identify |
| `src/f3_io.cpp` | F3 读写正确性 + 错误路径（含 0x4f invalidate 子类型） |
| `src/f4_pipeline.cpp` | F4 流水线吞吐（8 条在飞） |
| `src/f5_interop.cpp` | **F5 互操作**：`-initiator` 打任意对端，`-target` 给真实 host 用（32 槽 receive ring，覆盖它宣称的窗口） |
| `src/f6_lifecycle.cpp` | F6 host 完整序列 + 目标侧生命周期（31 条断言） |
| `src/f7_faults.cpp` | F7 故障注入（对端消失、反向消失） |
| `src/nvmeof_auth.h` | DH-HMAC-CHAP 的密码学原语：CNG 的 SHA/HMAC + 自研定长 Montgomery 模幂（`nvmeof_bignum.h`）+ RFC 7919 ffdhe 群（`nvmeof_dhgroups.h`） |
| `src/nvmeof_dhchap.h` | DH-HMAC-CHAP 协议：密钥解析（DHHC-1 + CRC32）、`Kt` 变换、target 半边、host 半边、环回自检 |
| `src/wire_selftest.c` | 字节级 golden 自检，C 与 C++ 双份编译 |
| `src/xref_constants.py` | 常量与两份 Linux 参考头文件**逐值比对**（97 对，含 DH-HMAC-CHAP 的 21 个） |
| `src/run_all.ps1` | 一次跑完全部 10 套并给结论表（约 220 秒） |
| `src/interop_link.ps1` | 互操作链路：桥接 + 搬 IP/路由 + 降 MTU，失败自动回滚 |
| `src/f5_session.ps1` | **互操作整场**：链路 → 对端 → 方向 A → discovery → 方向 B → 一份报告文件；`-Auth` 换成带认证的版本 |
| `src/run_f5_auth.ps1` | DH-HMAC-CHAP 六个用例（本机两口直连，不需要 Linux） |
| `src/nvmeof_iscsi.h` | **用户态 iSCSI target**：三段式 login、SendTargets、NOP/Logout/TaskMgmt、SCSI 命令集（INQUIRY/VPD、MODE SENSE、READ CAPACITY、REPORT LUNS、READ/WRITE(10/16) 走 R2T、SYNCHRONIZE CACHE）。每连接一个对象一个线程，backend（一条队列对）用互斥量串行。装不进命令 PDU 的写按 ITT 挂起成状态（`PendingWrite`，窗口 8），不是在 `handleScsi()` 里阻塞等 Data-Out |
| `src/tools_iscsi_write.ps1` | 写路径端到端逐字节检查：确定性图案写入 → initiator 读回 → 自建 SCSI pass-through 发 SYNCHRONIZE CACHE → 比对后端 namespace 文件（三条独立证据） |
| `src/tools_login_probe.ps1` | 把 Windows 真实发出的 iSCSI login **逐字节重放**给任意 target（做 LIO 参考实现的 A/B 对比用） |
| `src/mount_nvmeof.ps1` | 命名空间 → fixed VHD → 盘符，卸载时回推 |
| `EVIDENCE-1TB.md` | 1 TB 真盘挂到 Windows 的全部实测输出（含拆除记录） |
| `ref/linux_nvme.h`、`ref/linux_nvme_rdma.h` | 参考副本，供上面对比与查证 |
| `linux/` | 对端脚本：`nvmet_setup.sh`（含 rxe 软件 RoCE 与可选 `AUTH_KEY=` 认证）、`f5_linux_up.sh`、`nvmet_teardown.sh`、`f5_dirb_check.sh`（方向 B）、`f5_dirb_auth.sh`（方向 B + 认证）、`f5_dirb_demo.sh`（宽扫描 + 持久化演示）、`f5_nvmet_ref*.sh`（把 nvmet 当规格逐行量参考答案）、`f5_dsm_check.sh`（DSM 的 AD 位到底在哪） |

## 前置条件（要自己编译的话）

1. **Visual Studio**（含 C++ 桌面工作负载，用它的 `VsDevCmd.bat` 提供 `cl.exe`）。
2. **NetworkDirect / NDSPI 的头文件与库**：`ndspi.h`、`ndutil.h`/`ndutil.lib`。**本仓库不含这些**
   （它们不是本工程的代码，来自 WDK/Windows SDK 的 NetworkDirect 部分，或网卡厂商的
   ND 提供者安装包；实测环境是 HP/Mellanox ConnectX-3 Pro + WinOF 的 ND 提供者）。
   编译时还需要厂商的 NDv2 头（Mellanox 的 `…\MLNX_VPI\IB\SDK\inc\ndv2`），有则加入包含路径。
3. 三个路径通过**环境变量**覆盖，不设就用本机实测的默认值，**不需要改脚本**：

   | 环境变量 | 含义 | 不设时的默认值 |
   |---|---|---|
   | `ND_VS_DIR` | Visual Studio 安装目录 | `F:\Microsoft Visual Studio\18\Community` |
   | `ND_NDUTIL_INC` | NetworkDirect 头文件目录 | `D:\rdma\NetworkDirect\src\ndutil` |
   | `ND_NDUTIL_LIB` | `ndutil.lib` 所在目录 | `D:\rdma\NetworkDirect\src\x64\Release` |
   | `ND_MLNX_INC` | 厂商 NDv2 头目录（可选） | `C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2` |

   脚本自己所在目录一律用 `$PSScriptRoot` 推导，所以仓库放在哪里都能跑：

   ```powershell
   $env:ND_VS_DIR     = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
   $env:ND_NDUTIL_INC = 'C:\ndsdk\src\ndutil'
   $env:ND_NDUTIL_LIB = 'C:\ndsdk\src\x64\Release'
   cd <仓库>\src ; .\run_wire.ps1        # 先跑这个：不需要网卡
   ```

4. `ref/` 下两份是 Linux 内核的参考头文件（`linux_nvme.h` / `linux_nvme_rdma.h`，
   GPL-2.0 的 UAPI/内核头），**只作对照阅读，不参与编译**。

## 跑起来

一次跑全部（约 6 分钟，按顺序执行、互相不干扰）：

```powershell
cd <仓库>\src
.\run_all.ps1
```

单独跑某一套：`run_xref.ps1`、`run_wire.ps1`、`run_f1.ps1`、`run_f3.ps1`、
`run_f4.ps1`、`run_f5.ps1`、`run_f5_auth.ps1`、`run_f6.ps1`、`run_f7.ps1`。

每套的规则都一样：**删掉旧 exe → 看编译退出码 → 比对源码与头文件时间戳**，
绝不运行"看起来还在"的旧二进制（这条是从一次真实事故里来的，见 DESIGN §8.6）。

## 要接真实 host 时用哪个二进制

| 场景 | 用什么 |
|---|---|
| Linux `nvme-cli` / 任何外来 host 打我们 | **`f5_interop.exe -target <ip> <port>`** |
| 我们的 host 打 Linux `nvmet` 或任何对端 | `f5_interop.exe -initiator <serverIp> <port> <localIp> [-subnqn <nqn>]` |
| 自测（两端都是我们） | 任意 `run_f*.ps1` |

**F6 的 target 是生命周期测试替身，不是互操作 target**：它的 host 是严格一问一答，
所以它只挂一个 Receive，遇到会流水的真实 host 会丢 capsule（DESIGN §8.41）。
F5 的 target 有 8 槽 receive ring 与延迟完成，能扛住队列深度 32 的 host。

## 当前状态

| 验收项 | 状态 |
|---|---|
| 1 wire 自检 | ✅ `run_wire.ps1` |
| 2 两端 Identify 一致 | ✅ F1 / F6 |
| 3 写后读逐字节一致 | ✅ F3 |
| 4 错误路径有界（含对端消失，双向） | ✅ F3 / F6 / F7 |
| 5 **我们的 host ↔ Linux `nvmet`** | ✅ **跑通了**：`initiator failures: 0`（36 项检查），含逐字节一致的 WRITE/READ、32 条在飞的流水线，以及 **4 条 I/O 队列各自同时跑一条命令**（DESIGN §8.46、§8.49） |
| 6 吞吐 vs 裸 RDMA 基线 | ✅ 约 1.17 GB/s ≈ 2.53 GB/s 的 **48%**（DESIGN §8.16） |
| 7 **我们的 target ↔ Linux `nvme-cli`** | ✅ **跑通了**：`nvme connect` rc=0，`nvme list` 出现 `NDVMEOF0000000000001`，128 块写入→flush→读回 **`cmp` 逐字节相同**；主机建 **8 条 I/O 队列**、命令分散在 **6 条**上（DESIGN §8.46、§8.49） |
| 8 **DH-HMAC-CHAP 在带内认证** | ✅ **两个方向都跑通了（Linux 对端实测）**：真 Linux 主机用 `nvme connect -S <key>` 认证到我们的 target（内核日志 `qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048`，单向与**双向**都通过并搬运了数据，**错密钥被拒**，`authRefused=0`）；反向也一样——我们的 host 认证到**要求认证的 nvmet**，`Success2 sent (the controller's own response verified)`，即真 ffdhe2048 DH + 双向（DESIGN §8.51、§8.52） |
| 9 **Linux 的 1 TB 真盘成为 Windows 的活动盘** | ✅ Linux `/dev/nvme1n1`（CT1000P3PSSD8，1953525168 × 512 B = 931.51 GiB）出现为 `Get-Disk` #3，**Online**、GPT、NTFS `E:` 可读；两次与 Linux 侧的字节级对照；**全程零写入**（DESIGN §8.55、§8.56，[EVIDENCE-1TB.md](EVIDENCE-1TB.md)） |
| 10 **持续串流压测** | ⚠️ 读：整条 Windows 路径在**队列深度 1** 时 **79 MB/s**（64 KiB 块、单线程同步读；每条命令 0.755 ms，其中 Data-In socket 写 0.635 ms、NVMe 暂存 0.115 ms），**8 个并发读者时 230.6 MB/s**；1 MiB 读在深度 1 达 **119.7 MB/s**（Windows 会把 CDB 放大到 256 KiB，于是一条命令 = 一次 256 KiB 的 NVMe 读 + 4 个 Data-In PDU，见 DESIGN §8.59）。**报这个桥的吞吐必须带上队列深度**：早先的"73–102 MB/s"是单线程夹具的数字，不是天花板。单命令开销的大头在 **initiator 的 Data-In 路径**而不在本桥：发 64 KiB 给它要 669 µs，而从它那收同样的 64 KiB 只要 306 µs；两个 target 侧优化（BHS+数据合并为一次 `WSASend`、1 MiB socket 缓冲）实测都无效（§8.61）。全程零错误；数据路径 trace 默认关闭（打开慢约 20 倍），调试用 `-iscsitrace`，要每条命令的耗时分解用 `-iscsitime`。裸栈 **1230 / 1179 MiB/s**（写/读，20 轮）。**桥的写路径现在任意尺寸都可用，包括"新会话第一条就写 4 MiB"**，三条独立证据逐字节一致：① Windows initiator 自己读回；② 后端 namespace 文件（显式发 SYNCHRONIZE CACHE → NVMe FLUSH 之后）；③ 目标端 PDU 轨迹。实测 4 MiB @ 0（首写）、8 MiB @ 8 MiB、一个会话内 64 KiB → 128 KiB → 1 MiB → 4 MiB 爬坡、以及 1 MiB + 512 B（非整 burst 尾块，@32 MiB）全部通过。每条 256 KiB 写命令的线形是"即时数据 65536 + 一个 R2T 要剩下的 + 三个 64 KiB Data-Out + `GOOD`/残差 0"，窗口 8 条命令流水——与参考实现（tgt）产生的形状一致。这取代了老的"必须按尺寸爬坡"和"4 MiB 首写撕会话"：装不进命令 PDU 的写现在是**按 ITT 挂起的状态**，不是 `readPdu()` 阻塞循环（§8.64）；老的根因（**协商**，不是 R2T 字节）见 §8.63。两个值得知道的坑：裸磁盘句柄上的 `FlushFileBuffers` **到不了线上**（实测），所以"读回来对"不等于持久化——`src/tools_iscsi_write.ps1` 自己发 SYNCHRONIZE CACHE；`-iscsitrace` 会让墙钟时间变成 2.6 倍，别开着它测吞吐。桥默认只读（拒绝写入前会先把第一段数据排空，会话不会因此被撕开），故第 9 项不受影响 |

第 5、7 项是唯一能发现"我们两端一起写错"的测试——第一次真跑，
它们一共抓出 **11 个缺陷**，其中 7 个是我们两端对同一字段的理解与规范不一致，
而此前 8/8 自检全绿（DESIGN §8.46）。**自检证明内部一致，不证明正确。**

**这两项现在是一条命令**（实测：双向都过，之后 `run_all.ps1` 9/9、137 秒）：

```powershell
.\f5_session.ps1 -LaptopIp <对端> -LaptopUser <用户>        # 链路 → 对端 → 方向 A → discovery → 方向 B
.\f5_session.ps1 -Auth -LaptopIp <对端> -LaptopUser <用户>  # 同一条链路，方向 B 带 DH-HMAC-CHAP
.\f5_session.ps1 -Down                                      # 拆桥 + 回读验证
```

原始输出落在 `src/f5_session_<时间戳>.txt`。把手工步骤固化成脚本的过程里
**又抓出 4 个缺陷**（方向 A 漏传本地 IP、探测对端地址的时机反了、
远程命令串穿过 PowerShell→ssh→bash 会丢引号、方向 B 脚本自己的三个坑），
见 DESIGN §8.47 —— 其中"引号会丢"那一条曾在序列中间崩掉并在对端留下活控制器。

**多 I/O 队列（§8.49）**：target 真的拥有 8 个 queue pair（每条队列独立 capsule ring 与
in-flight 表），initiator 按对端授予数建队列。对端证据：方向 A 是 4/4 条队列各自建连并
同时各跑一条命令；方向 B 是 Linux 主机建 8 条、命令分散在 6 条上。自检 F5 默认就用 4 条队列。

**discovery（§8.50）**：`nvme discover` 能列出我们（对端实测 `DISC: the discovery log names
nqn.2024-01.local.rdma:windows-nd`，本机 target `discLogReads=3`、`controllers=2`），
我们自己的 host 也能用 `-discover` 读 Linux nvmet 的发现日志。discovery 与 I/O 是**两个
控制器**，所以 target 用 `-serve N` 连续服务（默认 1，保持 F7 的"主机消失即退出"断言）。

**DH-HMAC-CHAP（§8.51）**：`-authkey <DHHC-1:..>` 让 target 要求认证（Connect 结果置 ATR bit 17），
认证之前除 fabrics 之外的命令一律回 `0x4191`；host 侧看到 ATR 就自动完成
Negotiate→Challenge→Reply→Success1（有控制器密钥时再加 Success2）。
密码学是自研的 Montgomery 模幂——Windows 的 CNG **实测**拒绝自定义 DH 群
（`STATUS_NOT_SUPPORTED`），也拒绝导入不匹配的私钥（`STATUS_INVALID_PARAMETER`），
而 ffdhe 群参数是从 RFC 7919 原文抓取并逐条验证的。`.\f5_interop.exe -genkey` 生成密钥，
`.\run_f5_auth.ps1` 跑六个用例（含"错密钥必须被拒"和"无视 ATR 必须被 gate 拒"）。

**一个真能用的卷（§8.53）**：`-nsfile F:\x.img` 把 namespace 变成那个文件（几何 = 文件大小，
FLUSH 才回写），于是一台 Linux 主机写进来的字节可以直接在 Windows 侧看到：
```powershell
# Windows：target 用文件当 namespace（-serve 0 = 不限控制器数）
.\f5_interop.exe -target 192.168.100.2 4420 -serve 0 -nsfile F:\nvmeof-ns.img
# 对端（桥接窗口里）：
~/f5_dirb_demo.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd
# 然后在 Windows 上直接读那个文件：偏移 4096 处是主机写进来的文本，
# LBA 3000 / 3100 处是 write-zeroes 与 dsm deallocate 清出来的零。
```

同一轮里 `linux/f5_nvmet_ref*.sh` 把 nvmet 当规格，逐行量出参考答案（日志页 LID 策略、
带数据缓冲的 Get Features 一律 `SGL_INVALID_DATA`、Set VWC 必须拒、DSM 的 AD 位在 CDW11），
并据此修掉了三个"我们两端自测永远一致"的差异 —— 其中包括一条**会把控制器挂死**的：
`nvme persistent-event-log` 触发的 0 字节 RDMA Write 永不完成，把 admin 队列停死，
主机 7.6 秒后 Keep Alive 超时并拆链。

互操作的操作步骤、链路脚本与回滚见 `INTEROP_F5.md`；结论与缺陷清单见 DESIGN §8.46、§8.47、§8.49、§8.50、§8.51、§8.53、§8.54。

**Windows 上的 NVMe-oF 盘符（§8.54）**：`mount_nvmeof.ps1` 让我们自己的 initiator 扮演
Windows 缺失的那个角色（客户端 SKU 没有内置 NVMe-oF initiator）：
```powershell
# 从 Linux nvmet 取回 64 MiB 命名空间 -> 包成 fixed VHD -> 挂成 X:
.\mount_nvmeof.ps1
# 不想要 Linux 时，用我们自己的 target 当源：
.\f5_interop.exe -target 192.168.100.2 4420 -serve 0 -nsfile F:\nvmeof\target-ns.img
.\mount_nvmeof.ps1 -TargetIp 192.168.100.2 -Subnqn nqn.2024-01.local.rdma:windows-nd
# 在 X: 里写文件，然后回推（bytes 走 NVMe-oF/RDMA 回到对端命名空间并 FLUSH）：
.\mount_nvmeof.ps1 -Unmount
```

它是**真的 Windows 卷**（NTFS、资源管理器可见、可读写），但**不是活动块设备**：
写入要等 `-Unmount` 才回推。实测一整圈对得上 —— Linux 读回 64 MiB 的 sha256 与本机镜像
逐字节相同，重新挂载后文件仍在、2 MiB 随机文件校验和不变。

**那种"不是活动块设备"的遗憾，在 §8.55 被去掉了**：底下这一条路给出的是**真的活动磁盘**。

**用户态 iSCSI 桥（§8.55）：让 Windows 自带的 initiator 把远端盘当自己的盘用**

内核驱动那一层我们不做（客户端 SKU 没有 NVMe-oF initiator，用户态也没法把自己塞进卷栈），
所以反过来做：我们自己实现 iSCSI 的**对端**，Windows 用它内核里现成的 initiator 连上来。
`src/nvmeof_iscsi.h` 是那个 target，后端直接调同一进程、同一条队列对上的 NVMe-oF initiator
——没有第二个连接、没有 IPC。

```powershell
# 我们的 NVMe-oF initiator 接到 Linux nvmet，同时把结果用 iSCSI 暴露给 Windows
.\f5_interop.exe -initiator 192.168.100.5 4420 192.168.100.2 `
                 -subnqn nqn.2024-01.local.rdma:linux-nvmet -iscsi 3260
# Windows 自带的 initiator 登录（不需要任何内核驱动）
iscsicli AddTargetPortal 127.0.0.1 3260
Connect-IscsiTarget -NodeAddress iqn.2024-01.com.nvmeof:bridge0 -IsPersistent $false
Get-Disk | Where-Object BusType -eq 'iSCSI'      # -> NVMEOF iSCSI-NVMeoF, Online, GPT
```

从"登录就卡死"到"盘出现"之间一共修了 **7 个 bug**，根因是**收到的 PDU 没有跳过 data segment
的 4 字节对齐填充**（RFC 7143 §11.7 的 DataSegmentLength 不含填充）：discovery 的 login 文本
正好 88 字节（4 的倍数，无填充），**普通会话是 127 字节**，漏掉那 1 字节之后每个 PDU 都错位，
initiator 第二个 Login Request 的 opcode 落进了我们的 flags 字段，被读成 NOP-Out，
于是 Windows 一直等一个不会来的 Login Response。其余六个（单会话串行、MaxCmdSN 不前进、
发送侧同样漏填充、短传输报 residual 0、我自己改多线程时把已注册缓冲换成空 vector、
以及"日志只记发送不记接收"的盲区）逐条记在 DESIGN §8.55。

**1 TB 真盘（§8.56）：Linux 上那块 SSD，变成 Windows 的 `E:`**

```
Get-Disk | ? BusType -eq iSCSI
Number FriendlyName        PartitionStyle OperationalStatus SizeGB IsReadOnly
     3 NVMEOF iSCSI-NVMeoF GPT            Online            931.51       True
分区：300 MB ESP + 16 MB MSR + 931.2 GB NTFS -> E:，顶层 60 项，时间戳是真的
```

- **内容是真的，两次字节级对照**：经 `E:` 读一个文件（`fsutil file queryextents` 拿 LCN，
  加分区偏移算出绝对 LBA 1073520）与 Linux 侧裸读同一 LBA，sha256 都是
  `dec615ae…5dd397`；ESP 引导扇区（LBA 40）两侧也都是 `802b1462…a2452293`。
- **零写入，由盘自己证明**：SMART `Data Units Written` 挂载前后**都是 32884752**，
  而 `Data Units Read` 从 144240893 涨到 144241018。747 条 CDB 里只有**一次**写尝试
  （Windows 想写 NTFS 脏位，LBA 651264），被桥按只读拒成 CHECK CONDITION。
- 只读是**栏杆**不是默认值：`-iscsirw` 不开、MODE SENSE 报 WP、WRITE 在 SCSI 层拒绝
  —— nvmet 没有只读 namespace 属性，只能在这一侧保证。
- 最后一段路是链路：`src/interop_link.ps1 -Action Up` 把一个 CX3 口桥进 LAN 段，
  让对端的软件 RoCE 能 ARP 到；收工用 `-Action Down` + `-Action RestoreMtu` 把绑定还给系统。

全部实测输出（含拆除步骤与踩到的三个坑）见 `EVIDENCE-1TB.md`。

## 装成 Windows 服务

桥是这套东西里唯一"应该一直跑着"的部分：Windows 自带的 iSCSI initiator 连上它，远端盘就出现。
`install.ps1` 负责拷贝 exe、注册服务（自动启动 + 失败重启）并启动；`uninstall.ps1` 负责停服务、
登出 iSCSI 会话、删掉两个目录：

```powershell
cd <repo>\src
# 默认只读；要允许写就加 -ReadWrite
.\install.ps1 -Target 192.168.100.5 -Subnqn nqn.2024-01.local.rdma:linux-nvmet `
              -RdmaLocal 192.168.100.3 -IscsiPort 3260

iscsicli AddTargetPortal 127.0.0.1 3260
# 配服务用 -IsPersistent $true：后端消失时桥会退出并被重启，持久会话才能让
# Windows 在桥重新监听之后自己登录回来。
Connect-IscsiTarget -NodeAddress iqn.2024-01.com.nvmeof:bridge0 -IsPersistent $true
Get-Disk | Where-Object BusType -eq 'iSCSI'

.\uninstall.ps1
```

| | |
|---|---|
| 安装到 | `%ProgramFiles%\nvmeof-windows-nd\`（exe + `bridge.conf`，一行一个参数） |
| 日志 | `%ProgramData%\nvmeof-windows-nd\bridge.log` |
| 服务名 | `nvmeofNdBridge`（可用 `-ServiceName` 改） |
| 命令行 | 整条桥的命令行放在服务的 ImagePath 里，`sc qc nvmeofNdBridge` 可查 |

三点部署前要知道：

- **对端不在时服务会重试而不是退出**：`install.ps1` 会带上 `-backendretry 15`，所以 NVMe-oF
  target 还没起来时服务保持 `Running`，等它出现后自己连上，而不是退出后被 SCM 每 5 秒重启一次。
  已验证：连续三次失败（每次都会释放它占用的设备、队列与注册内存），随后对端出现，桥**无需重启**
  就接上了。
- **中途消失的后端会让桥主动退出，这是有意的**：实测 NVMe-oF target 进程一没，之后每次提交都是
  `ND_CANCELED`，队列对等于永久损坏——把 target 重新起来也没用，盘会一直报错（写
  `Data error (cyclic redundancy check)`，读 `fatal device hardware error`），而桥看上去一切正常。
  现在桥连续三次 NVMe 失败就结束会话并**非零退出**，SCM 因此会重启它、`-backendretry` 会重连
  （DESIGN §8.66）。实测：杀后端 → 18 s 后桥自己退出 `EXITCODE=1`；重启后端与桥 → 会话回来、
  盘 Online、2 MiB 写三条证据逐字节一致。配 `-IsPersistent $true` 的 iSCSI 会话，Windows 会自己
  重新登录，盘自己回来。
- **服务运行时日志文件是被占用的**（`Get-Content` 会报 "used by another process"）。要读就先
  `Stop-Service`，或者把 `-log` 指到别处再拷贝。这是已记录的局限，不是疏忽——见 DESIGN §8.62(2)。
- 服务只跑**桥**，它仍然需要一个 NVMe-oF target：那台 Linux，或者同一个 exe 再开一个 `-target`
  （服务的端到端验证就是这么做的：安装 → 供一个已知图案 → `0 / 65536 字节不符` → `Stop-Service`
  → 干净地 `exiting (rc 0)`）。

## 硬件前提

- 一台 Windows 机器 + 一块支持 RoCE 的网卡（本工程实测：ConnectX-3 Pro，
  HP 544+FLR-QSFP，固件 2.40.5000，WinOF 的 ND 提供者）。
- 自测只要一块双口卡把两口直连即可。
- 互操作需要一个**本地** Linux 对端（普通机器 + 软件 RoCE `rxe` 就够；
  云服务器不行——RoCE 不过路由，见 `INTEROP_F5.md`）。

## 许可

**GNU Affero General Public License v3.0 或更新版本**（`LICENSE`）—— 逐字官方原文，
34,523 字节，sha256 `8486a10c4393cee1c25392769ddd3b2d6c242d6ec7928e1414efff7dfb2f07ef`。

```
Copyright (C) 2026 Dingtaiqi

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
```

**AGPL 允许商业使用**，它管的是"闭源"：

- 公司内部使用、拿它赚钱、做成服务 —— **都可以，免费**。
- 代价是回馈：分发本工程或派生作品（**包括通过网络提供服务**）时必须给出完整对应源码。
  第 13 条（`LICENSE` L540）就是专门管网络服务的那一条，也是 AGPL 与 GPL 的唯一实质区别
  —— 对付"拿开源代码做闭源云服务"靠的就是它。
- 确实需要闭源（嵌进闭源产品、做闭源 SaaS）的公司，可以走 `COMMERCIAL.md` 的商业授权（双授权）。

两个兼容性坑：

1. `ref/` 下两份 Linux 内核头（`linux_nvme.h`、`linux_nvme_rdma.h`）是 **GPL-2.0**，
   **只作对照阅读、不参与编译**。**GPL-2.0-only 与 AGPL-3.0 不兼容**，
   不要把它们的代码并进本工程；实在要并，本工程得整体改成 GPL-2.0。
2. 链接厂商的 NetworkDirect 库（`ndutil`/NDSPI）没有影响——它们不是 copyleft 许可。


