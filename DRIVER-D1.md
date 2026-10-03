# DRIVER-D1 — Windows 原生 NVMe-oF 内核驱动（StorPort 虚拟微型端口）

> 目标：**让 NVMe-oF 的盘由内核驱动直接呈现为 Windows 原生磁盘** —— 不经用户态进程、不经 iSCSI 桥。
> 这是"Windows 原生 NVMe-oF"在这台机器上唯一可能的形态（见 §1 的实测依据）。

---

## 1. 为什么必须自研：实测依据（2026-10-04）

| 检查项 | 实测结果 |
|---|---|
| 系统 | Windows 11 专业工作站版 **25H2**，Build **26200.9457** |
| `nvmeof.sys` | **不存在** |
| `nvmf.sys` | **不存在** |
| `stornvme.sys` / `storport.sys` | 存在（325 KB / 2569 KB） |

**微软的原生 NVMe-oF initiator 只随 Windows Server 2025 提供**；Win11 25H2 上没有可挂接的 NVMe-oF
传输层（没有 `nvmeof.sys`/`nvmf.sys` 可供注册）。因此"原生"= **我们自己写一个 StorPort 微型端口**，
它上接 Windows 存储栈（于是对系统而言就是一块原生磁盘），下说 NVMe-oF。

---

## 2. 架构

```
   Windows 存储栈（分区/文件系统/卷）
              │  SRB（SCSI 请求块）
   ┌──────────▼──────────────────────────────────────┐
   │  nvmeofnd.sys  —— StorPort 虚拟微型端口（我们写） │
   │   · StorPortInitialize / VirtualDevice 模型      │
   │   · SRB ←→ NVMe 命令 翻译（Read/Write/Flush…）    │
   │   · NVMe-oF 协议：fabrics Connect / AdminQ / I/OQ │
   └──────────┬──────────────────────────────────────┘
              │  我们的内核 NDK 通路（D0 已证明可用）
   ┌──────────▼──────────────────────────────────────┐
   │  NDKPI：Adapter / PD / CQ / QP / MR / Read/Write  │
   └──────────┬──────────────────────────────────────┘
              │  RoCE（mlx4eth63 / CX3）
        NVMe-oF target（Linux nvmet，或我们的用户态 target）
```

**为什么用 StorPort 虚拟微型端口**：它是微软为"没有真实硬件的存储设备"（如 iSCSI/虚拟盘）提供的
标准模型 —— 系统会把它当作一块正常磁盘挂载、分区、格式化，不需要我们碰卷管理。

---

## 3. 复用地图（不重写已经验证过的东西）

| 已证明的成果 | 在 D1 中的用途 |
|---|---|
| **D0：内核 NDK 全链路**（适配器/PD/CQ/QP/Listener/Connector + 连接 + 私有数据 + **RDMA Read 逐字节一致**） | D1 的数据面地基，直接复用 |
| D0 的 MR 用法：`IoAllocateMdl` + `MmProbeAndLockPages` + `NdkCreateMr` + `NdkRegisterMr` + token | NVMe-oF 的 SGL/数据缓冲 |
| **D2：用户态 NVMe-oF 协议机**（fabrics Connect、AdminQ、Identify、I/O 队列、**Discovery、DH-HMAC-CHAP**） | D1 的协议逻辑蓝本，逐段移植 |
| D2：`f5_interop -target`（我们的 target）与 Linux `nvmet` | D1 的**对端**（用于验证） |
| `EVIDENCE-1TB.md` 的字节级对照方法（`fsutil file queryextents` + 对端裸读 + sha256） | D1 的验收方法 |

---

## 4. 分阶段与验收判据（每阶段都要有可复跑的证据）

### D1.1 StorPort 骨架（先不碰网络）
- **做**：虚拟微型端口初始化成功；用一块**内存盘**做 LUN；SRB 读写落到内存。
- **验收**：`Get-Disk` 出现我们的磁盘（`FriendlyName` 含我们的标识、`BusType` 为我们的类型）；
  分区→格式化→读写一个文件成功；**重启后仍能加载**。
