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
