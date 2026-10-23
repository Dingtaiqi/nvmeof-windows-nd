# 内核 NVMe-oF 状态与调试记录（ND 阶段 → NVMe/TCP 阶段）

> **先看当前状态**：[NVMe/TCP 阶段（2026-10-18 → 10-21）](#nvme-tcp-阶段2026-10-18--10-21-重写版)。
> 本文件上半部分（直到 `2026-10-18 09:2x`）是 ND/RDMA 阶段的历史记录，保留原样；
> 下半部分是 NVMe/TCP 阶段的总结（原先被编码损坏的追加记录已重写）。
> 日常验收：`tools/verify_after_reboot.ps1`；驱动/目标哈希见下表。

## 一句话（ND 阶段的历史结论）

内核驱动的 **NDK bring-up 全绿** ✓ —— adapter / PD / CQ / QP / Connector / MR / 注册内存
**全部 status 0**，rkey 与 lkey 都到手 ✓（网络关闭时反复验证过）。
但**一旦真正发起连接（`nvmeofk_doconnect=1`）就 `0x139`** ✗ ——
`P1 = 0xa` = **incorrect stack**（栈被破坏），导致**转储里的调用栈完全无法展开** ✗，
**出错指令一直拿不到** ✗。

**定位它需要活的 KDNET 调试会话** ✓ —— 完整步骤见 [tools/KDNET_SETUP.md](tools/KDNET_SETUP.md)。

## 已修并确认的真缺陷（每一条都有实证来源，不是猜测）

| # | 缺陷 | 证据来源 |
|---|---|---|
| 1 | `StepStart` 每次调用都 `KeInitializeEvent`，破坏 dispatcher 等待链表 | 转储实证：`rsi = g_async+0x30`、`WaitListHead.Flink == Blink == 陈旧的 ffffe48a2de0e180`、内核在 `nt+0x25b58f` 执行 `mov r12,[r12]` 且 `r12 = 0` |
| 2 | `NdkRegisterMr` 的 flags 是非法枚举组合 `0xF`（`ALLOW_REMOTE_WRITE` 是 `0x5`，**不是位**） | `ndkpi.h` 的 `#define` |
| 3 | 关闭回调 `closeDone` 声明为 2 参数，而 `NDK_FN_CLOSE_COMPLETION` **只收 1 个参数** | `ndkpi.h` |
| 4 | 释放路径 `WskReleaseProviderNPI` 在 NDK 对象仍打开时**永久挂死**（驱动停在 `Start Pending`，且**挂死不产生转储** ✗） | `sc query nvmeofk9` + 断点文件 |
| 5 | `NdkGetConnectionData` 的 `pPrivateDataLength` 是 **IN/OUT**（输入时必须给出缓冲区大小），原代码传了**未初始化栈变量** → 提供程序按垃圾长度写 64 字节栈缓冲区 | `ndkpi.h` + `0x139 P1=0xa` 完全吻合 |
| 6 | teardown 里连续 5 次复用同一上下文（与 #1 同类） | 代码审计 |

## 一条必须遵守的纪律：**每次测试前重启**

驱动**故意不释放任何 NDK 对象** ✓（对齐 D0 —— D0 只跑一次就成功，从不重复 ✗）。
结果是：**约十次运行之后，NDK 提供程序的状态被泄漏的适配器污染到"第一次创建就崩"** ✗✓。
这是"泄漏一切"策略在**重复测试**下的必然结果 —— 而**正确的清理路径**（用头文件里那个
1 参数的关闭回调 ✓）还没有实现 ✗。

## 硬件拓扑（这一夜查清的关键事实，别再搞错）

- Windows 这台：Mellanox CX3 双口，**两个 40G 口是自环互插的一对**
  （收发计数完全互补：24 收 2233/发 75006，23 收 75006/发 2233 ✓✓）
- `以太网 24` = **192.168.100.2**（独立、RDMA ✓）—— initiator 用这块
- `以太网 23` = **网桥成员** —— 它是**通往局域网那条路** ✓，**不可动** ✗
  （我曾两次试图给它配 IP ✗，都被 Windows 在开机时回滚 ✓ —— 幸好如此）
- 网桥 = `192.168.1.2`（成员：`以太网 20` Intel I211 + `以太网 23`）
- 对端 = **EndeavourOS 笔记本 + 千兆网卡上的软件 RoCE（`rdma_rxe`）**，地址 **192.168.100.5**
  —— 它**没有** CX3，**插不了 PCIe 网卡** ✗（本项目文档写得很清楚，我一开始却凭空假设它有 ✗）
- ~~**同卡回环被 RDMA-CM 拒绝**（`0xC0000236`）→ 必须两台机器、两个地址~~
  **已推翻**（2026-10-18 01:00 实测）：同卡自环的 RDMA-CM 握手是通的；当时失败是因为
  目标端 `f5_interop` 缺 `ndv2.dll` 没起来。真正挡住 RDMA 的是 hypervisor 下
  `NdkOpenAdapter` 返回 `0xC0010011`（详见下文 NVMe/TCP 阶段时间线）。

## 可用工具（都在 `tools/`）

| 工具 | 用途 |
|---|---|
| `analyze_crash.ps1` | 从 WER 报告直接**点名出错模块**（约 1 秒，**不需要调试器** ✓） |
| `analyze_dump_offline.ps1` | `cdb -z` **离线**解析转储（约 3 秒，**不碰活动内核，安全** ✓） |
| `run_kernel_nvmeof.ps1` | 一键跑内核 NVMe-oF（**自动实时读取 ifIndex**，不硬编码 ✓） |
| `KDNET_SETUP.md` | **活调试会话**的完整搭建步骤（含 `USB-KD` 等退路 ✓） |
| `sym/nvmeofk21.*` | 带符号的驱动 + PDB + MAP（含 **9 个增量日志落盘点** ✓ —— 以后即使崩溃，注册表 `HKLM\Software\NVMeoFProbe\nvmeofk_log` 里也会留下它走到哪一步 ✓） |

---

## 2026-10-18 01:00–01:25 状态更新（这一节比上面的旧内容新，先看这里）

### 已实测

- **同卡自环的 RDMA-CM 握手是通的**：内核 listener 绑在 `192.168.100.2:4420`
  （`NdkGetLocalAddress` 回读 family=2 port=4420 addr=192.168.100.2），
  内核 connector 从 `192.168.100.3` 连过去：connect event 到达、
  `NdkAccept` 完成 0x0、`NdkCompleteConnect` 完成 0x0。
  旧结论"同卡回环被 CM 拒绝（0xC0000236）"无效——那时目标端 `f5_interop`
  因缺 `ndv2.dll` 根本没起来（`0xC0000141`），`.2:4420` 上没人监听。
- **第一个数据路径 `NdkSend` 在 provider 内部同步挂死**：无 CQ 完成、无错误、
  无超时；4 分钟后服务仍是 `START_PENDING`。证据：
  [evidence/NDK-LOOPBACK-2026-10-18.md](evidence/NDK-LOOPBACK-2026-10-18.md)、
  [evidence/ndk-loopback-d4.log](evidence/ndk-loopback-d4.log)。
- `NdkGetConnectionData` 的缓冲给 64 字节会得到 `0xC0000023 = STATUS_BUFFER_TOO_SMALL`
  （provider 要 148 字节）；给 1 KiB 后 `NdkCompleteConnect` 立即完成。

### 已修、已编译、**尚未部署**（都要重启才生效）

- `nvmeof_tcpk.c`：`c->broken` 不再是一向锁存。协议错误后下一条命令走
  `NvtcpkReconnect`（关掉失步的 socket、重建、重做握手、清标志），而不是
  拒绝到重启。poisoned（WSK IRP 超时）仍然拒绝，这是故意的。
- `nvmeof_tcpk.c`：8 KiB 传输日志不再"满了就丢新行"，改为丢掉最旧的一半并继续写；
  刷新时显式补 REG_SZ 的 NUL。
- 新 `sym\d2\nvmeofnd2.sys`：SHA256 `B40537DC8448B4192082F50E23CC368EECE76DD79D91362CA1F392FF6A5D2263`
  （53,248 字节）。**运行中的仍是 `07EF388C75E5D85043F7180EECE538914EDCD77AF94419D10965794AB3F658BA`。**
  驱动有活动连接，按 0xCE 铁律必须"重启让 Windows 自己加载"。

### 已修并已用测试钉住

- `fillIdentifyNamespace` 的 LBAF 偏移其实在 `7d7c355` 就修好了（LBADS 在 130）。
  这次把它移到 `nvmeof_tcp.h`（`nvmeofTcpFillIdentifyNamespace`），让
  `tcp_identify_selftest.c` **直接测生产函数**而不是它的副本；
  测试读回线上字节断言 `ns[130] == 9`。`58 pass / 0 fail`。

### 机器现状（需要重启）

- 3 个测试驱动卡在 `START_PENDING`：`nvmeofkloop010118`、`nvmeofkloopd3b`、`nvmeofkloopd4`
  （`sc stop` → 1052，NOT_STOPPABLE；镜像被映射，文件也覆盖不了）。
- 它们之后 **WMI 查询开始 20 秒超时**；普通 shell 与 SCM 仍正常。
- 重启前不要再加载 `nvmeofk*` 内核实验：每次挂死都会再留一个不可回收的内核线程。

### 重启后的顺序（建议）

1. 自检（交接文档里的那五条）+ 确认 `nvmeofnd2` 仍是旧 hash；
2. 用 SYSTEM 计划任务部署新 `nvmeofnd2.sys`（`tools/deploy_driver_via_system_task.ps1`），
   比对大小 + SHA-256，再重启一次让它加载（或部署后直接重启，两次重启变一次）；
3. 验证 `Z:` 仍能在新驱动下读写（协议错误恢复 / 日志滚动都要靠长会话才能观察到）；
4. 再做 RDMA 数据面实验（先试"连接完成后延迟 1–2 秒再 Send"、"先 RDMA Read 再 SEND"、
   "两个 QP 放同一个 adapter"）；
5. 最后才是 #4 大传输（H2CData/C2HData/R2T）。

---

## 2026-10-18 01:21 蓝屏与恢复（实测，按发生顺序）

### 蓝屏

- `01:21:31` 系统蓝屏，`01:23` 重启。事件 1001：
  **`0x00000139 (0x000000000000000a, 0, 0, 0)`**，即
  `KERNEL_SECURITY_CHECK_FAILURE` / `FAST_FAIL_INCORRECT_STACK`。
  转储：`C:\Windows\Minidump\101826-27953-01.dmp`（2,889,551 字节，01:23:04）。
- 这是 `nvmeofk.c` 里记录过的同一种延迟性故障：NDK 连接/发送在 provider 里被卡住后，
  大约几分钟内 `mlx4eth63` 会以 `0x139 P1=0xa` 收场（栈不可展开）。
  本次三个测试连接（4420/4425/4426）分别在 01:01、01:07、01:11 卡住，
  01:21 蓝屏，时间线吻合。
- **不是 `nvmeofnd2` 的问题**：它是 TCP StorPort 微端口，本次实验没有加载/替换它；
  蓝屏后部署盘上的 `nvmeofnd2.sys` 仍是旧 hash `07EF388C...`。

### 重启后 `Z:` 消失，以及真正的原因

- 重启后 `Z:` 不见了；`ROOT\HDC\0002`（NVMe-oF/TCP 微端口）状态 `Problem`，
  code 10 `CM_PROB_FAILED_START`，problem status `0xC0000001`。
- 两个开机计划任务都跑了（`LastTaskResult = 0`），但 `nvmeof_tcp.exe` 起不来，
  日志只有 `cannot open namespace`。
- 根因不是文件损坏，也不是路径写错：**`D:\nvmeof` 被本机安全软件（火绒）保护**。
  实测（同一台机器、同一用户）：
  - 签名的 PowerShell 能写入 `D:\nvmeof\probe-ps.txt`（成功）；
  - 未签名的 `fopentest.exe` 对 `D:\nvmeof` 下的**任何**文件（包括新建的）
    `fopen("r+b")` 都返回 `errno 13`（EACCES），`fopen("w+b")` 同样失败；
  - 同一个 `fopentest.exe` 对 `D:\rdma` 下的文件读写正常；
  - 以 SYSTEM 通过计划任务运行 `nvmeof_tcp.exe` 时，对 `D:\nvmeof\bridge-ns.img`
    同样报 `cannot open namespace`。
  结论：**`nvmeof_tcp.exe` 是未签名程序，火绒禁止它在 `D:\nvmeof` 里写文件**。
  蓝屏前它一直能写，是因为它早已持有文件句柄；重启后重新 `fopen` 就被拒绝。

### 恢复步骤（已执行，`Z:` 已回来）

1. 把命名空间镜像复制到不受保护的路径，并比对哈希：
   `D:\nvmeof\bridge-ns.img` → `D:\rdma\nvmeof-ns.img`，
   SHA256 都是 `E7180E162618C3FB00ACC3AE0EB1DB619470F5D35125496BC00D307473975DBE`。
2. 起 target：`nvmeof_tcp.exe -target 127.0.0.1 4421 -nsfile D:\rdma\nvmeof-ns.img -blocks 32768 -serve`
   （pid 31260，监听 4421）。
3. 起 bridge：`iscsi_tcp_bridge.exe -listen 127.0.0.1 3260 -target 127.0.0.1 4421 -lun 0 32768`
   （pid 6904，监听 3260；它自己的 Identify 读到 `nsze=32768 blocks`）。
4. `pnputil /restart-device "ROOT\HDC\0002"` → `Device restarted successfully`，
   设备状态 `Started`，`Z:` 出现。
5. 验证：`Z:\roundtrip.bin` 的 SHA256 仍是
   `C0DC81BA3F03D8C865E1CF0D400EB298EF7DD3E419788D603CF45704A1455D9B`；
   新写 64 KiB 再读回，**逐字节相同**；新建/删除文件正常。
6. 开机脚本两份都改了（仓库 `tools/boot_4421.ps1` 与计划任务实际执行的
   `D:\rdma\boot_4421.ps1`），`-nsfile` 指向 `D:\rdma\nvmeof-ns.img`；
   并用 SYSTEM 计划任务实测过：target 能打开新路径并监听。

### 遗留

- `D:\nvmeof\bridge-ns.img` 是**旧的 16 MiB 镜像**（内容与复制时相同，之后 `Z:` 的写入
  只落在新文件上）。如果以后修好火绒的规则（把 `nvmeof_tcp.exe` 加入信任/排除），
  可以再搬回去；在那之前，开机路径以 `D:\rdma\nvmeof-ns.img` 为准。
- 三个 `nvmeofkloop*` 服务重启后变成 STOPPED（不再是 START_PENDING），
  可以 `sc delete` 清掉；驱动镜像仍在 `sym\k\` 作为证据。
- 新 `nvmeofnd2.sys`（`B40537DC...`）仍未部署，按 0xCE 铁律等重启时加载。

### 重启前状态（2026-10-18 01:45，操作者确认重启）

- 新 `nvmeofnd2.sys` 已暂存：`C:\Windows\System32\drivers\nvmeofnd2.staged.sys`，
  SHA256 `B40537DC8448B4192082F50E23CC368EECE76DD79D91362CA1F392FF6A5D2263`。
- `MoveFileEx(staged, nvmeofnd2.sys, DELAY_UNTIL_REBOOT|REPLACE_EXISTING)` 返回 **True**；
  `PendingFileRenameOperations` 末尾已出现：
  `*1\??\C:\Windows\System32\drivers\nvmeofnd2.staged.sys` /
  `*1!\??\C:\Windows\System32\drivers\nvmeofnd2.sys`（`!` = 目标已存在则替换）。
- `D:\nvmeof` 仍被拦：操作者在火绒里"信任 D 盘"后，**以 SYSTEM 计划任务运行
  `nvmeof_tcp.exe` 打开 `D:\nvmeof\bridge-ns.img` 仍然 `cannot open namespace`**。
  所以开机路径保持 `D:\rdma\nvmeof-ns.img`。
- 重启后按顺序验证：
  1. `(Get-FileHash C:\Windows\System32\drivers\nvmeofnd2.sys).Hash` 应为 `B40537DC...`；
  2. `sc query nvmeofnd2` → RUNNING；`pnputil /enum-devices /instanceid "ROOT\HDC\0002"` → Started；
  3. `Z:` 出现，`Z:\roundtrip.bin` 的 SHA256 应为 `C0DC81BA3F03D8C865E1CF0D400EB298EF7DD3E419788D603CF45704A1455D9B`；
  4. 若 `Z:` 没出现：先确认 4421/3260 在监听（开机任务），再
     `pnputil /restart-device "ROOT\HDC\0002"`。

---

## 2026-10-18 09:2x 重启后的结果（新驱动已加载，`Z:` 已恢复）

### 结果

- 重启时 `nvmeofnd2.staged.sys` 被成功替换成 `nvmeofnd2.sys`，staged 文件已消失，
  `PendingFileRenameOperations` 里的两条 nvmeof 记录也已被消费。
- **新驱动现在真的在跑**：`ROOT\HDC\0002` = `Started`，`nvmeofnd2` = `RUNNING`，
  部署文件 SHA256 = `C2A87053EFACDC7CB60A3EB92F7AC49F9A052150068E287CEE069250FFF1CC56`。
- `Z:` 回来，3 个文件都在；`roundtrip.bin` 的 SHA256 仍是
  `C0DC81BA3F03D8C865E1CF0D400EB298EF7DD3E419788D603CF45704A1455D9B`；
  新写 64 KiB 再读回，**逐字节相同**。

### 两个必须记住的坑

1. **构建出来的驱动是未签名的，部署前必须签名。**
   第一次重启后设备直接 `Problem 52 (CM_PROB_UNSIGNED_DRIVER, 0xC0000428)`，
   文件 hash 是未签名的 `B40537DC...`。补签（`signtool sign /fd sha256 /s My
   /n "NVMeoF Test Driver"`）后 hash 变成 `C2A87053...`；因为驱动没在运行，
   文件当时可写，直接覆盖 + `pnputil /restart-device` 就恢复了，**不用再重启**。
   现在 `src/driver/build_d2.cmd` **自带签名步骤**，构建完就是签好的。
2. **SYSTEM 计划任务里不能用 PowerShell `Start-Process -RedirectStandardOutput
   -WindowStyle Hidden` 来起 target/bridge。** 本机 A/B 实测（2026-10-18 09:22）：
   同样以 SYSTEM 运行，`Start-Process` 版本**无输出、无进程、无监听**；
   换成 `cmd /c start "" /b "<exe>" args > log 2>&1` 就正常打印并监听。
   这就是为什么之前每次重启后 target 都起不来、微端口 problem 10。
   `tools/boot_4421.ps1` 与 `tools/boot_bridge.ps1` 已改成 `cmd /c start`，
   并在 SYSTEM 计划任务下实测通过（4421/3260 都在监听）。

### 火绒

操作者在火绒里"信任 D 盘"后，**SYSTEM 计划任务运行 `nvmeof_tcp.exe` 打开
`D:\nvmeof\bridge-ns.img` 仍然 `cannot open namespace`**。所以命名空间继续放在
`D:\rdma\nvmeof-ns.img`，开机脚本也指向那里；`D:\nvmeof\bridge-ns.img` 仅作旧副本保留。

## NVMe/TCP 阶段（2026-10-18 → 10-21）—— 重写版

> 说明：本节原本是逐条追加的流水记录，但第 221 行之后被一次 **ASCII 编码的追加**
> 破坏（所有中文变成 `?`）。这里按事实重写为总结；更细的原始日志以 git 历史和
> `evidence/` 下的文件为准。

### 一、当前状态（2026-10-21）

- 内核 NVMe/TCP 微端口（`nvmeofnd2.sys`）+ 用户态 target（`nvmeof_tcp.exe`）在本机回环上
  **完整可用**：`Z:` 是 256 MiB 的 NVMe/TCP 卷，StorPort 盘、GPT、NTFS 都正常。
- 实测吞吐：**4 线程 × 512 KiB 命令、长跑（每线程 100 MiB I/O）聚合 203–233 MB/s
  （中位 ~223）**；单线程 512 KiB 写 ~63 / 读 ~82 MB/s；4 KiB 单线程只有 ~21/20 MB/s。
- 并发：`nvmeofnd_queues=8`（8 条连接、8 个 worker）；**4 路并发是甜点**，8 线程反而 ~195。
- 命令粒度：`nvmeofnd_chunkkb=512`（上限 512 KiB）；数据走 out-of-capsule 的
  C2HData/H2CData，单个数据 PDU 上限 **62 KiB（63488）**。
- 单连接流水线（批量）：**n=1 可用，n≥2 会挂死系统**，已暂停；默认
  `nvmeofnd_pipelinedepth=1`，且**启动强制归 1 并写回注册表**。
- 验收：`tools/verify_after_reboot.ps1` **0 失败**。

当前部署哈希：

| 组件 | SHA-256 |
|---|---|
| `nvmeofnd2.sys` | `5C16EFE4…AE350` |
| `nvmeof_tcp.exe` | `0C389764…0DEE` |
| `iscsi_tcp_bridge.exe` | `275E1138…63F7B` |
| `Z:\roundtrip.bin` | `172C15DC…28FD` |

### 二、时间线（按事实，含教训）

#### 2026-10-18 凌晨：NDK/RDMA 阶段收尾

- 同卡回环的 RDMA-CM 握手其实是通的；旧结论"同卡回环被 CM 拒绝（0xC0000236）"无效——
  当时目标端 `f5_interop` 因缺 `ndv2.dll` 没起来，`.2:4420` 上没人监听。
- 但第一个 `NdkSend` 在 provider 内部**同步挂死**（无 CQ 完成、无超时），几分钟后
  `mlx4eth63` 以 `0x139 P1=0xa` 蓝屏。
- 结论：这台机器上 mlx4/WinOF-5.50 的 NDK 提供程序在 `NdkOpenAdapter` 就返回
  `0xC0010011`（hypervisor 相关）——**RDMA 路线等 CX-4 硬件**，NVMe/TCP 继续。

#### 2026-10-18 上午：恢复与火绒

- 重启后 `Z:` 消失，根因不是文件损坏，而是 `D:\nvmeof` 被火绒保护，SYSTEM 任务
  也打不开命名空间 → 命名空间迁到 `D:\rdma\nvmeof-ns.img`。
- **永远不要 `pnputil /restart-device` 一个还有活动连接的微端口**——12:03 实测
  `0xCE`（P2=0x10，unload with pending operations）。

#### 2026-10-18 中午：out-of-capsule 与 64 KiB 数据损坏

- 目标端改为**数据 PDU 先于 CapsuleResp**；此前顺序相反，内核提前返回、把 16 个
  C2HData 留在 socket 里，NTFS 读到错位数据 → `Z:` CRC 错误。修复后
  `chkdsk Z: /f /x` 扫描无问题，`roundtrip.bin` 哈希恢复。
- 客户端（bridge、bigio selftest）改成**两种 PDU 顺序都接受**。

#### 2026-10-18 下午：PDU 从 4 KiB 到 62 KiB

- 4 KiB PDU 时 64 KiB 传输要 16 个 PDU、~33 个 IRP，只有 37 MB/s；改成 64 KiB PDU
  后 IRP 降到 2 个，仍只有 ~37 MB/s。
- 根因：64 KiB PDU = 65560 字节，**超过 64 KiB 的 socket 窗口**，触发零窗口停顿。
- 先试 1 MiB socket 缓冲；后来发现内核侧 `WskSetOption(SO_RCVBUF/SNDBUF)` 返回
  `0xC000000D`（参数无效），**缓冲从未真正加大**——真正解决问题的是把数据 PDU
  **封顶在 62 KiB（63488）**：60/62 KiB 能跑 57–69 MB/s，64 KiB 掉到 13–36。
- 最终 target / 微端口 / bridge 三处的数据 PDU 上限统一为 62 KiB。

#### 2026-10-18 傍晚：命令粒度与并发

- 512 KiB 命令：单线程写 63 / 读 82 MB/s（4 KiB 时只有 21/20）。
- 4 条连接 / 4 个 worker：137.7（1 连接）→ **196.6 MB/s**（4 连接），+43%。
- 8 条连接：没有额外提升（160–196，噪声内）；**4 路并发是甜点**，8 线程降到 ~195。
- 目标端两处优化（定位 I/O、每 PDU 一次 send）**没有可测量的提升**；目标端进程
  在 200 MiB 并发 I/O 中 CPU 只有 1.4 s，不是 CPU 瓶颈。
- 测试方法教训：**10 次迭代的数字偏悲观**（每轮文件创建/冷缓存），50 次迭代才看到
  真实聚合（203–233 MB/s）。

#### 2026-10-18 深夜：Z: 从 16 MiB 扩到 256 MiB

- 备份旧镜像 → `SetLength` 追加扩展到 256 MiB（数据保留）→ 开机脚本 `-blocks 524288`
  → 重启让微端口重新 Identify → `Resize-Partition` 扩分区。
- 结果：卷 268,349,440 字节，数据完好（`roundtrip.bin` 哈希不变）。

#### 2026-10-18 深夜 → 10-21：单连接流水线（批量）实验

- 传输层 `NvtcpkBatch`：先发 n 个胶囊（写命令连同数据），再按 cid 解复用收响应；
  微端口 `nvmeofnd_pipelinedepth` 让 worker 一次取多个 SRB 批量提交。
- **n=1 完全正常**（4 KiB、32 KiB out-of-capsule 都逐 PDU 验证过）。
- **n≥2 三次把系统挂死**：v1 混合方向 512 KiB、v2 小负载、v3 已做方向隔离
  （一批只允许同 opcode）仍挂死。挂死是系统级的（进程创建都阻塞），只能硬重启；
  挂死后 `Z:` 变 Dirty，`chkdsk /f /x` 每次都能修好。
- 安全措施保留：深度默认 1、启动强制归 1 并写回、运行时每秒可改；批量失败一律
  回退单命令路径。
- 结论：没有 WinDbg 内核调试不再盲试；批量代码保留但默认关闭。

### 三、必须记住的规则

1. **不要重启设备**：`pnputil /restart-device` = `0xCE`；改旋钮只能重启系统。
2. **数据 PDU 必须先于 CapsuleResp 发送**，否则内核提前完成、数据留在 socket。
3. **数据 PDU 封顶 62 KiB**：65536 字节边界会撞 socket 窗口。
4. `WskSetOption(SO_RCVBUF/SNDBUF)` 在连接 socket 上无效（`0xC000000D`），别再指望它。
5. 开机自保：`nvmeofnd_chunkkb` 每次启动重置为 4 KiB；`nvmeofnd_pipelinedepth`
   重置为 1 并写回；`nvmeofnd_queues` 只在 attach 时读。
6. 构建 ≠ 部署：SYSTEM 任务复制 + 哈希比对，`MoveFileEx(DELAY_UNTIL_REBOOT)` 暂存，
   重启加载。
7. **写文档一律 UTF-8**：本文件第 221 行之后就是被 ASCII 追加毁掉的。
8. 长跑测试用 50 次迭代；10 次的数字会被冷缓存和文件创建拖低 10–20%。

### 四、相关文档

| 文档 | 内容 |
|---|---|
| [README.md](README.md) / [README.zh-CN.md](README.zh-CN.md) | 项目总览、构建与运行 |
| [DESIGN.md](DESIGN.md) | 协议与驱动的详细设计（ND 阶段为主） |
| [DRIVER-D0.md](DRIVER-D0.md) / [DRIVER-D1.md](DRIVER-D1.md) | 驱动 bring-up 记录 |
| [ROADMAP.md](ROADMAP.md) | 路线图 |
| [HYPERVISOR-WAS-THE-CAUSE.md](HYPERVISOR-WAS-THE-CAUSE.md) | hypervisor 下 NDK 失败的定位 |
| [evidence/](evidence/) | 原始测试日志与复现记录 |

---

## 2026-10-22/23 IRP 修复、256 KiB C2H、以及"墙在本机回环"的定性

### 修复与实测

1. **`WskSetOption` 需要 IRP**（`13a4190`）：WSK 与 Winsock 不同，每个下投调用都必须给
   调用者分配的 IRP；传 `NULL` 得 `0xC000000D`。补上 IRP 后实测
   **`SO_RCVBUF = 1 MiB` 真正生效（0x00000000）**，`SO_SNDBUF` 返回
   `0xC00000BB`（NOT_SUPPORTED）——文档里"内核缓冲从未生效"的旧结论按此修正。
2. **chunk 上限 512 KiB → 1 MiB**：`MaximumTransferLength` 本来就是 1 MiB，纯拆分浪费。
   实测**无提升**（4 线程 177/187 vs 169/192），并由此推翻"每命令固定延迟"假设。
3. **C2H PDU 62 → 256 KiB**（`bee3a48`，按方向拆分：发送仍 62 KiB）：单线程读
   82 → 78–98 MB/s，只有 ~15%；4×PDU 减少换来不到 20%，**"每 PDU ~775 µs"模型被证伪**。
   自测 18/0、104/0；数据逐字节校验通过。

### 平台基线（`src/tcp_bwprobe.c`，裸回环 TCP，无任何 NVMe）

| 块大小 | 单流吞吐 |
|---|---|
| 4 KiB | 73 MB/s |
| 64 KiB | **147 MB/s** |
| 1 MiB | ~142 MB/s |

**结论：本机 127.0.0.1 同步往返的 TCP 每流封顶 ~150 MB/s、总封顶 ~200–300 MB/s**
（4 流 180–200，bridge 8 读 230）。我们的栈单流 75–98 = 裸平台的 **50–65%**。
在此机器、此路径、同步请求-响应协议下，~90 MB/s/流、~200 MB/s 聚合
**已接近平台包络**；PDU/命令尺寸调整不会再有数量级收益。

### 剩下的真杠杆（按预期收益排序，全部未验证）

1. **改走 CX3 自环双口**（target 绑 192.168.100.2）：绕开回环的线程乒乓，
   TCP offload + RSS + 真 DMA 是另一条包络；先用 `tcp_bwprobe` 测该路径基线。
2. **重叠往返**：IOCP（target）/ 多 IRP 在飞（内核）——本质是流水线，
   需先用 CrashOnCtrlScroll + 离线 cdb 根因批量挂死。
3. 8 线程聚合回退（~195 vs 223）：`eng->Lock` 全引擎一把自旋锁，可按队列分片。
| [tools/verify_after_reboot.ps1](tools/verify_after_reboot.ps1) | 一键验收 |

---

## 2026-10-23 CX3 真线定性：单机不可行；4 路裸回环 = 358 MB/s

### 三个实测结论

1. **本机对本机 IP 的 TCP 永远不上网卡**：`tcp_bwprobe` 在 `.2 <-> .3`（CX3 自环双口）上
   的数字与 127.0.0.1 完全一致（72.8/146.8/148.7 vs 72.9/147.2/142.3），而传输前后
   两张网卡的计数器纹丝不动（256 MiB 全走了 IP 层内部捷径）。
   ND 时代双口计数器互补是因为 NDK QP 绑定适配器、绕过 IP 层——TCP 做不到。
   **"CX3 真线跑 NVMe/TCP"在单台上结构性不可行，此路正式关闭。**
2. **4 路并发裸回环：每流 ~89–91 MB/s，合计 ~358 MB/s**（单流 147）。
   机器的回环总包络随并发增长。
3. **我们的栈在 4 并发下 180–200 MB/s = 裸包络的 ~55%**：每流 ~48 vs 裸 ~90。
   差距 ~2 倍，来自每个命令的额外往返（胶囊 + 每 PDU 一个 IRP + SRB 路径）。

### 修正后的杠杆排序

| # | 杠杆 | 预期 | 状态 |
|---|---|---|---|
| 1 | **每命令单 IRP 接收**（一次收满整个响应流再离线解析 PDU，甚至直收进 SRB 缓冲省一次拷贝） | 4 并发 200 -> 300+ | 未做，内核传输层中等改动 |
| 2 | Linux VM 做 nvmet-tcp 第三方互操作（VM 桥接到 CX3 口） | 正确性背书（perf 不保证走真线） | 未做 |
| 3 | 批量流水线（需先根因挂死：CrashOnCtrlScroll + 离线 cdb） | 单流重叠往返 | 挂起中 |
| 4 | `eng->Lock` 按队列分片 | 8 线程回退修复 | 未做 |