- **为什么先做这个**：把"存储栈这一侧"与"网络这一侧"解耦。StorPort 的失败模式（超时、复位、队列深度）
  与 RDMA 的失败模式完全不同，混在一起调试会浪费大量时间。

### D1.2 内核侧 NVMe-oF 协议核心
- **做**：fabrics Connect（含 SGL 里的 rkey RDMA Read 回 1024 字节 Connect data）→ AdminQ → Identify
  → I/O 队列建立。全部走 D0 的内核 NDK 通路。
- **验收**：对端（我们的用户态 target 或 Linux `nvmet`）日志显示我们的内核驱动完成 Connect 与 I/OQ 建立；
  我们侧打印出 `cntlid`、namespace 容量（`nsze`）、LBA 格式 —— 与对端 `id-ns` 一致。

### D1.3 I/O 路径
- **做**：SRB(Read/Write) → NVMe 命令 → RDMA 传输 → 完成 → SRB 完成。
- **验收**：**逐字节对照**（`fsutil file queryextents` 取物理块 + 对端裸读 + sha256 一致），
  与 `EVIDENCE-1TB.md` 同一标准。

### D1.4 产品化必需项（D1 期间逐步补齐）
- 卸载安全（**未等待的清理 = `0xCE`**，见 DRIVER-D0 §22）；
- 多队列（NVMe-oF 每 queue 一个 QP）；
- 超时/重连/控制器复位（StorPort 会要求复位，必须实现）；
- 认证（DH-HMAC-CHAP，用户态已实现）；
- 签名与安装包。

---

## 5. 已经付过学费的坑（**不要在 D1 重犯**）

均出自 DRIVER-D0（每条都有转储或实测证据）：

1. **手工构建内核驱动会静默丢掉 WDK 默认开关** → 必须显式 `/guard:cf` + `/GUARD:CF`，否则 CFG 函数表为空，
   提供程序回调我们时直接 fast-fail 蓝屏（**4 次**）。
2. **异步完成上下文绝不能放栈上** → 迟到的完成会写失效栈地址（`0x7E`，**1 次**）。
3. **`STATUS_PENDING` 不是失败** —— 异步接口返回 pending 是正常契约。我曾把它当错误，改掉了唯一能工作的
   accept 次序，白绕数轮。
4. **创建类 NDKPI 接口的对象经完成回调的 `Object` 传出**（`e->X = c.Object`）；用错签名会"成功但对象为 NULL"。
5. **`NdkRegisterMr` 的 `Flags` 决定远端能否读写**：被读方 `ALLOW_REMOTE_READ`(0x2)，落地方 `ALLOW_LOCAL_WRITE`(0x1)。
   传 0 会在完成时回 `0xC0000005`。
6. **私有数据（两段式）**：先查长度、**再按 announced 长度取**；传缓冲区大小会 `SUCCESS + len=0`**静默丢数据**。
   且必须在 **Accept 之前**读。
7. **accept 必须与客户端挂起的 connect 并发**（在连接事件回调里投递）。
8. **同一 `.sys` 文件不能加载第二次**（`sc start` 回误导性的 exit 2）→ 每次运行用唯一文件名。
9. **不在 `DriverUnload` 里做"未等待的清理"**（`0xCE`）。
10. **不强杀持有内核 RDMA 资源的进程**（`NDKPing.sys` 的 `0xCE` 教训）。
11. **改动效果必须读回来才算改动** —— 我有两次编辑静默失配（here-string 的 LF vs 文件 CRLF），
    结果"以为改了、其实没改"；并且**不要无条件打印"已完成"**。
12. **"不可能为真"的输出要第一优先查** —— `qp` 与 `cq` 打印出同一地址时，根因是多删了一行 `Step3Start(&c);`。

