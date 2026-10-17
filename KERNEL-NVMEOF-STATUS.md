# 内核 NVMe-oF 连接路径崩溃 —— 当前状态与调试方案

## 一句话

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
- **同卡回环被 RDMA-CM 拒绝**（`0xC0000236`，实测 ✓）→ **必须两台机器、两个地址**

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