---

## 6. 立即的下一步

**D1.1**：写 `src/driver/nvmeofnd.c` —— StorPort 虚拟微型端口骨架 + 内存 LUN，构建、签名、加载，
用 `Get-Disk` 看到我们自己的磁盘。**先把存储栈这一侧跑通，再接通网络。**

---

## 38. D1.1 进展：StorPort 虚拟微型端口**已构建并加载**，但 StorPort 不调用 `HwFindAdapter`

### 已完成

- **`src/driver/nvmeofnd.c`**：StorPort 虚拟微型端口（RAM LUN 48 MiB）。接口全部从
  `storport.h`（WDK 10.0.26100.0）读出后使用：`HW_INITIALIZATION_DATA` 字段表、`SCSI_REQUEST_BLOCK` 布局、
  `StorPortInitialize` / `StorPortNotification` / `StorPortGetSystemAddress` / `StorPortSetDeviceQueueDepth`、
  以及 `PORT_CONFIGURATION_INFORMATION.VirtualDevice`。
- **构建成功**：`cl /kernel /guard:cf` + `link /GUARD:CF storport.lib` → `nvmeofnd.sys`（9 KB），测试证书签名。
- **加载成功**：`sc create type= kernel group= 'SCSI Miniport'` → `STATE: 4 RUNNING`，**无蓝屏**（转储仍是 10/3）。

### 关键发现（埋点实测，不是推测）

驱动内把每一步写进 `HKLM\Software\NVMeoFProbe` 的 DWORD 值，加载后读回：

```
TraceDriverEntry      = 1
TraceStorPortInitRc   = 0x00000000     ← StorPortInitialize 成功
TraceHwStructSize     = 0xD0 (208)     ← 结构体大小正确
TraceFindAdapter      未到达 ✗
TraceInitialize       未到达 ✗
TraceBusChange        未到达 ✗
TraceStartIo          未到达 ✗
磁盘数: 4（与加载前相同，我们的盘没有出现）
```

**结论**：`StorPortInitialize` 接受了驱动（返回 0），但 **StorPort 从不调用 `HwFindAdapter`**。
原因是模型差异：**StorPort 是 PnP 驱动模型** —— 适配器的枚举由 **PnP 设备节点的到达**驱动，而不是像老的
SCSIPORT 那样在 `StorPortInitialize` 里直接回调 `HwFindAdapter`。**裸 `sc create` 一个内核服务不会产生
设备节点**，所以 FindAdapter 永远不被调用。`group= 'SCSI Miniport'` 也不是它需要的条件（试过了，无效）。

> 这与内核 NDK 探针的经验是同一类：**驱动"加载成功"不等于"代码被调用"**。今天第二次靠埋点把
> "跑起来了"和"被调用了"区分开（第一次是 NDK 的 `WskControlClient`）。

### 下一步（D1.2）

要让 FindAdapter 被调用，必须有设备节点，标准有两条路：

1. **INF + 根枚举设备**：写 `nvmeofnd.inf`（存储适配器类、根枚举硬件 ID），
   `pnputil /add-driver nvmeofnd.inf /install` 安装，再用 **`devcon install nvmeofnd.inf root\nvmeofnd`**
   创建根设备节点（`devcon.exe` 来自 WDK Tools，本机尚未确认存在）。
2. **软件设备（推荐，不依赖 devcon）**：写一个小用户态程序调用 **`SwDeviceCreate`**（`swdevice.h`），
   用与 INF 匹配的硬件 ID 创建软件设备节点 —— 这是现代 Windows 上创建无硬件设备的文档化方式。

两条路都需要 **INF 包**（驱动签名 + 类安装）。D1.2 就从写 INF 与软件设备创建工具开始。

---

## 39. D1.1 续：INF 安装链路已打通，设备启动失败（问题码 10）—— 定位到 `VirtualDevice` 的落点

### 已经打通的（每一步都有实测）

| 步骤 | 结果 |
|---|---|
| 驱动构建 + 测试证书签名 | ✅ `nvmeofnd.sys`（9 KB） |
| 作为服务加载 | ✅ `STATE: RUNNING`，**无蓝屏** |
| INF + `devcon install` 创建设备节点 | ✅ `ROOT\SCSIADAPTER\0001`（名称正确显示为 "NVMe-of over NetworkDirect (StorPort virtual miniport)"） |
| **目录文件（.cat）** | ✅ `Inf2Cat` 生成 + `signtool` 签名 → `pnputil /add-driver /install` **成功**，`Published Name: oem149.inf`，"Driver package installed on device: ROOT\SCSIADAPTER\0001" |
| 服务由 INF 正确创建 | ✅ `BINARY_PATH_NAME = \SystemRoot\System32\drivers\nvmeofndx.sys`、`LOAD_ORDER_GROUP = SCSI Miniport` |
| 驱动作为服务手动启动 | ✅ `sc start nvmeofndx` → `RUNNING`（**映像可加载、签名有效**：`Get-AuthenticodeSignature` = Valid，签发者 `CN=NVMeoF Test Driver`） |
| 代码完整性日志 | 干净（只有无关的反作弊驱动记录）→ **不是签名/CI 拒绝** |

### 卡住的地方（诊断精确）

```
devcon status root\nvmeofndx
    Name: NVMe-oF over NetworkDirect (StorPort virtual miniport)
    The device has the following problem: 10          ← CM_PROB_FAILED_START
Kernel-PnP: Device ROOT\SCSIADAPTER\0001 had a problem starting.
埋点: TraceDriverEntry / TraceFindAdapter / … 全部未到达（PnP 路径下驱动根本没跑起来）
```

### 根因判断与下一步（很具体）

`VirtualDevice = 1` 在 Windows 的存储驱动模型里是一个**设备（硬件）级**的设置：StorPort 靠它判断"这是一个虚拟适配器，
不需要硬件枚举"。我把它写进了 INF 的**软件键**：

```
[Nvmeofnd_Inst]
AddReg    = Nvmeofnd_AddReg          ← 软件键（HKR 指软件键）
[Nvmeofnd_AddReg]
HKR,, "VirtualDevice", 0x00010001, 1
```

**正确做法是放进 `.HW` 段**（`HKR` 在 `.HW` 段里指向设备的**硬件键**）：

```
[Nvmeofnd_Inst.HW]
AddReg = Nvmeofnd_HW_AddReg
[Nvmeofnd_HW_AddReg]
HKR,, "VirtualDevice", %REG_DWORD%, 1
```

**下一步**：改 INF → 重新 `Inf2Cat` + 签名 → `pnputil` 重装（或新硬件 ID）→ `devcon restart` →
期望看到 `TraceFindAdapter`、`TraceInitialize`、`TraceBusChange` 到达，且 `Get-Disk` 出现我们的 RAM LUN。

### 顺带记下的两个工具链事实（都踩过）

1. **测试签名模式下，INF 驱动包必须有签名过的 `.cat`**，只签 `.sys` 不够 —— `pnputil` 会明确报
   "The third-party INF does not contain digital signature information"，而 `setupapi.dev.log` 里给出真正原因：
   "Driver package does not contain a catalog file, and Code Integrity is in Test Signing mode. Error = 0xE000022F"。
2. **`Inf2Cat` 的 `DriverVer` 按 UTC 判定"未来日期"**：本机 UTC+8，本地 10/04 的 INF 会被判为未来
   （`22.9.7: DriverVer set to a date in the future`）→ 写**前一天**的日期即可。
3. `Inf2Cat.exe` 在 `bin\<ver>\x86\`（大写 I），而 `devcon.exe` 在 `Tools\<ver>\x64\` —— **不在同一个目录里**。