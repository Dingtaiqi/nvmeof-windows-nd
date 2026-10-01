# 在 Windows 上自己实现 NVMe-oF —— 设计

目标：用我们自己的 NetworkDirect 栈（`D:\rdma\rdmaio`，已在 CX3 Pro / RoCE v2 上跑到
2.53 GB/s）实现 NVMe-oF，**不用第三方免费阉割版**。

---

## 0. 为什么这条路和之前那轮调研完全不同

上一轮我去找"免费的 Windows NVMe-oF"，结论是：Windows 客户端没有 in-box 栈，
唯一免费的第三方（StarWind）不声称支持 mlx4/CX3，而所有 VM 路线被
`Virtualization Enabled In Firmware: No`（BIOS 里 AMD SVM 没开）堵死。

**方向错了。** 我们不需要找一个现成的 NVMe-oF —— 我们需要**写一个**。
NDSPI 已经具备 NVMe-oF/RDMA 需要的全部原语（下面的对照表），
而 `rdmaio` 里已经有可用的连接/注册/双面 Send-Recv/单面 Read-Write 基础设施。

所以本工程的两端**都自己实现**，本机两口直连自测。这不是退而求其次：
自研两端 + 后续用 Linux `nvmet` 做独立互操作验证，比接一个不支持的第三方产品更可控。

---

## 1. 原语对照：NVMe-oF/RDMA 需要什么 vs NDSPI 有什么

| NVMe-oF/RDMA 需要 | NDSPI 提供 | 现状 |
|---|---|---|
| 双面 Send/Recv 传 capsule | `IND2QueuePair::Send/Receive` | ✅ 已有（`rdma_transfer.cpp` 文件传输路径在用） |
| 单面 RDMA Read/Write 搬数据 | `IND2QueuePair::Read/Write(remoteAddress, remoteToken, flags)` | ✅ 已有（共享内存路径在用） |
| STag / rkey（SGL 里的 key） | `IND2MemoryRegion::GetLocalToken/GetRemoteToken` | ✅ 已有 |
| 内存窗口 + 失效（STag 生命周期） | `IND2MemoryWindow` + `QueuePair::Bind/Invalidate/SendAndInvalidate` | ⚠️ **未用过**，本工程第一次用 |
| 远端读限额（NVMe-oF 要用它算 in-capsule 空间） | `Connect/Accept(inboundReadLimit, outboundReadLimit)` + `GetReadLimits` | ⚠️ 现在传的是 `0`/`16`，等于没用 |
| RDMA 私有数据（协商用） | `Connect/Accept(pPrivateData, cbPrivateData)` | ⚠️ 现在传 `nullptr` |
| 完成队列 | `IND2CompletionQueue` | ✅ 已有 |

**结论：只差"内存窗口"和"私有数据/读限额"三样从未用过的能力，其余都是现成的。**

---

## 2. 协议子集（明确的取舍）

NVMe-oF 1.1a 很大。第一版只做**能让一个真实的 Linux `nvmet` target 接受并完成 I/O**
所需要的那一部分，其余明确列为非目标。

### 做（initiator 侧）

| 阶段 | 内容 | 完成判据 |
|---|---|---|
| F1 | Fabrics 建链：`Property Set`(CC.EN) → `Property Get`(CSTS.RDY) → `Connect`(admin) | 拿到 cntlid，target 回 CQE 且 status=0 |
| F2 | Admin：`Identify Controller`、`Identify Namespace`、`Set Features(NUM_QUEUES)`、`Create CQ/SQ` | Identify 里的 `subnqn` 与 target 一致；IO 队列建立成功 |
| F3 | I/O 单命令：`Read` / `Write`（单 SGL，keyed data block） | 写入后再读出，内容逐字节一致 |
| F4 | 多命令流水：多 SQE 在途 + CQE 按序回收 | 队列深度 ≥ 32 时无丢失、无 CID 错配 |
| F5 | 吞吐：把我们的多命令流水打到 NIC 上限 | 与裸 RDMA Write 基线（2.53 GB/s）对比并给百分比 |
| F6 | 生命周期：`Keep Alive`、`Disconnect`、`Delete SQ/CQ` | 正常路径与对端消失路径都能有界退出（不挂死） |

### 目标（target 侧，用于本机自测）

最小但真实的 target：一块**文件支撑的 namespace** + Admin + I/O + 正确的 SGL 处理。
它必须能拒绝格式错误的东西（错误的 fctype、越界 SLBA、不支持的 SGL 类型），
因为"只会说不会听"的 target 会让互操作测试失去意义。

### 非目标（第一版明确不做，写下来是为了不偷偷省略）

- Discovery 服务 / Discovery Log Page（直连 NQN，不做发现）
- 认证（DH-HMAC-CHAP）、TLS
- 多路径、ANA、namespace 热插拔
- `Compare` / `Write Zeroes` / `DSM` / `Format` / 固件下载
- 元数据（metadata SGL）、PI（端到端保护）
- SRQ、多 QP 负载均衡、CQ 中断模式（先用轮询）

---

## 3. 关键设计决定（以及为什么）

### 3.1 capsule 尺寸从 Identify 里读，不写死

`ioccsz` / `iorcsz` / `icdoff` 是 target 在 Identify Controller 里报的
（偏移 1792 / 1796 / 1800）。写死 "128/16/32" 会在测过的那个 target 上工作、
在别的 target 上悄悄错。**协议自己会告诉你尺寸，就别猜。**

### 3.2 先在单进程内把 wire 格式钉死，再连网

`src/nvmeof_wire.h` + `src/wire_selftest.c` 已经完成，并且：
- 所有结构体尺寸/偏移用 `static_assert` 钉住（`/W4 /WX` 下 C 和 C++ 两种编译都通过）
- 运行时逐字节验证 SGL 描述符（keyed 与 unkeyed）、fabrics 命令行、Connect data、CQE
- 8×256 穷举验证 status 字段的 pack/unpack 往返
- 16×16 穷举验证 SGL type/subtype 的位打包

**已经靠这套测试抓到并修掉了两个真 bug**（见 §4）。

### 3.3 队列模型：一个 admin QP + N 个 I/O QP，各自独立的 SQ/CQ

NVMe-oF 要求每个 I/O 队列有自己的 SQ 和 CQ，并且和 admin 队列分开。
RDMA 传输下每个 queue 对应**一个 QP**（这也是 NVMe-oF/RDMA 与 TCP 传输最大的结构差异）。
所以 `Connect` 命令要发 N+1 次（admin 一次，每个 I/O 队列一次）。

### 3.4 数据面用 RDMA Read/Write，不用 Send/Recv

- **Write 命令**：initiator 作为 requester 发 `RDMA Read`，从 initiator 的缓冲读
  （对 target 来说是从它的视角"读走"数据）。实际方向按规范：host 写数据时，
  target 执行 `RDMA Read` 把数据从 host 拉到 target；host 读数据时 target 执行 `RDMA Write`。
  两种都由 target 发起，用 host 在 SGL 里给的 rkey。
- **因此 initiator 必须用 memory window 把缓冲"注册"出一个 rkey 交给 target**，
  并在命令完成后 `Invalidate`。这就是 §1 里"从未用过"的那个能力。

### 3.5 控制器状态机照抄规范，不自创

`CC.EN` / `CSTS.RDY` 的次序、`Property Set/Get` 的 attrib 编码、
`Connect` 的 `cattr`（SQFLOW）都是互操作的硬要求，必须按规范走。

---

## 4. 已经抓到的两个真 bug（记录，因为它们说明测试是有效的）

### (a) 状态字段的整体错位一格

我最初按"SC = bits 7:0、SCT = bits 10:8"写（照抄 Linux 头文件的宏值）。
自检立刻失败。追下去发现两个公开来源**看起来**矛盾：

- SPDK `spdk_nvme_status` 位域声明顺序 `p:1 sc:8 sct:3 crd:2 m:1 dnr:1`
  → 小端下就是 **p@0, sc@8:1, sct@11:9, crd@13:12, m@14, dnr@15**，
  而且 SPDK 把它和 `status_raw` 放在同一个 union 里，所以这就是**原始位序**。
- Linux 的宏：`SC 0x00ff`、`SCT 0x0700`、`MORE 0x2000`、`DNR 0x4000`。

把 SPDK 的位序**整体右移 1 位**，Linux 的五个宏**全部精确吻合**。
所以 Linux 的常量定义在 `status >> 1` 之上，两边其实一致——
而且它们**必须**一致：SPDK 和 Linux nvmet 在生产环境里是能互操作的，
原始位序若有分歧根本不可能互通。

**这是这一个协议里最危险的一个位**（错一格 = 认为所有 target 的所有命令都失败）。
修好后，`nvmeof_wire.h` 里加了 5 条 `static_assert` 把"右移 1 位后必须等于 Linux 的宏"
钉住，以后再改任何一边都会编译失败。

### (b) `_Static_assert` 在 MSVC 的 C 模式下不认

`#define NVMEOF_STATIC_ASSERT(c,m) _Static_assert(c,m)` 在 MSVC C 编译下直接语法错误。
危险的失败模式不是报错，而是**有人为了让它过而把断言删掉**——那样整个头文件就退化成注释。
现在 MSVC 分支用 `static_assert` 拼写，并且自检**同时以 C 和 C++ 编译**，确保断言真的在生效。

---

## 5. 目录结构

```
D:\rdma\nvmeof\
  DESIGN.md              本文件
  src\
    nvmeof_wire.h        wire 格式（已完成，self-test 全绿）
    wire_selftest.c      wire 格式的编译期+运行期自检（已完成）
    stag_smoketest.cpp   NDSPI 原语冒烟测试（本轮新增，见 §8）
    build_stag.ps1       /  run_stag.ps1
    nvmeof_rdma.h/.cpp   NDSPI 之上的 RDMA 传输层（待写）
    nvmeof_admin.h/.cpp  Admin 命令与控制器状态机（待写）
    nvmeof_io.h/.cpp     I/O 命令与数据面（待写）
    nvmeof_target.h/.cpp 最小 target（文件支撑的 namespace）（待写）
    nvmeof_cli.c         命令行（待写）
```

---

## 8. 本轮实测：NDSPI 原语在 CX3 Pro / WinOF 5.50 上的真实行为

`stag_smoketest.cpp` 是两个进程（本机两口直连），逐项验证 NVMe-oF 依赖的原语。
**下面每一条都是跑出来的，不是文档推断的。**

### 8.1 ✅ 能用的

| 能力 | 实测结果 |
|---|---|
| `CreateMemoryWindow` | **成功**（这是整个设计最大的未知，现在确定了） |
| `Bind` 到 MR 的 **64 KiB 子区间** | 成功，`ND_SUCCESS` |
| Bind 的完成项 | **会来**，`status=0`、`type=2`（`Nd2RequestTypeBind`） |
| 窗口 rkey vs MR rkey | **不同**（如 `0x23040178` vs `0x23020178`），STag 是独立命名空间 |
| Connect/Accept 私有数据 | 双向都通，且**精确报出所需长度**（`ND_BUFFER_OVERFLOW` 语义正确） |
| 双面 Send/Recv（capsule 那种小控制消息） | 通 |
| `Invalidate` | 接受请求；完成项不一定来（超时属正常） |

**适配器自报的能力**（`IND2Adapter::Query`，对设计有直接约束）：

```
MaxTransferLength     = 1048576 KiB (1 GiB)
MaxInboundReadLimit   = 16
MaxOutboundReadLimit  = 128
MaxCallerData         = 56      <-- Connect 的私有数据上限，NVMe-oF Connect data 不能挤这里
MaxCalleeData         = 148     <-- Accept 的私有数据上限
MaxReadSge            = 32
AdapterFlags          = 0x10009 (InOrder DMA | MultiEngine | LoopbackConnections)
MaxRegistrationSize / MaxWindowSize = 0xFFFFFFFFFFFF（provider 报全 1 = "未实现该限制"）
```

**结论：`MaxCallerData=56` 意味着 NVMe-oF 的 Connect 私有数据只有 56 字节可用，
而 Connect Data 结构本身是 1024 字节** —— 所以 Connect data 必须走
命令 capsule 里的 SGL 指向的内存，**不能**塞进私有数据。这是设计上必须照做的。

### 8.2 ⚠️ 踩出来的行为（文档没说的）

1. **`Bind` 必须在 QP 已经连上之后发。** 在未连接的 QP 上 Bind 只是排队，
   **永远不产生完成项**。这一条让我连续两轮误判成"provider 不支持 Bind 完成项"。
   ⇒ NVMe-oF 里所有 memory window 操作都必须放在 `Connect` 之后。
2. **`GetPrivateData` 的时机是硬要求。** 服务端必须在 `GetConnectionRequest` 与
   `Accept` **之间**读；客户端必须在 `Connect` 与 `CompleteConnect` **之间**读。
   早了晚了都返回空缓冲区而且**不报错** —— 症状是握手全部 PASS、然后两边 rkey 都是 0。
3. **`GetReadLimits` 在 `Accept` 之后返回 `ND_INVALID_DEVICE_STATE` (0xC0000184)。**
   读限额能设不能查（至少在这个时点）。NVMe-oF 要用它算 `max_rdma_size`，
   得改成"我们自己记住协商值"。
4. **所有 SGE 的 Buffer 必须在 MemoryRegionToken 所指的那块 MR 里面。**
   我犯了这个错**两次**（控制消息一次、数据缓冲一次），症状是 provider 返回
   **原始 `0xC000003E`** —— 一个在 `ndstatus.h` 里**根本不存在**的状态码，
   既查不到也没法搜。⇒ NVMe-oF 的 capsule 缓冲、SGL 数据缓冲都必须是注册过的。
5. **双面 Send 的 Receive 必须先挂上。** 服务端 Accept 之后几毫秒就 Send 的话，
   Send 找不到匹配的 Receive，被 provider 判 `ND_IO_TIMEOUT` (0xC00000B5)，
   而**客户端只看到一个被取消的 Receive** —— 报错出现在"没错"的那一端。
6. **`NdStartup()` 必须先调。** 否则每次 `NdOpenAdapter` 都返回
   `ND_DEVICE_NOT_READY` (0xC00000A3)，看起来像驱动问题，其实是没初始化。
7. **连接端必须用 `NdResolveAddress(远端)` 得到的本地地址去开适配器。**
   用远端自己的地址开会变成自己连自己：一端 `ND_CONNECTION_REFUSED` (0xC0000236)、
   另一端 `ND_CONNECTION_ACTIVE` (0xC000023B)。而且**两口同子网时
   `NdResolveAddress` 会返回远端自己的地址**，此时连接是 provider 环回、
   **一个字节都不上线**。冒烟测试因此加了 `-local` 强制指定本端口。

### 8.3 ✅ 已解决：远程 RDMA Read 失败的真因 —— `outboundReadLimit` 不能为 0

**症状**：远程单边 RDMA Read 一律失败，返回**原始** `0xC000003E`
（`ndstatus.h` 里根本没有这个值）；而同样条件下的 RDMA **Write 完全正常**。

**定位过程（一次有用的对照实验）**：

| 顺序 | 操作 | 结果 |
|---|---|---|
| A | 用普通 MR rkey 做 RDMA **Write** | ✅ `status=0`，对端校验 4 KiB 数据落地 |
| B | 用**同一个** MR rkey 做 RDMA **Read** | ❌ `0xC000003E` |

A 通、B 不通 ⇒ **连接、rkey、地址、MR 注册、QP 全都没问题，问题只出在 Read 本身。**

**真因**：`Connect`/`Accept` 的 **`outboundReadLimit` 传了 0**。
该参数是"本端**允许发起**的未完成 RDMA Read 数上限"，**传 0 就是禁止本端发起任何 Read**。
Write 不受它约束，所以两边的表现差异正好是这个参数。

改成 **16** 之后，**全部通过**：

| 检查 | 结果 |
|---|---|
| 普通 MR rkey 的 RDMA Read | ✅ `status=0`，65536 B，首字节 0x5A 正确 |
| **memory window rkey 的 RDMA Read** | ✅ `status=0`，65536 B，数据正确 |
| **memory window rkey 的 RDMA Write** | ✅ `status=0`，服务端校验：窗口内被覆盖、**窗口前后一字节未动** |
| 服务端反向从客户端 MR 读 | ✅ `status=0`，65536 B，数据正确 |
| `Invalidate` | ✅ 完成项到达，`type=3`（`Nd2RequestTypeInvalidate`） |
| **失效后用旧 STag 再读** | ✅ 被拒 —— STag 是**真实的安全边界** |
| 越过窗口末尾读 1 字节 | ✅ `ND_ACCESS_VIOLATION` **0xC0000005**（QP 存活时） |

**最终成绩：客户端 0 failures，服务端 0 failures。**

### 8.4 两条对现有代码的连带发现

1. **`rdmaio` 生产代码的 RDMA Read 路径从来没有真正跑过。**
   `rdma_transfer.cpp` 所有 `Connect`/`Accept` 调用传的都是
   `(g_is_read ? 16 : 0, 0, ...)` —— **`outboundReadLimit` 恒为 0**。
   按本次实测，这意味着**它一次 Read 都发不出去**。
   这正好解释了 `CODE_REVIEW.md` 里 C1 那条"Read 模式文件传输越界、
   从未被实际执行过"：那条路径不是"没测"，是**根本不可能工作**。
   ⇒ 如果要保留 Read 模式，这一处必须一起改。

2. **RC QP 上的一次访问越界是致命错误，会拆掉整个 QP。**
   我在测试中间跑"越过窗口读"这个探针，结果**后面所有操作都变成 `ND_CANCELED`**，
   看起来像好几个独立故障。NVMe-oF 必须据此设计：
   **SGL 要在发起 Read 之前校验，而不是等它失败** ——
   一个坏 SGL 会打断整条连接，而不是只让那一条命令失败。

### 8.5 ✅ F1/F2 已完成：真实的 NVMe-oF 建链跑通了

`f1_bringup.cpp` —— 我们自己的 initiator 与 target，走本机两口直连。
**两端各 0 failures。**

| 步骤 | 实测 |
|---|---|
| `Connect` / `CompleteConnect`（读写限额 16） | ✅ |
| **Property Set** `CC.EN=1` | ✅ target 收到 `off=0x14 val=0x1`，回 CQE status `0x0001` |
| **Property Get** `CSTS` | ✅ 返回 `0x1`，RDY 置位 |
| **Connect（admin 队列）** | ✅ target 用 SGL 里的 rkey **RDMA Read 回 1024 字节 Connect data**，读到 `hostnqn` 正确，分配 `cntlid=1` |
| **Identify Controller** | ✅ target 用 SGL 里的 rkey **RDMA Write 4096 字节到主机内存**，主机校验 `sn`/`mn`/`subnqn`/`cntlid`/`sgls`/capsule 尺寸**全部完好** |

关键日志（target 侧）：

```
cmd 1: Property Set off=0x14 val=0x1
cmd 2: Property Get off=0x1C
cmd 3: Connect qid=0 sqsize=31 kato=30000
      SGL type=4 subtype=0 addr=0x19ED6460200 len=1024 rkey=0x24030160
      hostnqn='nqn.2014-08.org.nvmexpress:uuid:ndvmeof-initiator-0001'
      cntlid=1 assigned
cmd 4: Identify cns=1 nsid=0
      SGL type=4 addr=0x19ED6461000 len=4096 rkey=0x24030160
```

**这意味着 NVMe-oF 的数据路径形状已经验证**：capsule 携带 SGL → 对端用 host 公布的 rkey
做单边 RDMA 读/写。后面所有的东西（I/O 队列、Read/Write 命令、真正的 target）
都只是换 opcode 的同一套结构。

**一条要记住的副作用**：RDMA Write 的完成项里 `BytesTransferred` **恒为 0**（本 provider），
数据本身是到的（4096 字节全部校验通过）。**不能拿它判断写成功** ——
判据只能是对端内容校验。

### 8.6 本轮又踩了两次同一个坑

"**SGE 的 Buffer 必须在 token 所指的 MR 里**"这条规则，本轮在两个新地方又犯了：

1. target 的响应 CQE 用了栈上的 `uint8_t cqe[16]` → Send 返回 `ND_ACCESS_VIOLATION`
   （而且**报错在发送方**，接收方只是等不到东西）。
2. 打印 Identify 的 `sn`/`mn`/`subnqn` 时直接 `%s` 未终止的定长字段 →
   打出一屏 0xCC（这正是"看起来像协议错乱、实际是 printf 越界"的典型）。

已经第三次撞同一堵墙，所以把规则写进代码注释而不只是文档：
**凡是要进 SGE 的缓冲，一律显式取 `t.reg + offset`。**

顺带修掉一个**测试自身的假通过**：Identify 失败时，我仍然去检查 payload 内容，
而 payload 是 0xCC 填充的毒值，其中两条断言**通过了**。
现在命令失败就直接跳过 payload 检查 —— 假通过比失败更危险。

### 8.7 ✅ F3 完成：I/O 队列与 Read/Write 数据面跑通了

`f3_io.cpp` + 抽出来的 `nvmeof_rdma.h`（`Device` + 多 `Queue`，一个 QP 一个队列）。
**两端各 0 failures。**

| 检查 | 结果 |
|---|---|
| admin 队列 QP | ✅ |
| Property Set / Get / Connect(admin) | ✅ |
| Create CQ qid=1 / Create SQ qid=1 | ✅ |
| **I/O 队列用独立的第二个 QP**（两个 QP、两个 CQ、一条 fabric） | ✅ |
| Connect I/O queue（qid=1，带上 admin 分配的 cntlid） | ✅ |
| Identify Namespace | ✅ `nsze=2048 块，块大小 512 B（lbaf[0].ds=9）` |
| **WRITE/READ 往返 256 KiB** | ✅ 0 字节不符 |
| **WRITE/READ 往返 3584 B**（7 块，验证 NLB 是 0 基） | ✅ 0 字节不符 |
| **WRITE/READ 往返 512 B**（NLB=0） | ✅ 0 字节不符 |
| 越界 LBA 被拒 | ✅ `sct=0 sc=0x80`（LBA_RANGE） |
| 被拒之后队列仍然可用 | ✅ |
| target 累计 | `ioCommands=8 written=266752 read=266240` |

数据是 **target 直接 RDMA 读写自己的 namespace，没有 bounce buffer** ——
这是 NVMe-oF/RDMA 的全部意义所在，所以"没有 bounce buffer"本身就是被测对象。

### 8.8 F3 暴露的三个真问题（都值得记）

1. **NVMe 的 opcode 只在命令集内唯一。** `WRITE = 0x01`（NVM 命令集）和
   `Create SQ = 0x01`（Admin 命令集）**是同一个值**。
   我最初按 opcode 分派，结果**每一条 WRITE 都被当成 Create SQ 执行**
   （现象：WRITE 报成功但数据全是 0，target 的 `written=0`）。
   **必须按队列分派**：admin 队列解 admin 命令集 + fabrics，
   I/O 队列解 NVM 命令集。这是本轮最有价值的一条协议发现。

2. **在 listener 上挂第二个 `GetConnectionRequest` 会取消 QP 上待处理的 Receive。**
   现象：admin 队列的 Receive 立刻以 `ND_CANCELED`、`bytes=0` 完成。
   我本想把"等 I/O 队列连接"做成非阻塞的，结果把 admin 队列打死了。
   **修法不是改异步，而是改协议顺序**：让 initiator 在 Create SQ 之后
   **立刻**连 I/O 队列的 QP，然后再回到 admin 队列做 Identify。
   这样 target 侧的 accept 就是短等待，不会与 admin 流量重叠。

3. **我又一次手写了状态位提取，并且写错了。** `NVMEOF_STATUS_SC_MASK` 是
   **原始位掩码**（bits 8:1），不右移就得到 `0x100`，再截成 `uint8_t` 变成 `0x00` ——
   **一条被正确拒绝的命令因此被读成"成功"**。
   头文件里本来就有 `nvmeof_status_sc()` / `nvmeof_status_sct()`，
   我又绕过去手写了一遍。**判据：凡是状态字段，只用头文件里的访问器。**

顺带：进程的 stdout 改成 **unbuffered** —— 之前 target 挂死被 kill 时，
整个输出因为还压在缓冲区里而全部丢失，**偏偏那正是最需要日志的时刻**。

### 8.9 一条测试环境规则：三个套件不能重叠运行

我按顺序连着跑 F3 → F1 → stag，F1 出现了 4 个 failure，
症状（QP 的 Receive 以 `ND_CANCELED`、`bytes=0` 完成）和 §8.8 第 2 条一模一样。
单独重跑 F1 **连续两次 0/0** —— 所以它不是回归，是**上一个套件的进程还没退干净**
时下一个套件的 target 就起来了，还占着 RDMA 栈。

已修：`run_f1.ps1` / `run_f3.ps1` 启动前会等 `stag_smoketest` / `f1_bringup` / `f3_io`
**全部退出**再继续。修完再按同样的顺序连跑：

```
F3    initiator failures: 0 / target failures: 0
F1    initiator failures: 0 / target failures: 0
STAG  client failures: 0 / server failures: 0
```

**教训**：一个"看起来像回归"的失败，在断言是回归之前，
先确认环境里没有别的实例 —— 这一步比读代码便宜得多，而且这次它是唯一原因。

### 8.10 ✅ F4（流水线）已完成：1024 条命令零错误

**状态**：`f4_pipeline.cpp` 三连跑全绿，`initiator failures: 0` / `target failures: 0`：

```
[PASS] both pipelined phases ran to completion - write 16.00 MiB in 0.026 s, read 16.00 MiB in 0.038 s
[PASS] every command completed with success status - badStatus=0
[PASS] every command id completed exactly once, no corrupt payloads
       - duplicates=0, mismatching read data=0, wrong-cid=0, cids seen=1024/1024
```

8 条命令在飞、64 轮、每命令 32 KiB，两个阶段（写 16 MiB / 读 16 MiB）共 **1024 条命令**：
每一条的 cid 恰好完成一次、没有错槽、读回的 512 个负载逐字节正确。

**这一轮真正的收获是四个 bug，全都是"看起来像协议故障、实际是原语误用"：**

#### (1) `GetOverlappedResult` 必须在**发起该请求的那个对象**上调用

F4 的 I/O 队列 Connect 一直超时（`ND_IO_TIMEOUT`），而 F3 用同样的代码能过。
唯一的结构差异是 target 侧等 io 连接请求时写的是
`io.waitOverlapped(listener, 15000)`，而 F3 写的是 `io.waitOverlapped(io.conn, 15000)`。
改回 `io.conn` 后**立刻通过**。

规则要说清楚：**OVERLAPPED 结构体本身可以随便复用，但查询它的对象必须是发起请求的那个。**
用 listener 去查 `io.ov`（其实是 `io.conn` 的请求）拿到的是 listener 自己上一次操作的结果，
于是 `Accept` 在对端的 io 连接请求真正到达之前就执行了。

#### (2) `Queue::reap(expect)` 会**静默丢弃**不匹配的完成

```cpp
if (expect && r.RequestContext != expect) { printf("ignoring..."); continue; }  // ← 丢掉了
```

串行命令里没问题，**流水阶段是灾难**：等 slot i 的时候 slot i+1 的响应先到，
被 `continue` 扔掉，然后第 i+1 次循环白等 5 秒去等一个**已经到过**的完成。
改成"poll 一次 + 按 ctx 分派"后两个阶段立刻跑完。

#### (3) namespace 必须装得下整个写集，否则 LBA 映射会折回

namespace 2 MiB（4096 块）而写集 16 MiB，映射写成 `% maxSlba`（`4032`）。
`32256 % 4032 == 0` —— **round 63 正好折回 round 0**，于是读阶段 round 0 读到的是
round 63 的字节。

定位它的线索是错值的**形状**：所有不匹配的字节都恰好差 `0xA1`，
而 `0xA1 = 63 × 31 mod 256` 正是 pattern 生成器里的 **round 项**。
**恒定的差值指向"读到了另一个 round"，而不是"数据损坏"** —— 这个区分省掉了一整轮瞎猜。

已把"写集必须装进 namespace"和"namespace 必须装进已注册区域"写成 `static_assert`。

#### (4) Receive 必须在 `Accept` **之前**挂好

`Accept` 才是让 QP 可用的动作，也才是释放对端 `CompleteConnect` 的动作。
**在 Accept 之后再 `postReceive`，就是在和一个已经有资格发送的对端赛跑**，而且窗口里还有一个 `printf`。

输了这一场不是可重试的小抖动：initiator 的 Send **完全不完成** ——
没有 RNR 重试、没有错误完成，只有一个 5 秒超时；target 那边则在对端退出时
把 Receive 报成 `ND_CANCELED`。**这就是前面几轮那个"admin Receive 莫名其妙被取消"的真相：
它一直是受害者，不是故障点。**

**怎么确认它是竞态而不是我改坏了代码：** 打开 trace（每命令多 4 次 `printf`）→ 通过；
关掉 trace → 连续 3/3 失败。**printf 恰好提供了 target 需要的那点延迟。**

连带一条已记录的坑在这里再次生效：`listener->GetConnectionRequest(io.conn, ...)`
会取消**另一个 QP 上已挂的 Receive**，所以 io 队列的 `GetConnectionRequest` 之后，
admin 的 Receive 必须重新挂一次。

#### (5) cid 必须在两个阶段之间唯一

两个阶段各自把命令编成 1..512，于是"cid 不得重复"的检查报出 512 个重复 ——
**是测试的编号错了，不是协议错了。** 改成跨阶段唯一（1..1024）。

#### 吞吐（**最终修正**：256 KiB/命令 × 8 在飞 × 64 轮 = 128 MiB/阶段，3 次）

| | 第1次 | 第2次 | 第3次 | 均值 | 离散 |
|---|---|---|---|---|---|
| write | 1156.90 | 1156.74 | 1192.96 | **1168.9 MiB/s** | 3.0% |
| read | 1092.25 | 1210.82 | 1248.62 | **1183.9 MiB/s** | 13% |

两端各自扫过自己的 128 MiB（transfer 128 MiB + namespace 128 MiB，region 257 MiB），
**每一次都对上不同的缓冲区**，所以这个数字对应"真实存储"的访问模式，而不是缓存里的假象。

**对比裸 RDMA Write 基线 2413 MiB/s（缓存驻留）：约 48%。**

**write ≈ read（1169 vs 1184）。** 走完整条 NVMe-oF 路径（capsule + SGL + target 块层）后，
两个方向没有实质差异 —— 之前"write 93% / read 70%"的差距**完全是基准自己造成的**。

#### 我在这个数字上错了三次，每次都错在不同地方（都值得记）

| 我给过的数 | 错在哪 | 怎么发现的 |
|---|---|---|
| 596 MiB/s（占 22%） | 基准逐字节生成/校验 pattern，测的是**它自己的 CPU 循环** | 命令大小从 32 KiB 扫到 512 KiB，吞吐**完全不变** —— 16 倍范围不变，既不是带宽也不是每命令开销，只能是按字节的成本 |
| 2254 MiB/s（占 93%） | 写阶段 8 个缓冲区跨轮复用（4 MiB，**留在 L3**），读阶段却扫遍整个 namespace（走 DRAM）。两个方向根本不可比 | 给每条命令分配独立缓冲区（transfer 区 = namespace 区）后，write 掉到 ~1150，而 read 升到 ~1180 |
| 700–1200 之间乱跳 | **用 PowerShell 管道边跑边抓输出**（`Select-String` 在测试运行期间抢 CPU） | 改成先重定向到文件、跑完再读，3 次重复的 write 离散立刻降到 3.0% |

**三条合起来是一条方法论：性能数字要先证明"测的不是工具自己"。**
判据分别是 —— 对命令大小不敏感 ⇒ 瓶颈在按字节的代码里；两个方向差一大截 ⇒ 先查
两侧的访问模式是否对称；重复之间跳得厉害 ⇒ 先查测量本身有没有干扰被测对象。
**这一轮我三次都是先给出数字、后被自己的判据推翻，所以这三条判据比数字本身更有价值。**

#### 顺带：单个 MR 到 513 MiB 可用，2049 MiB 不行

扫描里 region 到 **513 MiB** 注册与传输都正常（namespace 256 MiB）；
**1025 MiB 仍能跑**；**2049 MiB 的配置两个端点都打印了 layout 却没有产出任何结果**。
§7 里"CX3 单个 MR 到 191 MB 可用、268 MB 挂死"那条旧结论的适用范围已按本轮实测更新 ——
当时的 268 MB 上限更可能是当时的策略护栏。**实际可用的安全上限在 1–2 GiB 之间，未细分。**




### 8.11 上一轮的教训仍然有效：测试脚本曾经在骗我

（以下这段与 F4 的四个 bug 无关，但它把更早几轮的结论都置于怀疑之下，保留原样。）

三个 run 脚本的构建守卫是错的，导致"跑的是旧二进制"。

原来的守卫是：

```powershell
if (-not (Test-Path $exe)) { Write-Host "BUILD FAILED"; exit 1 }
```

**只要上一次构建留下过一个 exe，这个检查就永远通过。**
于是 `f4_pipeline.cpp` 因为一个 `C2001: 字符串字面量中的换行符` 编译失败时，
脚本照样运行**上一次的旧 exe**，而我把那个结果当成了"F4 的协议问题"，
还照着它做了四轮诊断（比对 F3/F4 的 target、改 region 大小、交叉测试、加 trace）——
**全部建立在错误的二进制上。**

更糟的是脚本还过滤了构建输出：
`| Where-Object { $_ -match "error|warning|\.cpp" }`，
而我在看运行结果时又只 grep 了 `PASS|FAIL`，**编译错误从头到尾没进过我的视野**。

**修法**（三个脚本都改了）：
1. 构建前先删掉 exe；用**构建退出码**判断，而不是"exe 是否存在"。
2. 再检查**源文件时间戳是否比 exe 新**，新则拒绝运行。
3. 构建输出不再过滤，错误原样打出来。

**教训**：一个测试脚本能给出的最强保证，是"它跑的确实是你刚写的那份代码"。
这一条不成立时，后面所有结论都是噪声 —— 而且它会伪装成协议问题，
让你往最贵的方向debug。**先证明被测物是刚构建的，再解释失败。**

#### 顺带：我自己打补丁的工具又犯了同一个错

用 Python 脚本改 C 源码时，`\n` 被解释了两层，写进 C 字符串字面量的是**真换行**，
于是 `C2001`。这已经是同一类错误第二次（上一次是 `_p7.py` 的断言失配）。
**规则：跨语言改代码时，替换字符串一律用 raw string（`r"""..."""`），
或者干脆用 edit 工具做字面替换。**

#### 空洞断言已修

"every command id completed exactly once" 和 "every command completed with success status"
这两条断言，在流水阶段**提前 bail 时也会 PASS**（计数器还是 0）——
和 §8.6 里"0xCC 毒值让两条断言通过"同一类。
现已改成：cid 计数必须**恰好等于 2×rounds×kMaxInFlight**，
且 `wrongCid == 0`，都挂在"阶段确实跑完"这个前提上。

#### 推荐的复现命令

```
run_wire.ps1                            # wire 自检，C 与 C++ 双份 /W4 /WX
run_f1.ps1                              # 建链 + Identify
run_f3.ps1                              # 单命令 I/O 与边界
run_f4.ps1 -xfer 262144 -rounds 64      # 流水吞吐：128 MiB/阶段，region 257 MiB，write 离散 3.0%
run_f4.ps1                              # 流水默认：32 KiB × 64 轮 = 16 MiB/阶段，region 33 MiB
run_f6.ps1                              # 生命周期：私有数据 / Keep Alive / 删除 / Disconnect
```

**一个套件跑完再跑下一个**（见 §8.9）：上一个套件的进程还在退出时，
下一个套件的 queue pair Receive 会以 `ND_CANCELED` + 0 字节完成，看起来和协议故障一模一样。
四个脚本都已经在开头清进程并等待。
`run_f4.ps1 -xfer` 必须两端一致（namespace 布局由它推导），脚本已经把它同时传给两个端点。
**跑吞吐时不要用 PowerShell 管道边跑边抓输出**，先重定向到文件、跑完再读（见上表第三行）。

### 8.12 ✅ F6 完成：生命周期（私有数据握手 / Keep Alive / 队列删除 / Disconnect）

`f6_lifecycle.cpp`，跑 3 次全绿，`initiator failures: 0` / `target failures: 0`，
target 侧 `ioCommands=2 keepAlives=1 disconnected=1 sq1=0 cq1=0`。

F1/F3/F4 证明的都是"控制器已经起来且永不消失"时的事。真实控制器要被**创建**和**销毁**，
而 NVMe-oF over RDMA 的这两件事都发生在 capsule 到不了的地方。

#### 1. RDMA 私有数据握手（这是整个工程最大的互操作风险，现在验过了）

RDMA 传输在**连接的私有数据**里协商，不在 capsule 里：host 在 Connect 里宣告
`(recfmt, qid, hrqsize, hsqsize, cntlid)`，控制器在 Accept 里回答 `(recfmt, crqsize)`。
**Linux nvmet 会校验这一块并在不对时拒绝连接**，所以一个忽略它的 target
"自己跟自己测什么都过，遇到任何真实 host 就挂"。

实测两个方向都逐字段正确：

```
initiator 发出: recfmt=0 qid=0 hrqsize=32 hsqsize=32 cntlid=0xFFFF
target   收到: recfmt=0 qid=0 hrqsize=32 hsqsize=32 cntlid=0xFFFF   ← 完全一致
target   回答: recfmt=0 crqsize=32
initiator 收到: len=148 recfmt=0 crqsize=32
```

并且**故意发一个 recfmt=0xDEAD 的 Connect**：target 用 `Reject()` 拒绝，
initiator 收到 `ND_CONNECTION_REFUSED (0xC0000236)`。
这条负向测试放在最前面，就是为了让"来者不拒的 target"在其它测试通过并把它盖住之前先暴露。

#### 2. `GetPrivateData` 给正好 32 字节会返回 `ND_BUFFER_OVERFLOW`，但数据是对的

`0x80000005 = ND_BUFFER_OVERFLOW`。给 32 字节缓冲区时它填好了全部 32 字节，
却返回 BUFFER_OVERFLOW，并报告 `len=148`（= 适配器的 `MaxCalleeData`）；
请求方向报告 56（= `MaxCallerData`）。

**所以只看 HRESULT 会误判成失败，只看数据会误判成成功 —— 两个都要看，并且缓冲区要给大。**
现在两边都用 512 字节缓冲区并只校验前 32 字节。

#### 3. connector 是"一次性"的（Reject / 被拒的 Connect 之后不能再用）

拒绝掉一个连接之后：initiator 的下一次 Connect 返回 `ND_CONNECTION_ACTIVE (0xC000023B)`，
target 的 `GetConnectionRequest` 直接失败。**都不是"这个 connector 已经用过了"这种清楚报错。**
修法是换一个 connector（`Queue::newConnector()`）。
initiator 侧那次故意发坏 recfmt 的探测因此用了**独立的临时 Queue**，用完就销毁。

#### 4. 忘记 CQE 的 phase 位是静默且极具误导性的

`NVMEOF_STATUS_MAKE(sct, sc)` **不含 phase 位**，调用者必须自己 OR 上 `NVMEOF_STATUS_P_MASK`。
F6 第一版忘了，症状是：

```
[FAIL] admin bring-up (enable, RDY, Connect)
[FAIL] Keep Alive on the admin queue is accepted
...
[PASS] Delete CQ while its SQ still exists is refused
[PASS] Delete SQ qid=1 a second time is refused
```

**凡是期望成功的断言全挂，凡是期望失败的断言全过** —— 因为"没有 phase 位"和"不是成功"
是同一个谓词（`statusOk()` 要求 P=1）。看起来完全像传输坏了，实际是应答方少了一个 bit。

修法不只是补上：新增 `NVMEOF_STATUS_CQE(sct, sc)`（含 phase），并在 wire 自检里
把两者的差别钉死（`MAKE(success)==0x0000`、`CQE(success)==0x0001`）。

#### 5. 我在新文件里把 F4 的 `reap(expect)` bug 又写了一遍

F6 的 `submit()` 一开始又是"先 reap Send 再 reap Receive"。
`reap(expect)` 会**静默丢弃**不匹配的完成，于是响应先到时被扔掉，下一次调用白等 5 秒。
**同一个错误在新文件里复现，说明 §8.10(2) 那条只写在文档里是不够的。**
现在两个文件都是"poll 一次 + 按 ctx 分派，什么都不丢"。

#### 6. Keep Alive 不是 fabrics 命令，是 admin opcode 0x18

**我原本计划给它编一个 fctype，那会造出一条 Linux 不认识的命令。**
查了 FreeBSD 的 `nvmf_proto.h`（本身派生自 SPDK 的 `nvmf_spec.h`）确认：
fabrics fctype 表是 `PROPERTY_SET=0x00 / CONNECT=0x01 / PROPERTY_GET=0x04 /
AUTH_SEND=0x05 / AUTH_RECV=0x06 / DISCONNECT=0x08`，**Keep Alive 不在里面**。
同一次查阅还拿到了 Disconnect 的 64 字节布局（`recfmt@40`）、RDMA 私有数据的
两个 32 字节结构、以及 `trtype/adrfam/qptype/prtype/cms` 的取值 ——
这些都已写进 `nvmeof_wire.h` 并由自检钉住，是 F5 直接要用的东西。

实测：admin 队列上 Keep Alive 成功；**I/O 队列上 Keep Alive 被拒，`sct=0 sc=0x01`
（Invalid Opcode）** —— 这正是"必须按队列分派而不是只按 opcode 分派"的又一处。

#### 7. 删除顺序与 Disconnect

- **CQ 的 SQ 还在时删 CQ 被拒**：`sct=1 sc=0x0C`（Invalid Queue）。规范要求 host 先删 SQ，
  把它做成真实检查而不是注释，才让这条顺序要求有意义。
- **重复删同一个 SQ 被拒**：`sct=1 sc=0x01`（Invalid Queue Identifier）。
- **Disconnect 必须先应答再拆**：那个完成包要在控制器即将销毁的 queue pair 上发出去。
  实现上 target 发完成、等 Send 完成、然后才拆。
- **拆掉之后 host 的下一条命令在 31 ms 内失败，不挂死**（断言挂了上限，不是"看起来没卡"）。

#### 明确不做：Authentication

fctype 0x05/0x06 这个子集不实现，写在这里是为了不偷偷省略。

### 8.13 ⛔ 重大设计修正：NVMe-oF 的 I/O 队列不是用 Create SQ/CQ 建的

**这一条推翻了 F3/F4/F6 建立 I/O 队列的方式，是查 Linux `nvmet` 源码时发现的。**

`drivers/nvme/target/admin-cmd.c`（本次直接读的权威实现）里，
`create_sq` / `delete_sq` / `create_cq` / `delete_cq` 四个 handler **第一件事都是**：

```c
static void nvmet_execute_create_sq(struct nvmet_req *req)
{
	struct nvmet_ctrl *ctrl = req->sq->ctrl;
	if (!nvmet_is_pci_ctrl(ctrl)) {
		status = nvmet_report_invalid_opcode(req);   // ← 非 PCI 控制器直接 Invalid Opcode
		goto complete;
	}
	...
```

**`nvmet_is_pci_ctrl()` 为假 = 所有 fabrics 传输（RDMA/TCP/FC）。**
也就是说：**在 NVMe-oF 里，Create I/O SQ 和 Create I/O CQ 根本不是合法命令。**

NVMe-oF 1.1 规范 "differences from the NVMe Base specification" 一节说得更直接：
I/O 队列与 Create SQ/CQ 是一对一改由 **fabrics Connect 命令**建立。

**正确模型：**

| | 我们 F3/F4/F6 的做法（错） | NVMe-oF 正确做法 |
|---|---|---|
| admin 队列 | fabrics Connect qid=0 | fabrics Connect qid=0 ✓（这个对的） |
| I/O 队列 | Create CQ qid=1 → Create SQ qid=1 → fabrics Connect qid=1 | **每个 I/O 队列一个 QP，在这条 QP 上直接发 fabrics Connect，qid=1,2,...** |
| 队列大小 | Create SQ 的 QSIZE | Connect 的 `sqsize` + 私有数据里的 `hsqsize`/`hrqsize` |
| 队列数量 | 无 | 先 `Set Features: Number of Queues`，控制器在 result 里回它能给几个 |
| 删除 I/O 队列 | Delete SQ/CQ | **不存在**；断开那条 QP 就是删除 |

**后果**：一个真实的 Linux host 连我们的 target 时，
会在 `Set Features(NUM_QUEUES)` 就撞上我们没实现的 opcode；
即使跳过，也永远不会发 Create SQ，于是我们的 target 会一直卡在
`[accept] Create SQ seen` 那个等待上。**F5 按现在的流程必然失败。**

**顺带查出三个 Identify Controller 字段是错的**（对比 `nvmet_execute_identify_ctrl`）：

| 字段 | 我们 | Linux nvmet | 说明 |
|---|---|---|---|
| `sqes` | `6` | `(0x6 << 4) \| 0x6` = 0x66 | 高 4 位是**最大** SQ entry size（2^n），我们填 0 等于说"最大 1 字节" |
| `cqes` | `4` | `(0x4 << 4) \| 0x4` = 0x44 | 同上 |
| `icdoff` | `2`（32 B） | **不设置 = 0** | RDMA 不用 in-capsule data；填 2 等于宣称命令里有 32 字节 in-capsule data，而那正好压在命令字段上 |
| `kas` | 0 | `NVMET_KAS` | KAS=0 的字面含义是"不支持 Keep Alive"，可我们实现了 |
| `maxcmd` | 0 | `NVMET_MAX_CMD()` | host 用它定 tag set 深度 |

另外 Linux 的 `delete_cq` 在 CQ 仍被 SQ 占用时返回
**`NVME_SC_QID_INVALID`(0x01) + DNR**，不是我们 F6 断言的 `INVALID_QUEUE`(0x0C)。
我们的断言把一个与 Linux 不同的行为固化成了"通过"。

**已按这一节重做：见 §8.14 的 F7。**

### 8.14 ✅ F6 已按 Linux host 的真实形状重建：22/22，跑 2 次全绿

`f6_lifecycle.cpp` 整体重写。现在的流程就是 Linux host 会走的那一条：

```
Connect qid=0（admin 队列对，带私有数据握手）
Set Features: Number of Queues        ← 不实现它，真实 host 连一个 I/O 队列都不会建
Get Features: Number of Queues        ← 与 Set 的答复必须一致
Property Set CC.EN=1 / Property Get CSTS
Set Features: Keep Alive Timer
Keep Alive                            ← 必须在 KATO 之后
Identify Namespace / Identify Controller
每个 I/O 队列：新建一条 queue pair + 在它上面发 fabrics Connect(qid=N)   ← 没有 Create SQ/CQ
两个 I/O 队列各自跑 WRITE/READ
Create/Delete I/O SQ/CQ 四条 → 必须全部 Invalid Opcode
Disconnect → 控制器先应答再拆
```

target 侧证据（不是"断言过了"，是它自己数出来的）：

```
target summary: ioQueuesGranted=2 ioQueuesEstablished=2 ioCommands=4
                keepAlives=1 katoMs=120000 disconnected=1
  io queue qid=1 commands=2
  io queue qid=2 commands=2
```

**两条 I/O 队列各自独立承载了流量** —— 否则"两个队列"只是循环上界，不是协议事实。

#### 重建过程中暴露的三个真问题

**(a) target 在 `GetConnectionRequest` 里阻塞，会把 admin 队列饿死**

第一版让 target 在 `Set Features(NUM_QUEUES)` 之后去
`listener->GetConnectionRequest(io.conn, &ov)` 并等最多 15 秒。
可是 host 在这个时点还没连 I/O 队列——它正在做 Get Features / KATO / Keep Alive / Identify。
结果 target 卡在等连接上，**host 后面每一条 admin 命令都超时**。

症状极具误导性：`Get Features` 失败、`Set Features: KATO` 的 result 打印出
上一个命令的 `0x00010001`（NUM_QUEUES 的答复），Identify 读回一片 `0xCC` 毒值。
看起来像响应错位一格，实际是 target 单线程里的一次阻塞等待。

修法：新增 `Queue::pollOverlapped()`——**一次非阻塞的 `GetOverlappedResult`**。
不能用 `waitOverlapped(ms=0)` 代替：那会把"还没完成"当成超时并调用
`CancelOverlappedRequests()`，**把正在轮询的那个请求本身取消掉**。
现在 target 发出连接请求后继续轮询 admin，请求完成再收尾。

**(b) Keep Alive 在 KATO 之前必须被拒**

Linux `nvmet_execute_keep_alive` 里：
```c
if (!ctrl->kato) { status = NVME_SC_KA_TIMEOUT_INVALID; goto out; }
```
**KAS=0 的字面含义是"不支持 Keep Alive"**，所以控制器没配置 KATO 时回
`KA_TIMEOUT_INVALID`(0x01) 而不是成功。这也是 host 发现自己没设过定时器的方式。
已实现并定了断言。

**(c) I/O 队列只答一条命令**

`serve()` 处理完 fabrics Connect 后忘了重新挂 Receive，
于是那条队列答完 Connect 就再没有 pending 的 Receive，
**host 之后所有 capsule 都超时，而 target 侧一声不响**。
这和 §8.10(4) 是同一个形状：漏挂 Receive 的表现永远是"对面 Send 不完成"。

#### 修正后的 Identify Controller 字段（实测值）

```
sqes=0x66 cqes=0x44 icdoff=0 kas=1 sn='NDVMEOF0000000000001'
```

对比 Linux nvmet 的 `nvmet_execute_identify_ctrl` 逐字段核对过。新增
`NVMEOF_ID_CTRL_OFF_RAB(72)` / `MAXCMD(514)` / `KAS(320)` 三个偏移，
并用 `static_assert` 钉住它们与相邻字段的关系（MAXCMD 正好夹在 CQES 和 NN 之间）。

#### 还没跟上的一条（诚实记下）

**`f3_io.cpp` 和 `f4_pipeline.cpp` 仍在用 Create SQ/CQ 那套 PCI 模型建 I/O 队列。**
它们作为**数据面**测试依然有效（capsule 编解码、SGL、RDMA Read/Write 语义、
流水线、吞吐），但它们的**建队流程不是 NVMe-oF 的**。
F6 现在是模型基准。把 F3/F4 改成 fabrics Connect 建队是后续工作，
不影响它们已经给出的结论，但读它们的时候要知道这一点。

### 8.16 ✅ F4 也搬到 fabrics 建队模型上了：流水线不受影响，吞吐结论不变

F4（吞吐/流水线）原来也是用 Create SQ/CQ 建 I/O 队列的。现在改成和 F6 一样的形状：
`Connect qid=0` → `Set Features: Number of Queues` → `Set Features: KATO`
→ 在 I/O queue pair 上发 `fabrics Connect qid=1`，**没有 Create SQ/CQ**。
target 侧改成由 NUM_QUEUES 触发、**非阻塞**地接受 I/O 连接。

一次跑通的证据（`initiator failures: 0` / `target failures: 0`）：

```
[PASS] admin bring-up (enable, RDY, Connect)
[PASS] Set Features: Number of Queues - controller granted 1 I/O queues
[PASS] Connect I/O queue qid=1
[PASS] Identify Namespace
[PASS] both pipelined phases ran to completion
[PASS] every command completed with success status - badStatus=0
[PASS] every command id completed exactly once, no corrupt payloads
       - duplicates=0, mismatching read data=0, wrong-cid=0, cids seen=1024/1024
  [accept] Number of Queues granted; waiting for the I/O queue pair
  [accept] GetConnectionRequest -> 0x00000000 ND_SUCCESS
  [accept] I/O queue connected; 8/8 capsule receives armed
```

**1024 条命令仍然一条不差** —— 换建队方式没有动到数据面，这正是应该的结果。

顺带把 F4 target 的 Identify Controller 也改成 fabrics 正确值
（`sqes=0x66` / `cqes=0x44` / `icdoff=0` / `kas=1` / `maxcmd`）。

#### 吞吐复测（256 KiB/命令 × 8 在飞 × 64 轮 = 128 MiB/阶段，3 次，文件重定向抓取）

| | 第1次 | 第2次 | 第3次 | 均值 |
|---|---|---|---|---|
| write | 1386.58 | 1130.34 | 855.29 | **1124 MiB/s** |
| read | 1204.51 | 1371.65 | 1108.35 | **1228 MiB/s** |

全部 `initiator failures: 0`。**结论与 §8.10 一致**：约 1.1–1.2 GB/s，
占裸 RDMA 基线 2413 MiB/s 的 ~48%，write ≈ read。

**但 write 的三次离散这次到了 ~50%（855–1387）**，而同一配置在 §8.10 量到的是 3.0%。
我没有把原因查清：可能是机器上其它活动，也可能是这条流程多了一次连接的时序影响。
**所以这里只报区间和均值，不报单点数字** —— 单点会让它看起来比实际更确定。

### 8.17 还差的一步

`f3_io.cpp`（单命令与边界/错误路径）**仍在用 Create SQ/CQ 的 PCI 模型建 I/O 队列**。
它的数据面断言与建队方式无关，所以结论不受影响，但它是最后一份没跟上 F6 模型的测试。

### 8.18 F5 仍然 blocked（本轮重新验证过的证据）

| 检查 | 结果 |
|---|---|
| `Win32_Processor.VirtualizationFirmwareEnabled` | **False** |
| `VMMonitorModeExtensions` / SLAT | True（CPU 支持，只是固件里关着） |
| `Win32_ComputerSystem.HypervisorPresent` | False |
| `wsl --status` | 提示需要启用 WSL2 所需功能 |
| Docker Desktop | 已安装，但 `dockerDesktopLinuxEngine` 管道不存在 → Linux 容器起不来 |

**本机现在拿不到任何 Linux 用户态**：WSL2、Hyper-V、Docker 都依赖同一件事 ——
BIOS 里的 AMD SVM。需要用户二选一：**进 BIOS 打开 SVM 重启**，或**用 U 盘启动 Linux**
（live 镜像装上 `nvmet` + `nvme-cli` 即可）。

在这之前，F5 需要的 wire 层东西已经全部备齐并验证：
capsule 字节镜像金标准、私有数据结构与传输常量、fabrics fctype 全表、
以及现在这套按 Linux host 顺序走的建链流程。

### 8.20 ✅ 找到并修掉 F1 的间歇性失败：它一直没吃到 §8.10(4) 那条规则

回归时 F1 连续失败 3 次（单独跑也一样），而 F3/F4/F6 全绿。症状：

```
[PASS] connector Bind / Connect / CompleteConnect
[FAIL] Property Set CC.EN=1 - cid=0 status=0xFFFF     ← 响应缓冲区一个字节都没被写
```

`status=0xFFFF` 是毒值没被覆盖，也就是 initiator **一条响应都没收到**。
连接是通的（Bind/Connect/CompleteConnect 全过），所以问题不在建链。

原因是 **F1 的 target 在 `Accept` 之后才挂第一个 Receive**：

```c
hr = t.conn->Accept(...);              // ← 这一步才释放对端的 CompleteConnect
...
for (;;) {
    if (!t.postReceive(t.reg + kCapsuleOff, 64, CTX_CAPSULE)) break;   // ← 太晚了
```

这正是 §8.10(4) 记过的那条：**Accept 才是让 queue pair 可用的动作，
在它之后再挂 Receive 就是在和一个已经有资格发送的对端赛跑**，
而输掉这场竞赛不是可重试的小抖动 —— 对端 Send 完全不完成（无 RNR 重试、无错误完成，
只有 5 秒超时），target 侧则一声不响。

**它一直是间歇性的，之前多次跑过，所以从没被当成 bug。** 修法就是把
`postReceive` 移到 `Accept` 之前，循环里改成"消费掉一个 capsule 之后再补挂"。

**F3 有完全一样的写法**，同样修掉。修完 F1 连跑 4 次 0/0、F3 连跑 2 次 0/0。

**诚实说明**：F1 修之前也是"多数时候能过"，所以 4/4 不能算统计证明；
但失败特征（status 0xFFFF + Send 不完成）与 §8.10(4) 记录的机理完全吻合，
修法也正是那条规则本身。

#### 顺带：`run_f1.ps1` 的构建守卫还是 §8.11 里那个坏版本

```powershell
& cmd.exe /c $cmd 2>&1 | Where-Object { ... } | ForEach-Object { "  $_" }
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { "BUILD FAILED" }
```

两个问题：**没有先删 exe**（编译失败时旧 exe 仍在，检查照样通过），
而且**新鲜度检查完全缺失**。已改成和 f3/f4/f6 一样：先删、看构建退出码、
并检查源文件**和两个头文件**的时间戳 —— 只改 `nvmeof_wire.h` /
`nvmeof_rdma.h` 正是"只看源文件"会漏掉的那类改动。

### 8.22 ✅ F3 也搬到 fabrics 模型 —— 三个 I/O 测试现在同一种建队方式

`f3_io.cpp` 原来也是 Create SQ/CQ 建 I/O 队列。现在和 F4/F6 一样：
`Connect qid=0` → `Set Features: Number of Queues` → `Set Features: KATO`
→ 在 I/O queue pair 上 `fabrics Connect qid=1`。target 侧的接受逻辑同样改成
由 NUM_QUEUES 触发、**非阻塞**。

改完 `initiator failures: 0` / `target failures: 0`，**所有边界与错误路径断言一条不少**：

```
[PASS] Set Features: Number of Queues - controller granted 1 I/O queues
[PASS] Connect I/O queue (qid=1) on its own queue pair
[PASS] READ ... 262144 B read back, 0 mismatching byte(s), round trip intact
[PASS] READ ... 3584 B  (odd size, NLB is 0-based)   round trip intact
[PASS] READ ... 512 B   (NLB = 0)                    round trip intact
[PASS] out-of-range LBA is refused - sct=0 sc=0x80 (expected LBA_RANGE 0x80)
[PASS] the I/O queue still works after a rejected command
```

**踩到并修掉的一个自己造的坑**：我把"qid != 0 的 Connect 不允许"加进了共用的
capsule handler，但那个 handler 同时服务 I/O 队列——而 I/O 队列的 Connect **就是** qid != 0。
于是 I/O 队列的 Connect 被自己拒了。判据必须挂在 `isAdmin` 上。

### 8.23 ✅ SGL / STag / Invalidate：实现到这张 Windows 栈的能力边界为止

目标里点名了 `SGL/STag + RDMA Read/Write/Invalidate`。查了 Linux nvmet 的
`nvmet_rdma_map_sgl()`（本次直接读的源码），语义是明确的：

```c
case NVME_KEY_SGL_FMT_DATA_DESC:            // type 0x4
    switch (sgl->type & 0xf) {
    case NVME_SGL_FMT_ADDRESS | NVME_SGL_FMT_INVALIDATE:
        return nvmet_rdma_map_sgl_keyed(rsp, sgl, true);   // ← invalidate
    case NVME_SGL_FMT_ADDRESS:
        return nvmet_rdma_map_sgl_keyed(rsp, sgl, false);
```

而 `map_sgl_keyed(..., invalidate=true)` 只是记下 `rsp->invalidate_rkey = key`，
然后在应答时换成 **`IB_WR_SEND_WITH_INV`**：

```c
if (rsp->invalidate_rkey) {
    rsp->send_wr.opcode = IB_WR_SEND_WITH_INV;
    rsp->send_wr.ex.invalidate_rkey = rsp->invalidate_rkey;
}
```

**三个要点**：

1. **它不是另一种传输**，就是一次普通的 keyed 传输（type 仍 0x4），
   只是 subtype 变成 0xf，外加"传完之后把刚给的那个 key 作废"。
2. **正因为 type 相同，"只看 type"的代码会把它当成普通描述符，
   然后把失效请求一声不响地丢掉。** 新加的 `nvmeof_sgl_wants_invalidate()`
   把这两者分开，wire 自检里也钉死了这一点
   （`0x40` vs `0x4f`，`type_of` 两者相同）。
3. **本机栈做不到第三步。** `IB_WR_SEND_WITH_INV` 要求"发送应答的同时作废对端的 STag"，
   而 `IND2QueuePair` 只有**本地** `Invalidate()`（作废我们自己的 memory window）；
   `SendAndInvalidate` 只存在于更新的 `IND` 接口（`ndspi.h:970`），IND2 没有。

**所以 target 的选择是拒绝，不是"照做但跳过失效"。** 后者会让 host 以为 STag 已经死了、
而它其实还活着——**这是唯一一种"静默错误"的选项，所以是唯一不采用的选项**。
现在返回 `SGL_INVALID_TYPE`(0x11) 并打印原因，F3 里两条断言把这个行为钉住：

```
[PASS] a keyed descriptor with the Invalidate subtype is refused, not silently served
       - sct=0 sc=0x11
[PASS] the I/O queue still works after the invalidate refusal
```

**这是一个能力边界，不是一个未完成项**：在 ND2 这层 API 上做不到，
除非改用 `IND` 接口（需要 provider 支持）或自己发 raw WR——两者都超出了当前范围。
已记进 §7 风险表。

**顺带说明**：Linux nvmet 的数据面默认走的是普通 keyed 描述符，
所以这条**不在 F5 互操作的关键路径上**（Linux host 默认也不发 invalidate 描述符）。

### 8.25 ✅ 补齐 "host 在 Connect 之后一定会发、而我们没实现" 的 admin 命令

F5 卡在 Linux，但**不需要 Linux 也能找出它会在哪儿断**：照着 Linux host 的实际顺序
把命令一条条列出来，看哪些我们答不了。查出**四个**，全部补上并定了断言。

#### (1) `Identify CNS=2`（Active Namespace ID list）—— 最致命的一个

host 枚举 namespace 的顺序是：
**Identify Controller(CNS=1) → Active Namespace ID list(CNS=2) → 对每个 id 做 CNS=0 和 CNS=3**。

我们原来只实现 CNS=0/1。**结果是：host 能连上、能读到控制器信息，然后发现"这个控制器没有
namespace"** —— 看起来像数据面故障，实际是少了一个 admin 分支。

#### (2) `Identify CNS=3`（Namespace Identification Descriptors）

每个描述符 4 字节头（type, length, reserved[2]）+ 值。host 需要 CSI 描述符
（type=0x03, len=1, value=0x00 = NVM）。Linux 在缺失时会退回到"假定 NVM"而不是失败——
**正因为它是"能过但不正确"，才更容易一直不被发现。**

#### (3) `Property Get` 必须按寄存器分别回答

原来 `result = st.enabled ? 1 : 0` —— **对任何 offset 都返回同一个值**。
这在 CAP / VS / CC / CSTS 四个寄存器里有三个是错的。现在按 offset 分派：

```
CAP=0x200001001F   (MQES=31, CQR=bit16, CSS.NVM=bit37)
VS =0x10400        (NVMe 1.4)
CC =1
CSTS=1             (RDY)
```

断言特意检查"四个值里至少有三个不同"——**返回同一个值的实现对每条单独的
"读到了非零值"断言都会通过**，只有互相比较才抓得住。

#### (4) `maxcmd`

`nvmeof_wr16(id + MAXCMD, ...)` 之前是 0。host 用它定 tag set 深度。

#### F6 现在的完整 host 形状序列（26 条断言，0 失败）

```
[PASS] Connect qid=0 establishes the admin queue - cntlid=1
[PASS] Set Features: Number of Queues - asked for 2, controller granted 2
[PASS] Get Features: Number of Queues agrees with the Set
[PASS] Set Features: Keep Alive Timer - controller uses 120 s granularity
[PASS] Keep Alive is accepted once KATO is set
[PASS] Identify Controller reports fabrics-correct fields
       - sqes=0x66 cqes=0x44 icdoff=0 kas=1 maxcmd=32 sn='NDVMEOF0000000000001'
[PASS] Identify Active Namespace ID list (CNS 2) - count=1 firstNsid=1
[PASS] Identify Namespace Identification Descriptors (CNS 3) - nidt=0x03 nidl=1 csi=0x00
[PASS] Identify Namespace geometry for the id the list returned - nsze=2048 block=512
[PASS] Property Get answers each register separately - CAP=0x200001001F VS=0x10400 CC=1 CSTS=1
[PASS] fabrics Connect on the I/O queue's own queue pair - qid=1 / qid=2
[PASS] every granted I/O queue was established without Create SQ/CQ - 2 of 2
[PASS] WRITE then READ round trips on this I/O queue - qid=1 / qid=2 (bad=0)
[PASS] all four PCI-only queue commands are refused - 4 of 4 refused
[PASS] Disconnect is acknowledged before the controller tears down
```

wire 自检也补了 CNS 2/3、四个 property offset 互不相同、以及 CAP 三个位域的回读检查
（`CAP` 是位域拼出来的，位移写错不看回读是发现不了的）。

### 8.27 ✅ 找到了 host 判断"能不能用"的那张清单，并逐条对上

上一轮补的是"host 会发的命令"。这一轮直接去读 host 侧代码，
找到了**它决定接受还是拒绝我们的那个函数** —— `nvme_check_ctrl_fabric_info()`
（`drivers/nvme/host/core.c`）：

```c
	/* In fabrics we need to verify the cntlid matches the admin connect */
	if (ctrl->cntlid != le16_to_cpu(id->cntlid)) { ... "Mismatching cntlid" ... return -EINVAL; }

	if (!nvme_discovery_ctrl(ctrl) && !ctrl->kas) {
		dev_err(... "keep-alive support is mandatory for fabrics\n"); return -EINVAL;
	}
	if (nvme_is_io_ctrl(ctrl) && ctrl->ioccsz < 4) { ... return -EINVAL; }
	if (nvme_is_io_ctrl(ctrl) && ctrl->iorcsz < 1) { ... return -EINVAL; }
	if (!ctrl->maxcmd) { dev_warn(... "Firmware bug: maximum outstanding commands is 0\n"); }
```

**五条，全是硬门。** 现在 F6 里**直接按这个谓词断言**而不是分别断言各字段——
因为这就是内核会算的那个表达式，失败时能指出是哪一条：

```
[PASS] the host's fabric-info checklist passes on every clause
       - cntlid=1 vs 1, kas=1, ioccsz=8, iorcsz=1, maxcmd=32
```

**其中 `kas != 0` 是 "keep-alive support is mandatory for fabrics"，一次硬拒绝。**
上一轮把 KAS 从 0 改成 1 —— 当时看是"字段填得对不对"，
实际上是**不填就必然连不上**。这条属于事后才明白分量的一类。

#### 同时查出四处"Linux 会检查、我们没检查"

| 位置 | Linux 的行为 | 我们原来 | 现在 |
|---|---|---|---|
| Connect capsule 的 `recfmt`（偏移 40） | `!= 0` → `CONNECT_FORMAT \| DNR` | **完全没看** | 检查并拒绝 |
| Connect data 的 `cntlid`（admin 连接） | `!= 0xFFFF` → `CONNECT_INVALID_PARAM` | 没看 | 检查并拒绝 |
| I/O 队列 Connect 的 `sqsize` | `qid && sqsize > MQES` → 拒绝（MQES 来自 CAP） | 没看 | 检查并拒绝 |
| 同一 qid 二次 Connect | → `CMD_SEQ_ERROR`(0x0c) | 没看 | 检查并拒绝 |

**共同点：我们会对真实控制器会拒绝的请求回 success。** 这不是"宽松"，
而是让 host 和控制器对"达成了什么"产生分歧 —— 正是 F5 要防的那类问题。

#### 另外三处

**(1) `Property Get` 的 `attrib` 字节选的是访问宽度。**
Linux 的 target 按它分表：**8 字节读只允许 CAP，4 字节读只允许 VS/CC/CSTS/CRTO**。
我们原来完全忽略它。现在按宽度分派，而且 F6 读 CAP 时用 8 字节、其余用 4 字节——
**照着真实 host 的读法发**，否则测的是一种 host 不会采用的访问方式。

**(2) `CAP.TO = 0` 让 host 只有一次机会。**
`nvme_wait_ready()` 是**先读一次 CSTS，再判断是否超时**，期限是 `(CAP.TO + 1) / 2` 秒。
TO=0 → 期限为 0 → **只要第一次读到的不是 RDY 就立刻放弃**。
我们因为"写 CC.EN 的同时就置起 RDY"而侥幸通过，**但那是靠零抖动，不是靠设计**。
现在报 TO=30（15 秒余量）：

```
[PASS] CAP advertises a non-zero ready timeout and MQES - CAP=0x201E01001F TO=30 (15.0 s)
```
（`0x201E01001F` 逐位核对：MQES=0x1F、CQR=bit16、TO=0x1E、CSS.NVM=bit37。）

**(3) `cattr` bit 2 = DISABLE_SQFLOW。**
host 声明"我不跟踪 SQ head"，控制器此时应把 CQE 的 SQHD 报成 `0xffff`
（Linux 的 `nvmet_install_queue` 就是这么做的）。
我们原来恒报 0 —— **报了一个 host 明确说过不维护的头指针**。现在按标志位报。

#### F6 现在 28 条断言，0 失败

wire 自检也补了 CAP 四个位域的回读、property 访问宽度、SQFLOW 位、以及 `CMD_SEQ_ERROR`。

## 6. 验收（按这个顺序，从弱到强）

1. wire 自检通过（**已完成**，C 与 C++ 双份 `/W4 /WX`）
2. 我们的 initiator ↔ 我们的 target：Identify 拿到一致信息（**已完成** F1/F6）
3. 我们的 initiator ↔ 我们的 target：写后读内容逐字节一致（含边界：1 块、整 ns、
   跨越我们内部 chunk 边界、SLBA 越界必须被拒）（**已完成** F3）
4. 错误路径：对端中途消失、connect 参数非法、SGL 类型不支持 → 有界报错，不挂死
   （**已完成**：SGL 类型与 invalidate 描述符= F3，connect 参数 = F6，**对端中途消失 = F7**，
   **双向**：host 消失时 target 也必须自己退出，见 §8.39(3)）
5. **我们的 initiator ↔ Linux `nvmet`（独立对端）** ← 真正的互操作验收（**blocked**，见 §8.18；
   夹具已就绪：`run_f5.ps1 -initiatorOnly` + `linux/nvmet_setup.sh`，步骤见 `INTEROP_F5.md`）
6. 吞吐 vs 裸 RDMA Write 2.53 GB/s 基线（**已完成** §8.10/§8.16：~1.1–1.2 GB/s ≈ 48%）
7. 我们的 target ↔ Linux `nvme-cli`（反向互操作）（**blocked**；
   夹具已就绪：`f5_interop.exe -target`，Linux 侧 `nvme connect` 步骤见 `INTEROP_F5.md` 第 2 节）

第 5/7 项需要 Linux。**当前机器开不了 VM**（BIOS 里 SVM 关着），
所以要么改 BIOS + 重启，要么 U 盘启动 Linux。这两项在此之前只能标为 blocked，
**但第 1–4、6 项已经全部做完**。

### 8.35 ✅ F7：第 4 项里最后一条"对端中途消失"补上了

`f7_faults.cpp`。第 4 项的三条里，SGL 类型（F3）和 connect 参数（F6）都有测试，
**只有"对端中途消失"一条空着** —— 而它恰恰是最要紧的一条，因为
**一个挂死的控制器比一个报错的控制器更糟**。

失败是 target 自己注入的：服务满 N 条 I/O 命令后直接 `ExitProcess(0)`。
**故意不是优雅 Disconnect** —— Disconnect 是 initiator 有权收到的协议消息，
而这个测试关心的是它永远收不到的情况。从 initiator 的角度，
对端进程崩掉和对端机器掉电是同一件事。

两条用例，各 1 次跑 2 遍，全部 0 失败：

**用例 A：根本没有 target 在监听**

```
[PASS] with no target listening, admin Connect fails rather than hanging
       - resp=0xC0000236 ND_CONNECTION_REFUSED after 0 ms
```

**用例 B：target 服务 4 条命令后蒸发**

```
[fault] served 4 I/O commands, vanishing now (no Disconnect, no cleanup)
[fault] WRITE 2 failed after 31 ms (transportOk=0)
[PASS] the peer's disappearance is detected, not waited on forever
       - 4 ok (2 write, 2 read, 2 read-verified), 1 failed, detected after 31 ms
[PASS] detection happened within the deadline rather than at the process timeout
[PASS] every READ that claimed success delivered its payload
[PASS] at least one command completed before the fault, so the path was live
[PASS] tearing down queue pairs whose peer is gone is bounded - queue teardown took 0 ms
```

**四条断言各自针对一种明确的失败模式，值得逐个说明为什么它们不是凑数的：**

| 断言 | 如果它不成立，说明什么 |
|---|---|
| 失败**被检测到** | 对端没了却永远沉默 —— 这正是"挂死" |
| 检测是**有界的** | 不是"最后跑完了"，而是在期限之内；31 ms 而不是 5 s 的 reap 超时，说明传输层自己把 queue pair 刷出来了 |
| **成功的 READ 都真的拿到了数据** | RDMA 传输可以"已投递、传了一半、然后被放弃"。完成包到了、数据没到，就是这里要挡的 |
| 对端消失后的**拆除也是有界的** | 释放一条对端已不存在的 queue pair，正是最容易阻塞在一个永远完不成的握手上的地方。实测 0 ms |

**注意"检测到"和"没挂死"是两件事**，测试分开断言：前者证明传输层报了错，
后者证明我们的重试/等待逻辑有上限。只看后者的话，一个"等满 20 秒再报错"的实现也会通过。

### 8.29 ⛔ 两个常量是我凭记忆编的，而我们自己的测试一直在为它们背书

这一轮去读 `include/linux/nvme.h`，撞上了这个工程从第一版就写在 §7 里的
**头号风险**，而且它已经真的发生了：

> 两个端点由同一个作者写成 —— 它们互相一致，并且一起和真实世界不一致。

#### (1) `NVMEOF_NIDT_CSI` 写成了 `0x03`，实际是 `0x04`

```c
enum {
	NVME_NIDT_EUI64		= 0x01,
	NVME_NIDT_NGUID		= 0x02,
	NVME_NIDT_UUID		= 0x03,
	NVME_NIDT_CSI		= 0x04,     // ← 我们写的是 0x03
};
```

我原来写的是 `CSI=0x03 / UUID=0x01 / NGUID=0x02` —— **三个里有三个不对**。
后果不是"字段有点歪"，而是：**Linux host 解析我们的 CNS=3 响应时，
看到的是 `nidt=0x03`（UUID）配 `nidl=1`，也就是去读一个 1 字节的 UUID。**

**这个错误此前一直"通过"**，因为 target 和测试用的是同一个错常量。
上一轮我甚至专门为这个写了断言（`nidt=0x03 nidl=1 csi=0x00`）并且 PASS ——
**断言写对了，被断言的值是错的。** 这正是自洽测试的盲区。
修完打印变成 `nidt=0x04`。

#### (2) Keep Alive 超时的状态码是**通用**的 `0x1A`/`0x19`，不是我编的 `0x01`/`0x02`

```c
	NVME_SC_KA_TIMEOUT_EXPIRED	= 0x19,
	NVME_SC_KA_TIMEOUT_INVALID	= 0x1A,
```

它们和 `NVME_SC_SUCCESS`、`NVME_SC_INVALID_OPCODE` 一样在**通用状态码**块里（SCT=0），
而我在头文件里把它们放进了"命令特定"块（SCT=1）并标成 0x01/0x02。

**host 是先用 SCT 读出类型、再读码值的**，所以码值对、类型错，状态依然是不可读的。
现在三个 target（F3/F4/F6）都改成 `sct = GENERIC`。

#### 这一轮的教训比修掉的两个值更重要

前几轮我一直在做"读参考实现、对照我们"的事，并且确实抓到了建队模型、
四个缺失的 admin 命令、fabric-info 硬门。**但那些都是"有没有实现"，
而这一轮是"实现的值对不对"** —— 后者更隐蔽：

- 缺一个命令，行为会明显不对；
- **一个常量编错，只要两端同源，测试会一路绿灯，直到接上真实对端。**

现在的防线是把 Linux 头文件里的值**抄进自检并注明出处**（`test_admin_cases_a_host_needs`），
包括"四个描述符类型必须互不相同"这条 —— 它专门针对"CSI 塌到 UUID 上"这个具体错误。

**仍然是同一句老话：自洽的测试只能证明两个端点互相一致。**
区别在于，这一轮是它第一次真的把一个错值放过去了。

### 8.31 ⛔ 我们宣称了一个自己会拒绝的能力（SGLS bit 20）

接着上一轮的方法继续对 `include/linux/nvme.h`，又抓到一个**比常量写错更严重**的：

```c
	NVME_CTRL_SGLS_BYTE_ALIGNED = 1,
	NVME_CTRL_SGLS_DWORD_ALIGNED = 2,
	NVME_CTRL_SGLS_KSDBDS       = 1 << 2,
	NVME_CTRL_SGLS_MSDS         = 1 << 19,
	NVME_CTRL_SGLS_SAOS         = 1 << 20,
```

**bit 20 是 `SAOS` —— "SGLs are supported with an Offset"，也就是 in-capsule data。**

我头文件里写的是：

```c
#define NVMEOF_CTRL_SGLS_24BIT_LEN  (1u << 20)  // 24-bit length in keyed descriptor
```

两个错叠在一起：

1. **"keyed 描述符的长度是 24 位"是描述符格式本身的性质，根本没有对应的能力位。**
   我为它编了一个位。
2. 编出来的位落到 **bit 20 = SAOS** 上，于是**四个 target 全部都在宣称
   "我支持 in-capsule data（用 offset 描述符寻址）"** ——
   而我们的 `parseSgl` 只认 type 4，**遇到 offset 描述符会当作非法 SGL 类型拒绝**。

**所以：我们对 host 说"这个请求合法"，然后在它发过来时回错误。**
这比少一个能力危险得多——host 没有理由怀疑一个它被明确告知支持的请求。

修法：按 Linux 的名字和值重写，并且**只宣称我们真的做的**：

```c
#define NVMEOF_CTRL_SGLS_ADVERTISED \
    (NVMEOF_CTRL_SGLS_BYTE_ALIGNED | NVMEOF_CTRL_SGLS_KEYED)
```

顺带发现 **`BYTE_ALIGNED`（bit 0）原本没设**，而 Linux 的 nvmet 对每个 target 都设。
F1 的断言也从"检查那个编出来的位"改成**检查我们没宣称 SAOS**：

```
[PASS] controller advertises keyed SGLs and does not claim in-capsule data
       - sgls=0x00000005 (keyed=yes, byte-aligned=yes, in-capsule=not claimed)
```

（4 → 5：去掉 bit 20，补上 bit 0。）

#### 附带：`ONCS` 里也有一个编出来的位

```c
#define NVMEOF_CTRL_ONCS_VERIFY       (1u << 7)
```

Linux 的 ONCS 表里没有 verify（bit 7 不是 ONCS 位）。虽然当前没人用它
（F6 报 oncs=0），但**一个编出来的能力位比一个缺失的更糟**——缺失最多是不支持，
编出来会让 host 相信一个不存在的承诺。已按 Linux 的六个位重写。

#### 这一轮和上一轮合起来说明的事

两轮下来一共修掉 **四个凭记忆编造的常量**（NIDT_CSI、KA_TIMEOUT 状态码、
SGLS bit 20、ONCS bit 7），其中三个此前一直"通过"我们自己的断言。

**规律是：凡是"我记得是……"而手边没有头文件的地方，错误率是 100%。**
现在所有这类取值都在 `nvmeof_wire.h` 里**注明来自 `include/linux/nvme.h` 并逐位核对**，
自检里也加了"四个描述符类型必须互不相同""SGLS 不得宣称 SAOS""ONCS 各位必须互不相同"
这类**针对具体错误形态**的断言，而不只是检查非零。

### 8.33 ✅ 那些"故意留 0"的字段是承重的，现在有断言替它们说话

接着对 `include/linux/nvme.h` / `core.c`。这轮不是抓到错值，而是抓到一件
**一直没写下来、却一直在替我们挡事的事实**。

#### `oaes = 0` 是"我们不需要实现 Async Event"的唯一原因

host 侧：

```c
#define NVME_AEN_SUPPORTED \
	(NVME_AEN_CFG_NS_ATTR | NVME_AEN_CFG_FW_ACT | \
	 NVME_AEN_CFG_ANA_CHANGE | NVME_AEN_CFG_DISC_CHANGE)

static void nvme_enable_aen(struct nvme_ctrl *ctrl)
{
	u32 result, supported_aens = ctrl->oaes & NVME_AEN_SUPPORTED;
	if (!supported_aens)
		return;                      // ← 我们一直从这里返回
	status = nvme_set_features(ctrl, NVME_FEAT_ASYNC_EVENT, supported_aens, ...);
```

`oaes` 是 Identify Controller 偏移 92 的字段。**我们报 0 → `supported_aens == 0`
→ host 直接 return → 既不 Set Features: Async Event，也永远不发 Async Event 命令。**

所以"`Abort`/`Async Event` 还没做"**不影响 F5** —— 但这是 `oaes=0` 换来的，
不是我推理出来的，是代码里那条 `if (!supported_aens) return;`。

#### 同样的机制还有六个

| 字段 | 留 0 挡住了什么 |
|---|---|
| `oaes` | Set Features: Async Event + Async Event 命令（以及 AEN 的延迟完成机制） |
| `lpa` (bit 1) | **Command Effects log**，那是一次我们没实现的 Get Log Page |
| `apsta` | **APST**，那是 Set Features: Auto PST 配 256 字节负载 |
| `oncs = 0` | timestamp Set Features；host 也不会发 DSM / Write Zeroes / 预留 |
| `ctratt = 0` | host behaviour Set Features（CRDT/ELBAS 相关的门） |
| `cmic = 0` | ANA log 读取；同一 subsystem 的第二个控制器会被拒——而这是对的，我们只服务一个 |
| `oacs = 0` | namespace 管理、security、directives |

**换句话说：我们"没实现 Set Features 之外的 admin 命令"之所以没出事，
是因为这些位全是 0，host 压根不会去发。** 这是一层真实存在的、却没有任何注释保护的防线——
只要有人为了"填得完整点"把 `lpa` 或 `oaes` 设上，host 立刻会开始发我们答不了的命令。

#### 所以这轮做的事：给这层防线加上会说话的断言

F6 现在有一条断言，**逐字段报出这个零挡住了什么**：

```
[PASS] capability fields stay zero so the host does not send what we cannot answer
       - oaes=0(no AEN cmd) lpa=0(no effects log) apsta=0(no APST)
         ctratt=0(no host-behaviour) cmic=0(no ANA) oacs=0 oncs=0
```

改任何一个值的人，**必须先反驳括号里的理由**，而不是只面对一个 0。
头文件里对应位置也写了同一张表和一句话：
**"实现一个特性时的顺序是：先实现它激活的那条命令，再设那个位。"**

#### 顺带补上的偏移与位定义

`CMIC(76)` / `IEEE(73)` / `OACL(258)` / `AERL(259)` / `FRMW(260)` / `LPA(261)` / `APSTA(265)`
/ `OAES(92)` 都补进头文件并逐个在自检里钉住——**偏移写错等于让 host 把别的字段当成能力位读**，
后果和上一轮的 SGLS bit 20 是同一种。

另外 `NVMEOF_OAES_DISC_CHANGE` 是 bit 31：放在 `enum` 里会让 MSVC 报 C4308
（枚举值放不进 `int`，被转成负数）。值本身最终还是对的，但**要读者去推敲一次符号变化**，
而且负的掩码在别处比较会出错，所以改成 `#define` + 无符号字面量。

### 8.37 ✅ 常量交叉核对工具化，以及一条我上次只修了一半的时序规则

#### (1) 把"逐个读头文件"变成一条能重跑的命令

前面四轮抓到的 bug 全是同一类：**常量是凭记忆写的，而 target 和测试共用这个错值，
所以一路绿灯**（NIDT_CSI、KA_TIMEOUT 状态码、SGLS bit 20、ONCS bit 7）。
"认真读一遍头文件"就是找到它们的方法，**也正是一件人只会认真做几次的事**。

现在有了 `src/xref_constants.py` + `ref/linux_nvme.h`（Linux `include/linux/nvme.h`
的原文副本，保留在仓库里以便复现和查证）：

```
$ run_xref.ps1
  parsed from reference: 660 constants; from ours: 216
  compared : 59
  mismatched: 0
  RESULT: PASS - every checked constant matches the reference header
```

59 对常量逐值比对，覆盖 fabrics fctype、Connect/queue 模型、CNS、
namespace 描述符类型、能力位、AEN 掩码、feature id、状态码、SGL 类型/子类型、
namespace 字段、寄存器偏移。**上一轮手改的四个值现在是被机器确认的，而不是我记得对的。**

**这个工具自己也犯了三次同类错误，值得记下来，因为它们全都表现为"参考里没有这个常量"：**

| 我写的 | 后果 |
|---|---|
| 允许字符集漏了 `x` | **所有 `0x..` 十六进制值解析失败** —— 看起来像参考头文件没定义它们 |
| 整数字面量的后缀（`1u`）被当成标识符拒绝 | 我们这边所有带后缀的常量都"找不到" |
| `enum\s*\{` 不认带标签的枚举 | `enum nvmf_capsule_command {` 这种**全部跳过**，而重要的那些恰好都带标签 |

**一个检查器失败时会安静地少检查，然后报告通过** —— 这和它要防的 bug 是同一种。
所以脚本现在会打印"解析到多少个常量"，并且**把"配对数"当失败条件**：
配对列表里出现解析不到的常量就判 FAIL，而不是跳过。

#### (2) 一条我上次只修了一半的时序规则

F1 又出现了老症状（`status=0xFFFF`，"never became ready"）。上一轮我把它归结为
"Receive 必须在 Accept 之前挂"，改了之后确实过了很多次 —— **但那只修了一半。**

完整规则是两条：

1. **挂 Receive 要在 Accept 之前**（否则第一个命令丢）。
2. **重挂 Receive 要在"发送应答"之前**（否则第二个命令丢）。

F1 的循环是"消费 capsule → 处理（内含发送应答）→ 下一轮开头才重挂"，
**应答先出门，Receive 后挂上**。对端看到应答就立刻发下一个命令，
那个命令到达时队列上什么都没有 —— 而按 §8.10(4) 的结论，**这种情况不是可重试的，
Send 直接永远不完成**。症状正好是 `Property Set` 过、`Property Get` 挂。

修法：**把 capsule 拷出来 → 立刻重挂 → 再处理**（拷出来是因为要重挂的正是
handler 还在读的那块缓冲区）。F1 改完连跑 4 次 0/0。

#### (3) 同一条规则在其余 target 里的状态 —— 说清楚哪修了、哪没修

| 位置 | 顺序 | 状态 |
|---|---|---|
| F1 admin | 消费 → 处理/发送 → 下轮重挂 | **已修**（拷出 → 重挂 → 处理） |
| F6 IO（`serve`） | 处理 → 发送 → 重挂 | **已修**（处理 → 重挂 → 发送） |
| F6 admin（`targetAdmin`） | 处理 → 发送，调用方重挂 | **已修**（handler 内先重挂再发送，调用方不再重挂） |
| F3 admin / F3 io | 处理 → 发送 → 紧接着重挂 | **未改**，见下 |
| F4 io（6 处）/ F7 admin+io | 同上 | **未改** |

**为什么没全改，以及为什么它们一直没出事**：这些位置的"发送 → 重挂"之间只隔一个
函数返回和一次调用（几百纳秒），而对端要收到应答、处理、再发下一条命令，
中间有一个网络往返（几十微秒）。**这个窗口几乎总是被 target 赢下**，
所以它们是"没被测出来"，不是"没有问题"。

**这是一条已记录但未全部消除的隐患，不是已完成项。** 修它不需要新知识，
只需要在每个 handler 里把两行的顺序换过来 —— 记在这里，免得下次当成新发现。

### 8.39 ✅ 时序隐患清完，以及一条被误解了很久的 `ND_CANCELED`

#### (1) §8.37(3) 表格里"未改"的那几处，现在全改了

| 位置 | 改法 |
|---|---|
| F3 admin / io | handler 内部**先重挂再发送**；调用方不再重挂 |
| F4 io（6 处） | 一律"重挂 → 发送" |
| F4 admin | 同上（`targetAdmin` 内重挂，调用方不重挂） |
| F7 admin / io | 同上；`targetAdmin` 内重挂，调用方不再重挂 |
| F6 io（`serve`） | 已在 §8.37 改过 |

改的过程中发现 **F4 的 I/O capsule 分支少一个 `}`**（上一轮加"立刻重挂"时吃掉的）：
`f4_pipeline.cpp` 处于**从未编译过**的状态，而它的 `.exe` 还是更早那次构建留下的 ——
第一次真正编译就报 `C1075`。这里没有造成误判，纯粹是因为编译先于运行；
`run_f4.ps1` 的"删掉 exe + 看编译退出码 + 比时间戳"三件套本来也会拦住它。

顺带把 F3 的两个队列的 capsule 缓冲区分开了（admin 用 `kCapInOff`，I/O 用
`kCapInOff+64`）。它们原来共用 64 字节：F3 的 host 是严格一问一答，所以**从未暴露**，
但第一个把 admin 和 I/O 命令重叠发出去的 host 会踩到。

#### (2) `ND_CANCELED` 不是"丢了一个消息"，是"这个 queue pair 到此为止"

我按 §8.8(2)/§8.16(4) 记的"listener 连接请求会取消另一个 QP 上的 Receive，
所以要重挂"去写重挂逻辑，结果**打开了两个无限循环**：

- F4 的 target 日志：**973 行** `Receive did not deliver a capsule (st=0xC0000120); re-arming`，
  一直打到 10 秒空闲超时才停（这是**旧代码**本来就有的行为，不是新引入的）；
- F3 的 target：**1652 行**，直接把我新加的分支打到被测试脚本 kill 掉。

规律很清楚：**一旦 Receive 以 `ND_CANCELED` 回来，之后每挂一个 Receive 都立刻再以
`ND_CANCELED` 回来** —— 重挂不是恢复，是自旋。所以四套 target 现在统一成
"**`ND_CANCELED` = 该 queue pair 结束**"：报告一次、停止轮询它，两个队列都结束了就退出循环。

**那么"admin Receive 莫名其妙被取消"到底是什么？** 这次是测出来的，不是推的：
给 F4 加了一条**真实主机一定会做**的断言 —— 在 16 MiB 流水线 I/O 跑完之后，
在 admin 队列上补一条 Keep Alive。结果：

```
[admin] Identify cns=0
[admin] Keep Alive                        <- I/O 全部跑完之后才收到的
[dbg] admin cqe ... st=0xC0000120 bytes=0  <- 这之后才是取消
[admin] ... queue pair declared dead
```

**Keep Alive 被正常应答（`status=0x0001`）**，取消是在 host 进程退出时才出现的。
所以 admin queue pair 在整个 I/O 阶段一直是活的，
**"发送之后重挂 Receive"才是当时那个 bug 的真正成因**（§8.37(2)），
不是"队列被打死了"。§8.8(2) 里"GetConnectionRequest 会打死另一个 QP"这条**降级为未复现**：
F3 全程没有出现过中途取消，F4 的取消全部发生在一端消失之后。
那两处"保险式重挂"保留，但它们现在只是一次多余 post，不再是解释。

F4 现在多一条断言（`admin queue still answers Keep Alive after the pipelined load`），
它把"队列是不是真的死了"变成一条能过的测试，而不是日志里的怪现象。

#### (3) F7 补了反向故障：host 先消失，target 必须自己发现

原来是"target 中途消失，看 host 多久发现"。反向那一半同样重要，而且更能暴露问题：
**target 没有空闲超时**，如果它不把取消当成结束，就会一直抱着一个已经不存在的 host 的
queue pair 不放。新增 case C：initiator 跑 4 条 I/O 命令后 `ExitProcess`（不做 Disconnect、
不做拆除），target 必须在 15 秒内**自己退出**并且日志里说明它为什么知道对端没了。

结果：`[admin]/[io] ... declared dead` → `both queue pairs returned ND_CANCELED;
the initiator has gone` → 自行退出，`target failures: 0`。
F7 现在是 A/B/C 三个 case。

### 8.40 ⛔ 把 Linux 当参考实现读，抓到三个"两边都同意、世界不同意"的缺陷

本轮没能拿到 Linux（BIOS 里 SVM 仍然关着，WSL2/VM/QEMU 全部起不来，磁盘上也没有 ISO），
所以换了一个不需要 Linux 的办法：**把 Linux 的驱动当参考实现逐行读，然后拿它的规则检查我们**。
读的是：`drivers/nvme/host/rdma.c`、`drivers/nvme/target/rdma.c`、
`drivers/nvme/target/admin-cmd.c`、`drivers/nvme/host/core.c`、`include/linux/nvme-rdma.h`。

**读源码不等于跑起来**（下面这些仍然是"按参考核对过"，不是"跑通验证过"），
但它一次就抓到了三个**我们自己的 host 和 target 都同意、而 Linux 不会同意**的缺陷 ——
正是 §8.29 记下的那种风险，只不过这次是结构而不是常量。

#### (1) 我们的 host 根本连不上 Linux 的 target（两个独立的错）

`nvmet_rdma_parse_cm_connect_req()`：

```c
if (!req || conn->private_data_len == 0)  return NVME_RDMA_CM_INVALID_LEN;
if (le16_to_cpu(req->recfmt) != NVME_RDMA_CM_FMT_1_0) return NVME_RDMA_CM_INVALID_RECFMT;
queue->recv_queue_size = le16_to_cpu(req->hsqsize) + 1;
if (!queue->host_qid && queue->recv_queue_size > NVME_AQ_DEPTH)
        return NVME_RDMA_CM_INVALID_HSQSIZE;
```

| 谁 | 原来发的 | Linux 的反应 |
|---|---|---|
| F1 / F3 / F4 | `Connect(..., nullptr, 0, ...)` **完全没有私有数据** | `NVME_RDMA_CM_INVALID_LEN`，连接被拒 |
| F6 / F7 | `hrqsize = 32, hsqsize = 32` | `32 + 1 > NVME_AQ_DEPTH(32)` → `INVALID_HSQSIZE`，连接被拒 |

也就是说**五套 initiator 没有一套能连上 Linux**，而五套里的每一套都能连上我们自己的 target
—— 因为我们的 target 从来没看这两个字段。Linux 的 host 发的是
`hrqsize = 队列深度, hsqsize = 深度 - 1`（admin 队列固定 32/31）。

修法：`nvmeof_rdma_fill_req()` 一个函数填对，五个 initiator 全部改用它；
target 侧改用 `nvmeof_rdma_check_req()`，**按 nvmet 的同一顺序、同一错误码**校验，
并且**总是回 32 字节的 accept 私有数据**。这样 host 的错就再也藏不住了：
target 现在会像 nvmet 一样把没有私有数据的连接拒掉。

#### (2) 我们的 target 会拒绝 Linux host 的**每一条** I/O（§8.23 的结论被推翻）

`nvme_rdma_map_data()` → `nvme_rdma_map_sg_fr()`：

```c
sg->type = (NVME_KEY_SGL_FMT_DATA_DESC << 4) | NVME_SGL_FMT_INVALIDATE;   // 0x4f
```

默认参数（`register_always=true`）下 **Linux host 对每一条 Read/Write 都打 0x4f**。
而我们的 target 从 §8.23 起**拒绝 0x4f**（`SGL_INVALID_TYPE`）。
按原样上 Linux，方向 B 会是"100% 的 I/O 被拒"。

§8.23 当时的理由：*"传输能做、失效做不到，照做就等于告诉 host 它的 STag 已经死了而其实还活着。"*
**这条理由是错的**，参考实现自己写明了这一点 —— Linux host 的完成路径：

```c
if (wc->wc_flags & IB_WC_WITH_INVALIDATE) { 校验 rkey }
else if (req->mr) { ib_post_send(IB_WR_LOCAL_INV) }   // 没有收到失效就自己本地作废
```

host **不假设**我们做了失效；收到普通 SEND 时它自己作废。所以"传输 + 普通 SEND"
不是撒谎，而是 host 明确支持的两种结果之一。**不可存活的选项是拒绝**，不是照做。

改法：0x4f **照做**（传输 + 普通 SEND），计数器从 `invalidateRefused` 改成
`invalidateServed`，F3 的断言反过来：不但要成功，还要**逐字节验证数据真的搬了**
（否则"只回成功不搬数据"也能过）。

#### (3) Identify 与命令集里几个"承诺和实力不一致"的字段

| 字段 | 我们原来 | 参考实现 | 为什么必须改 |
|---|---|---|---|
| `ioccsz` | 8（128 B） | 4（=`sizeof(nvme_command)`，nvmet 在 `inline_data_size=0` 时） | Receive 缓冲只有 64 字节，说 8 就是承诺收 128 字节的 capsule |
| `icdoff`（F1） | 2 | 0 | 既不是 0 也推不出 64 字节 SQE 的偏移（那该是 4），纯属编的 |
| `vwc` | 1 | nvmet=1 | 我们是内存命名空间，说 1 会让 host 在每次 fsync/umount 前发 Flush，而我们用 Invalid Opcode 回答 → host 侧 I/O 报错 |
| KATO `Get Features` | 秒 | **毫秒** | `nvmet_set_feat_kato` 返回秒、`nvmet_get_feat_kato` 返回毫秒，这是参考实现的不对称，F3/F7 两边都回了秒 |
| Flush（0x00） | 未实现 | 实现 | 和 admin 的 Delete SQ 同 opcode，只有队列能区分；现在实现并断言 |

`vwc` 选 0 而不是 1：这个命名空间就是进程内存，没有易失缓存可刷 —— 说 0 是**准确的**，
同时仍然回答 Flush（host 出于自己的理由发来时不能报错）。

#### (4) 交叉核对工具自己第四次犯同一类错

给 `ref/linux_nvme_rdma.h`（新加的参考副本）加常量对时，工具报
"参考里没有 `NVME_RDMA_IP_PORT`"。真相是**工具把它吃掉了**：

```python
re.finditer(r"#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+([^\n/]+)", text)
```

`\s+` 能匹配换行，于是 `#define _LINUX_NVME_RDMA_H`（没有值）把**下一行的宏整个当成了它的值**。
改成 `[ \t]+` 之后 73 对常量全部解析、全部相符。

**这次是工具自己抓住的**：配对列表里出现解析不到的名字会判 FAIL 而不是跳过 ——
这正是 §8.37 加的那条规则的用处。检查器少检查然后报通过，和它要防的 bug 是同一种，
这是第四次了（前三次：漏 `x`、整数后缀、带标签的 enum）。

#### (5) 新断言当天就抓到一个真错

F5 的 target 是新写的，它忘了往 Identify 里写 `cntlid`。第一次自测：

```
[FAIL] the target passes the host's fabrics checklist - cntlid=0 (Connect said 1)
```

而 Linux 的 `nvme_check_ctrl_fabric_info()` 第一句就是
`if (ctrl->cntlid != id->cntlid) 拒绝`。**一条照着参考实现写的断言，在写下的当天就抓到一个真 bug。**

#### (6) 现在的状态：说清楚"已经证明"和"还没证明"

| | 状态 |
|---|---|
| 五套 initiator 现在都发合法私有数据（`hrqsize=depth, hsqsize=depth-1`），target 按 nvmet 校验 | ✅ 有测试 |
| 0x4f 照做 + 数据逐字节验证 | ✅ 有测试 |
| `ioccsz=4 / icdoff=0 / vwc=0 / Flush / KATO 单位` | ✅ 有断言（F1/F6） |
| 常量与两个参考头文件逐值相符 | ✅ `run_xref.ps1`，73 对 |
| **与真正的非 Windows 对端跑过一次** | ⛔ **没有，一次都没有** |

最后一行是这一轮唯一没有解决的问题，而且**只有换一台 Linux 才能解决**。
为此本轮把 F5 做成"Linux 一到手就是十分钟的事"：
`src/f5_interop.cpp`（两个角色）、`src/run_f5.ps1`、`linux/nvmet_setup.sh`、
`linux/nvmet_teardown.sh`、以及 `INTEROP_F5.md`（含方向 A/B 的完整步骤、
期望输出、以及出错时看哪一条 dmesg）。

### 8.41 ⛔ "一个 Receive" 是能跑的假象：真实 host 会流水线，我们会丢命令

这一轮做的是"不需要 Linux 也能推进"的那部分：让所有 target 按 nvmet 的规则校验私有数据、
实现 Async Event、并**给 F5 的 target 加上并发能力**。后者是这轮最重要的发现。

#### (1) 单个 Receive ≠ 够用：一次流水线就丢 3/4 的命令

给 F5 host 加了一个 Async Event 测试：**连发 5 条 capsule 不等应答**。
target 只收到 **2** 条。把 admin 队列改成 **8 个槽的 receive ring**（nvmet 是 32）之后，
5 条全到。

同一件事在 I/O 队列上更致命：加一个"4 条命令在飞"的流水线测试，
第一次跑的结果是 **4 条里只有 1 条被应答**。原因是两层叠加：

| 层 | 问题 | 后果 |
|---|---|---|
| receive | I/O 队列只有 1 个 Receive | 第 2 条以后的 capsule 到了没地方落 —— **按 §8.10(4) 的实测，这种 capsule 不重试，直接丢** |
| dispatch | target 用 `q.reap(CTX_DATA, …)` 等自己的数据传输 | `reap()` **把不匹配的完成直接扔掉并打印 `ignoring completion`** —— 别的 capsule 的完成就这样被吃掉了（和 §8.16(2) F4 的 bug 2 是同一个坑，只是换了个文件） |

修法两条，都是 F4 I/O 路径早就用过的形状：

1. **ring**：admin 和 I/O 各 8 个槽，每个槽有自己的 buffer 和 ctx，谁消费谁重挂；
2. **不等待**：收到 capsule 就**启动**数据传输并返回，传输完成时再回答
   （`CTX_DATA_BASE + slot` 分派）；期间到达的其他 capsule 完成被**排队而不是丢弃**
   （`waitForData()` 把 admin capsule 塞进 `pending[]`，主循环再逐条处理）。

修完：4/4 应答、4/4 读回的数据逐字节正确，target 的 `ioCommands=11`。

**这条对方向 B（Linux `nvme-cli` → 我们的 target）是硬门槛**：
Linux host 的队列深度是 32，它**按设计流水线**。在这之前，
第 7 项验收只要一上量就会大面积丢命令，而现象是 host 侧超时、target 侧看起来一切正常。

**说清楚现状**：8 槽 ring + 延迟完成目前只有 **F5 的 target** 有。
F1/F3/F6/F7 的 target 仍是"一个 Receive + 等待传输完成"——
它们各自的 host 是严格一问一答（F4 例外，它自己就是流水线的那一方），所以测不出来。
**这是一个已知的、有意的差异**：F5 是互操作那一套，其余几套是各自主题的最小实现。

#### (2) Async Event：正确的回答是"不回答"

Linux host 只在 `oaes` 非零时才发 AEN（`nvme_enable_aen()`：`if (!supported_aens) return;`），
所以对我们**不是必需的**。但它是"打开 `oaes` 之前必须先实现"的那一步（§8.33），
而且它的正确行为很容易写错——**成功和失败都是错的答案**：
说成功等于告诉 host"有事件了"，说错误等于告诉它命令被拒。
真实控制器把它**扣住**，有事件才完成。

现在 F5 的 target 实现为延迟完成，上限取参考实现的 4（`NVMET_ASYNC_EVENTS`），
第 5 条回 `ASYNC_LIMIT`。断言三条：4 条被扣住（700 ms 内没有任何完成）、
第 5 条被拒且**确实有**完成（`sct=1 sc=0x05`）、4 条挂着时 Keep Alive 仍然正常。
`oaes` 仍然保持 0：**实现了能力，但不宣称** —— 因为我们没有任何事件会产生，
宣称它只会让 host 去设掩码然后等一个永远不会来的通知。

#### (3) 写测试时又踩了两个坑，都值得记

* **`0x105` 不是"低字节 0x05 + 通用 SCT"**。ASYNC_LIMIT 在协议里是
  `SCT=命令特定, SC=0x05`。我先写了个"≥0x100 就推成命令特定"的辅助宏 ——
  自己加的静态断言立刻把它否掉：`CONNECT_INVALID_PARAM = 0x82` 是**命令特定**的，
  而 `LBA_RANGE = 0x80` 是**通用**的，同一个低字节两种含义，**根本没有算术规则**。
  改成一个显式拆开的常量对 + 一条断言把它们拼回 0x105。
* **给"没有 Receive 的应答"发 SEND 会打死整个 QP**。测试第一次跑时，
  第 5 条 AER 的应答发出去时 host 没挂接收缓冲 —— RNR 重试耗尽后 QP 进入错误态，
  **target 上 8 个 Receive 全部以 `ND_CANCELED` 被冲掉**，target 直接退出。
  这不是"丢一条消息"，是"这一侧整个队列对都没了"。修完测试（先挂应答接收）后一切正常；
  记在这里是因为它说明了 §8.10(4) 那条规则的另一半：**没挂 Receive 不只是收不到，
  还会让发方把 QP 打坏**。

#### (4) 本轮代码改动一览

| 位置 | 改动 |
|---|---|
| `nvmeof_rdma.h` | `Queue::acceptChecked()`：GetPrivateData → 按 nvmet 校验 → Accept（带 32 字节 rep）或 Reject（带参考错误码） |
| F1 / F3 / F4 / F7 target | 全部改用 `acceptChecked()`（F1 的 `Transport` 太老，同样逻辑就地写） |
| F5 target | admin/I/O 各 8 槽 receive ring；数据传输延迟完成；`waitForData()` 排队而非丢弃；Async Event 延迟完成 |
| F5 host | 新增流水线 I/O 测试（4 条在飞）与 Async Event 测试（4 条扣住 + 第 5 条 ASYNC_LIMIT） |
| `nvmeof_wire.h` | `NVMEOF_SC_ASYNC_LIMIT`(0x105) + 拆分的 SCT/SC 对与断言、`NVMEOF_AER_MAX_PENDING`(4) |

全套复跑：`xref` PASS（73 对）／`wire` PASS／F1 0/0／F3 0/0／F4 0/0／
**F5 0/0（27 条）**／F6 0/0（31 条）／F7 0/0（A/B/C 三 case）。

### 8.42 为"最后一步"做的工程准备：链路、MTU、一条命令

第 5/7 项只差一台**本地** Linux。这一轮把"到时候要现场拼的东西"提前做成脚本并验证，
因为真正上场时任何一步出错都会表现得像协议故障。

#### (1) 云服务器不能当对端（这条必须先说清楚）

尝试过用一台阿里云服务器：**不行，而且是协议层面的不行**。
rxe 与真 RoCE 一样要求**同一二层、ARP 能直接解析**；云服务器隔着 NAT 与网关，
中间没有任何一跳会转发这类语义。更硬的一条是：我们的 RoCE 端点只能长在 CX3 的口上
（`192.168.100.2`），它唯一能 ARP 到的邻居就是对面那个 CX3 口——想让它去公网，
就得把端点绑到没有 RDMA 能力的主板网卡上。**就算硬凑通了，失败也无法区分
"我们代码错"还是"网络环境错"，这正是互操作测试最不能接受的失败模式。**

#### (2) 本机的接法：桥接，而不是换线

实测拓扑：`以太网 20`（Intel I211，192.168.1.2/24，默认路由）是局域网，
`以太网 23/24` 是 CX3 两个口（192.168.100.3/.2），两口直连。要让本地 Linux 加入这个二层：

**把 `以太网 20` 与 `以太网 23` 桥接**（`netsh bridge create <id> <id>`，本机验证可用），
于是 `笔记本 → 路由器 → 以太网 20 → 桥 → 以太网 23 → 直连线 → 以太网 24`
成了一条二层通路，而**留给 RoCE 的 `以太网 24` 不动**——正好一口一个端点。
（上一版文档里我写的 `New-NetSwitchTeam` 是网卡组合、不是桥接，已更正。）

#### (3) MTU：一个会让测试静默失败的坑

CX3 两个口当前是巨帧：**IP MTU 4074、`RoceMaxFrameSize` 2048**；
而到笔记本的路径是 **1500**。2048 字节的 RoCE 帧过不了 1500 的路径——
**不是变慢，是被丢掉**，现象是"连接超时、任何日志里都没有线索"，
和"对端没起来"完全一样。所以互操作前先把这一侧降到 1500/1024
（`interop_link.ps1 -Action LowMtu`），跑完再 `-Action RestoreMtu` 恢复实测过的快配置。
两个方向的设置都记录在 `interop_link_state.json` 里。

#### (4) 这一轮新增的东西

| 文件 | 作用 | 验证到什么程度 |
|---|---|---|
| `src/interop_link.ps1` | 桥接 + 搬 IP/路由 + 降 MTU；**先记录状态**，`-Up` 若局域网不通**自动回滚** | `-Check` 跑通并打印正确计划；`netsh bridge` 语法与适配器 id 解析实测正确；ICMP 与 TCP 双探针 |
| `linux/f5_linux_up.sh` | 对端一条命令：rxe → 加同网段地址 → ping 通 → 配 nvmet | `bash -n` 与 `--posix -n` 均通过（Git Bash）；失败路径实测给出可操作的提示 |
| `linux/nvmet_setup.sh` | 新增 `RXE_NETDEV=` 软件 RoCE 分支、UDP 4791 防火墙放行、接口/子网检查 | 同上；各发行版包名写进注释 |
| `src/run_all.ps1` | 一次跑完 8 套并给结论表 | **实测 8/8 通过，86 秒** |
| `README.md` | 工程入口：目录、怎么跑、**"接真实 host 该用哪个二进制"**、验收状态 | ― |

`bash -n` 之所以能跑，是因为本机有 Git for Windows 的 bash（`C:\Program Files\Git\bin\bash.exe`）——
`wsl.exe` 那条路是死的，但这一条能用，所以 Linux 脚本不必等到对端才第一次被解析。

#### (5) 仍然要说清楚的边界

上面这些都是**让验证能顺利发生**的准备，**不是验证本身**。
第 5、7 项的状态没有变：在真 Linux 上跑通之前，
"我们能和别的实现互通"这句话仍然**没有证据**。

### 8.43 ✅ 宣称的窗口必须真的能收：8 槽 → 32 槽

F5 的 target 在 accept 私有数据里告诉 host "我的接收队列是 32"（`crqsize = 32`），
**而它的 receive ring 只有 8 个槽**。也就是说：一个按队列深度 32 打满的 host，
第 9..32 条 capsule 会**在 target 侧无声地消失**（§8.10(4)：没挂 Receive 的 capsule 不重试）。
Linux host 的默认队列深度正是 32 —— 这一条对方向 B 是致命的，
而它不会以"错误"的形式出现，只会以"host 超时"的形式出现。

修法两件：

1. **ring 提到 32**，并且用编译期断言把两个数字绑在一起：
   `static_assert(kCapSlots >= kSqDepth, "the capsule ring must cover the crqsize the target advertises")`
   —— 这样它们再也不会各自漂移。
2. **测试真的打满这个窗口**：流水线测试从 4 条在飞提到 **32 条**（就是宣称的窗口大小）。
   顺带修了测试自己的一个 bug：它把 32 个应答接收全挂进**同一个 16 字节缓冲**，
   于是每个回答覆盖前一个，读到的状态是"最后一个到的"——
   现在每个在飞命令有自己的应答缓冲和 ctx。

结果：**32 of 32 commands answered, 32 with success**；读回 32/32 条各自的数据，
0 字节不同；target `ioCommands=67`。

这条也说明一件更大的事：**"我们宣称的能力"和"我们实际的实现"之间需要一条断言**，
否则两者可以各自正确、合起来骗人 —— 和 §8.29 那四个常量是同一种病，
只不过这次骗人的不是常量值，而是队列深度。

### 8.44 ⛔ 第 5 个"凭记忆编的线上常量"，以及读 header 才发现的属性宽度规则

这一轮把 fabrics 命令的**数据长度规则**拿去和参考实现核对
（`drivers/nvme/target/fabrics-cmd.c`），一次抓到两个都在关键路径上的缺陷——
而且都在 host 的**第一条**和**最后一条**命令上。

#### (1) `NVMEOF_FCTYPE_DISCONNECT = 0x08` 根本不存在

参考头文件的枚举只有五个值：

```c
enum nvmf_capsule_command {
        nvme_fabrics_type_property_set  = 0x00,
        nvme_fabrics_type_connect       = 0x01,
        nvme_fabrics_type_property_get  = 0x04,
        nvme_fabrics_type_auth_send     = 0x05,
        nvme_fabrics_type_auth_receive  = 0x06,
};
```

**没有 disconnect。** 但我们有一套完整的东西围着它建起来了：

| 位置 | 曾经的样子 |
|---|---|
| `nvmeof_wire.h` | `NVMEOF_FCTYPE_DISCONNECT = 0x08` + 一个 64 字节的 `nvmeof_fabrics_disconnect` 布局 |
| F5/F6 target | `else if (fctype == DISCONNECT) { … st.disconnected = true; }` —— "host 要走了" |
| F5/F6 host | 发 0x08 并**断言 target 接受了它**，然后把后续行为建立在"关联已经消失"上 |
| 注释 | 写着"这些值是从 header 里取的，不是编的" |

对着真 target，这条命令只会得到 `Invalid Opcode`；**之所以一直没暴露，就是因为我们的
target 愿意接受它**。host 真正该做的是 Linux host 做的事：
**Property Set CC = 0（关控制器）**，然后 RDMA 连接消失。

已改：删掉常量与结构体，host 改成写 CC.EN=0，target 改成按 nvmet 回答
`Invalid Opcode`，并**新增一条断言**：0x08 必须被拒。
（顺带一个测试顺序的教训：这条探针必须在 CC.EN=0 **之前**发 —— 一个刚被告知
CC.EN=0 的 target 有权停止服务，探针就会打在一个已经不存在的 target 上。
我第一次就是这么写错的，断言如实报了失败。）

#### (2) 属性读写要声明寄存器宽度：CAP 必须用 8 字节读

```c
if (req->cmd->prop_get.attrib & 1) {          // 8 字节读
        switch (offset) { case NVME_REG_CAP: … default: INVALID_FIELD | DNR }
} else {                                      // 4 字节读
        switch (offset) { case VS: case CC: case CSTS: case CRTO: … default: INVALID_FIELD | DNR }
}
```

我们的 host 读 CAP 时 `attrib = 0` → Linux 会以 `Invalid Field` 拒绝，
**而这是 host 连上之后发的第一条命令**。我们的 target 完全不看 attrib 字段，
所以两端一致、世界不同意——又是同一类。F6 的 target 其实**早就会检查**
（它按宽度分表），所以这个 bug 只藏在没有检查的那几个文件里。

已改：F5 host 读 CAP 时写 `attrib = 8 字节`；F5 target 按 nvmet 的规则检查宽度
（8 字节只准 CAP，CAP 必须 8 字节）；wire 自检钉住 `attrib` 在偏移 40 以及两个宽度值。

#### (3) 检查器补上了这一类：家族里"多出来的成员"也要报

之前的配对表只能检查**我想起来列进去**的常量；一个**没有对应物**的常量恰恰是它跳过的
——所以 0x08 活了整个项目的生命周期。现在 `xref_constants.py` 增加了**家族检查**：

* `NVMEOF_FCTYPE_*` 必须都能在参考里找到 `nvme_fabrics_type_*`；
* `NVMEOF_PROP_*` 必须都能找到 `NVME_REG_*`；
* 确实属于我们自己的成员（`attrib` 宽度掩码与两个宽度值）必须**逐条写明理由**，
  否则一样判 FAIL。

**并且我验证了这个检查真的会响**：临时把 `NVMEOF_FCTYPE_TELEPORT = 0x09` 加回去，
`run_xref.ps1` 立刻报

```
[INVENTED] NVMEOF_FCTYPE_TELEPORT  has no nvme_fabrics_type_teleport in the reference
RESULT: FAIL - 1 wire constant(s) exist only in this project
```

删掉即恢复 PASS。**一个没验证过的检查等于没有检查**——这条比检查本身更值得记。

#### (4) 这一轮之后的状态

| | |
|---|---|
| 凭记忆编的线上常量 | 至今 **5 个**（§8.29 的 4 个 + 本轮的 fctype 0x08），全部已修，且现在有家族检查兜底 |
| 全长回归 | `run_all.ps1`：**8/8 通过，84 秒** |
| 第 5、7 项 | **仍然没有证据** —— 需要的是一台本地 Linux，不是更多代码 |

### 8.38 下一阶段

1. **F5 —— Linux `nvmet` 互操作**，唯一真正的验收，需要 Linux（见 §8.18）。
   §6 的第 1–4、6 项**已经全部做完**，剩下的两项（5 和 7）都只差一个独立对端。
   **上场前的工程准备已全部做完**（§8.42：接法、桥接脚本、MTU、对端一键脚本、全量回归）；
   缺的只是那台本地 Linux 机器通电、插上网线。
2. ~~把 §8.37(3) 表格里"未改"的那几处顺序换过来~~ —— **已做完**（§8.39(1)）。
3. 复现命令一览：**`run_all.ps1`**（一次跑完 8 套并给结论表 —— 实测 86 秒，
   8/8 通过）；顺序上 `run_xref.ps1` 最先跑，它失败时其余结果都不可信。
   入口文档见 `README.md`。

可选（按"先实现命令、再设位"的顺序）：`Async Event` 的延迟完成机制，
以及把 `oaes` 打开 —— 但注意 §8.33：**打开它之前必须先实现 Async Event 命令本身。**
（§8.40 顺带把这条也测实了：Linux host 只在 `oaes` 非零时才发 AEN，见
`nvme_enable_aen()` 的 `if (!supported_aens) return;`，所以我们保持 `oaes=0` 是安全的，
而不是"大概没事"。）

### 8.45 ⛔ 桥接会关掉两块网卡的 TCP/IP（这是设计行为），但我的回滚脚本"报成功却什么都没做"

这一节不是关于 NVMe-oF 的，是关于**上一节那套链路脚本把我自己的机器弄坏了**，
以及为什么"看起来修好了"和"真的修好了"在这里是两件事。

#### (1) 症状：断网检查器说"网络适配器没有启用 TCP/IP 服务"

实测（不是推断）：

```
桥存在时：  以太网 20  TCP/IPv4=False   IsBridged=Yes
            以太网 23  TCP/IPv4=False   IsBridged=Yes
            网桥       TCP/IPv4=True    ← 地址 192.168.1.2、DNS 192.168.1.1 都在它身上

销毁桥后：  以太网 20  TCP/IPv4=True
            以太网 23  TCP/IPv4=True
```

**这是 Windows 网桥的设计**：成员网卡不再持有自己的协议栈，TCP/IP 被移到桥接口上。
所以那条告警在"桥存在"期间**必然出现**，而且必然只出现在这两块网卡上——
段本身是好的（DNS、网关、对端笔记本全程可达）。**不需要"修复"，销毁桥就自动恢复。**

#### (2) 真正的缺陷：回滚是假的，而且它两次都在说谎

```powershell
netsh bridge destroy 2>&1 | Out-Null      # ← 没有参数
```

`netsh bridge destroy` **没有参数时只打印用法，返回码仍然是 0**。所以这一行
什么都没销毁，而脚本继续往下跑。然后：

```powershell
New-NetIPAddress -InterfaceAlias $LanAdapter ... -ErrorAction SilentlyContinue
Ok "  restored $LanIp on '$LanAdapter'"   # ← 无论成功与否都打印这句
```

桥成员网卡**不能持有 IP**，所以 `New-NetIPAddress` 必然失败——被
`-ErrorAction SilentlyContinue` 吞掉，紧接着脚本宣布"已恢复 192.168.1.2"。
**假成功 + 被吞掉的错误**，这正是本项目 §8.6 那条老毛病的翻版，只不过这次
受害者是宿主机的网络配置。

正确写法（现在脚本里的）：

```powershell
$guid = '{' + $br.InterfaceGuid.ToString().ToUpper() + '}'   # 桥适配器自己的 GUID 就是它
netsh bridge destroy $guid
# 等桥真的消失，再恢复地址（顺序反了就等于没恢复）
# 恢复完**回读地址确认存在**，不存在就报 Bad，而不是宣布成功
```

#### (3) 顺带挖出的第三个坑：状态文件是乱码，回滚本来就无从谈起

`interop_link_state.json` 里的网卡名是 `浠ュお缃?24` 这种乱码，每个 IP 列表都是空的、
每个 MTU 都是 null。原因：**写入端是 PowerShell 7**（`Set-Content -Encoding UTF8`
在 7 里**不写 BOM**），**读取端是 PowerShell 5.1**（无 BOM 时按 ANSI 读），
于是脚本里的 `'以太网 20'` 变成乱码、`New-NetIPAddress -InterfaceAlias <乱码>` 全部静默失败。

也就是说：**就算销毁桥成功了，那份状态也恢复不出任何东西。**
现在写入统一用 `[System.IO.File]::WriteAllText(..., UTF8Encoding($true))`（显式 BOM），
读取统一带 `-Encoding UTF8`，`Save-State` 遇到旧文件先备份而不是直接返回，
并且**发现某块网卡没有地址可记录时会 Warn**——因为"空列表"正是坏回滚的样子。

#### (4) 为什么这个"窗口"躲不掉

RoCEv2 不可路由，对端必须同网段。本机两个 CX3 口之间是 QSFP 直连线，
笔记本的 RJ45 在 LAN 交换机上——**把这两段并成一个 L2 的唯一办法就是桥**。
所以互操作测试期间那条告警会出现，测完 `-Action Down` 立刻消失（已实测）。
把这条写进脚本头部注释，就是为了下次看到它的人不要再去"修"它。

### 8.46 ✅ 第一次真正的互操作：和 Linux nvmet / nvme-cli 双向跑通，期间抓到 11 个缺陷

**结果（本轮最终证据）**

| 方向 | 对端 | 结果 |
|---|---|---|
| A：我们的 initiator → Linux `nvmet` | rxe（软件 RoCE，笔记本） | **`initiator failures: 0`**：CM 连接、Connect(qid 0)、CAP/CC/CSTS/VS、Identify（真数据 `mn='Linux' fr='6.18.41-'`）、命名空间发现、Set Features、Keep Alive、Async Event 上限、I/O 队列 Connect、WRITE+READ 逐字节一致（4096 B）、FLUSH、越界拒绝、**32 条在飞的写/读流水线各 32/32 成功**、fctype 0x08 被拒、CC.EN=0 退出 |
| B：Linux `nvme-cli` → 我们的 target | nvme-cli 3.1 + rxe | `nvme connect` **rc=0**，`nvme list` 出现 **`/dev/nvme2n1  NDVMEOF0000000000001  NetworkDirect NVMe-oF prototype  33.55 MB`**；128 块（65536 B）写入 → `nvme flush` → 读回 → **`cmp` 逐字节相同**；33 次 Keep Alive 全部应答，控制器不再进 error recovery |

**这一轮修掉的 11 个缺陷，全部是"我们两端自己测永远测不出来"的类型。**
修完 `run_all.ps1` 仍然 **8/8**——也就是说这些 bug 从来不曾被自检覆盖过，
这正是 §6 里把第 5、7 项单列的原因。

#### (1) SQE 的 byte 1 必须是 `0x40`（PSDT = METABUF）

`nvmet_req_init` 在**解析任何命令之前**先查这一字节（`target/core.c`）：

```c
	if (unlikely((flags & NVME_CMD_SGL_ALL) != NVME_CMD_SGL_METABUF)) {
		req->error_loc = offsetof(struct nvme_common_command, flags);
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto fail;
	}
```

我们一直发 `0x00`。后果不是"某个字段不对"，而是**整条初始化序列全被拒**：
CAP 读回来是 0（错误路径上 result 就是 0）、`CC.EN=1` 被拒、Connect 得到
`sct=0 sc=0x02 DNR`。我们自己的 target 不查这个字节，所以自检全绿。

#### (2) 顺序：Connect 必须是 admin 队列上的**第一条**命令

fabrics 控制器是 **Connect 创建出来的**；在那之前 admin 队列没有 controller，
nvmet 把该队列上的一切都送进 connect 解析器并回 Invalid Opcode。
我们原来按 PCIe 的顺序（CAP → CC.EN → CSTS → Connect），这在 fabrics 上不可能工作。
正确顺序（照 `nvme_enable_ctrl`）：
`Connect(qid 0) → GET CAP(attrib=1) → SET CC → GET CAP → [GET CRTO] → SET CC|EN → GET CSTS 轮询 → GET VS → Identify …`

#### (3) 没有数据的命令**也要**带一个 keyed dptr（`0x40`）

nvmet 的 RDMA transport 对**每一条**命令都 switch 那个描述符类型字节：

```
nvmet_rdma: invalid SGL subtype: 0x0        ← 连打六次
nvmet: got cmd 6 while CC.EN == 0 on qid = 0
```

我们给 Property Get/Set 留了全 0 的 dptr。Linux 主机对"无数据"的写法是
**长度 0 的 keyed SGL**（`nvme_rdma_set_sg_null`），这就是 `nvmeof_sgl_set_null()`。

#### (4) 状态字的 bit 0 不是"成功"的一部分

Linux 读完成项是 `le16_to_cpu(status) >> 1`，**从不看 bit 0**。nvmet 因此把一个
完全成功的 Connect 回成 bit0=0，而我们的 `statusOk()` 要求它置位——于是
`Connect qid 0: sct=0 sc=0x00` + `[FAIL]`。自己的 target 恰好置位，所以自检发现不了。

#### (5) CRTO 只在 CAP 声明 CRMS.CRWMS **且** CRMS.CRIMS 时才能读

nvmet 的 CAP 只有 bit43（CRWMS），属性读也不实现 CRTO。照抄 Linux 的判断就不会踩。

#### (6) ⚠️ 这个驱动在线上把 **rkey 按字节反转**（RDMA READ 两个方向都是）

**实测，不是推断**：

- 我们的 initiator 打 nvmet：目标端读我们 1024 B connect data 报
  `remote access error (10)`；把**我们公布的 token 字节反转**后立刻成功；
- 我们的 target 读 Linux 主机的 connect data：`ND_ACCESS_VIOLATION`；
  把**传给 `Read()` 的 key 字节反转**后立刻成功（`ND_SUCCESS bytes=1024`，
  读回来的 hostnqn/subsysnqn 正确）；
- 同一块缓冲、同一个 key、同一个长度，**CX3 对 CX3 却一直正常**——
  对称的错误对"两端共用同一驱动的自己人"是隐形的。

修法在 `nvmeof_rdma.h` 的 `ndWireKey()`：公布时用 `ndWireKey(token)`，
消费时把对端 SGL 里的 key 也过一遍同一个函数。地址不受影响（同批实验证明
地址是原样上线的）。

#### (7) ⚠️ MTU 必须和对端一致，否则**大块单向写会静默失败**

RoCE Max Frame 2048（IP MTU 1500）时：1024 B 的 inbound RDMA Write 成功，
**4096 B 的 Identify 数据永远不到**——没有响应、缓冲全 0、对端也不报错，
只有连接随后死掉。把 RoCE Max Frame 降到 **1024**（与 rxe 的 IB MTU 一致）后
Identify 立刻通过。`interop_link.ps1 -Action LowMtu` 本来就是对的，
是这一轮之前从未跑到需要它的地方。

#### (8) target 侧：I/O 队列的 Accept **也要带 CM 回复私有数据**

我们原来对 I/O 队列 `Accept(..., nullptr, 0)`。Linux 主机随后直接拆连接
（`nvme connect` 报 `could not add new controller`，我们的日志里
`[io] slot 0: ND_CANCELED`）。补上同样的 32 字节 `crqsize` 回复即可。

#### (9) target 侧：只能给得出几个 I/O 队列，就说几个

我们回 "8"，Linux 主机就老老实实建 8 条 I/O 队列，第二条就失败。
现在 `kMaxIoQueues = 1`：**回给主机的数字是契约，不能承诺自己没有的队列对。**

#### (10) target 侧：KATO 来自 Connect，而 KATO=0 时 Keep Alive 必须回"无效"

Linux 主机的 keep-alive 定时器**写在 Connect 命令里**（毫秒；`ctrl->kato * 1000`）。
我们原来只把它打印出来，`st.katoMs` 一直是 0 —— 于是每条 Keep Alive 都按规范回
`KA_TIMEOUT_INVALID`，主机判定 keep-alive 失败 → **error recovery → I/O 队列冻结**。
症状是"28 条读里 13 条永远没有完成、缓冲没动、主机挂在那儿"，
真正的原因在几百行之前的 Connect 解析里。

#### (11) target 侧：Get Log Page 的长度是 NUMDL/NUMDU，不是 CDW10 的低字节

```
numd = (CDW10 >> 16) | (CDW11 << 16);   len = (numd + 1) * 4
```
我们原来用 `bytes[40..43]`，把 **LID** 也当成了长度的一部分：Linux 主机读
512 B 的 error log 被算成 33292300 B，截断到 4096 后**往主机 512 B 的缓冲里写了
4096 B** —— 越界 RDMA Write，回来的是 `ND_ACCESS_VIOLATION`，队列对随之死亡。
现在照 nvmet 的 `nvmet_check_transfer_len` 做：**SGL 的长度就是传输长度，
命令要的长度必须与它相等**，不等就回 `SGL_INVALID_DATA (0x0f)`。

#### (12) ⚠️ 差点毁掉一台真机：脚本按"第一个 nvme 设备"写数据

`f5_session.ps1` 原来在方向 B 里直接读写 **`/dev/nvme0n1`**。在那台笔记本上，
`/dev/nvme0n1` 是一块 **512 GB 的系统盘**（真正的分区表和数据），
而我们自己的设备是 `nvme2n1`。只要有人照着脚本跑一次，
就会往用户的系统盘里写 256 KiB 随机数据 —— 这不是"参数没填对"，
是**一个默认会指向系统盘的路径**。

现在改成：**按序列号找设备**（`nvme list` 里 `NDVMEOF` 开头的那个），
找不到就**报错退出并且不碰任何块设备**，并且写测试用 `nvme write/read` 而不是 `dd`。
真正的教训是：脚本里任何**写**路径都不能用"设备序号"来定位 —— 序号会随接进来的
设备数量变化，而序列号不会。

#### 这一轮最该记住的一句

上面 11 条里，有 **7 条**是"我们自己的 target/host 对某个字段的理解与规范不一致"，
而它们此前一路通过 8/8 自检。**自检证明的是内部一致性，不是正确性**；
真正的验收只能是和一个不共享我们错误的实现对接。

### 8.47 ✅ 把"手工互操作"变成一条命令，然后照着它跑了一遍——又抓出 4 个缺陷

§8.46 的结论是靠一串手工命令得到的。这一节做的是把那些命令固化成
`f5_session.ps1`（链路 → 对端 → 方向 A → 方向 B → 一份报告文件），
**然后真的用这条命令跑完整场**。结果：双向都过，但也又一次证明
"写下来的步骤"和"能跑通的步骤"是两件事 —— 跑的过程里又抓出 4 个缺陷。

#### 达标证据（本轮，`src/f5_session_20261001-*.txt`）

| 方向 | 判据 | 实测 |
|---|---|---|
| A 我们的 host → Linux `nvmet` | 脚本自判 | **PASS（33 项检查通过，`initiator failures: 0`）**，本轮跑了两次都是这个结果 |
| B Linux `nvme-cli` → 我们的 target | 对端自己比 | `CMP: identical (65536 bytes written, flushed, read back)`；target summary `connects=1 ioQueuesGranted=1 ioCommands=30 flushes=1 written=65536 read=405504 keepAlives=1 katoMs=5000 unknownAdmin=0 unknownIo=0`，`target failures: 0` |

`read=405504` 比 65536 大是**正常的**：真主机会读命名空间去探测几何信息。
所以断言写成 `written == 65536`（我们写的就是这些）、`read >= 65536`。
第一版写成 `read == 65536`，会误报。

#### (13) ⛔ "桥在的时候自测 F1/F7 报错"的真因：方向 A 没有指定本地 IP

`run_f5.ps1` 的 `-clientLocalIp` 默认是 `192.168.100.3`，而那正是**被桥接的口 A**；
它的 TCP/IP 已经交给桥，打开适配器得到 `NdOpenAdapter 0xC0000141`。
`f5_session.ps1` 原来没有传这个参数，于是**在桥接窗口里跑方向 A 必然失败**。
之前把这个现象记成"环境的副作用"，其实是脚本漏传参数 —— 传
`-clientLocalIp 192.168.100.2`（留给 RoCE 的口 B）之后一次就过。

#### (14) ⛔ 探测对端地址的时机反了：它一直靠上一次会话的残留才"能跑"

脚本原来在第 1 步之后、第 2 步**之前**去 ping `192.168.100.5`，
可这个地址是第 2 步的 `f5_linux_up.sh` 加上去的。
它能一直通过，只因为上一次会话把地址留在了笔记本的网卡上 ——
**换台新机器或重启一次，就会假报"两端不在同一二层"**。现在顺序是
链路 → 对端 → 再探测。

#### (15) ⛔ 远程命令串不能穿过 PowerShell → ssh.exe → bash：引号会丢

方向 B 原来是一段 here-string 拼出来的 ssh 命令。实测两种坏法：

* `awk '{print \$1}'`：PowerShell 的双引号字符串里反斜杠是**字面量**，
  对端收到 `{print \$1}`，awk 报 `backslash not last character on line`；
* `echo "using $DEV (serial NDVMEOF...)"`：**双引号在途中被丢掉**，
  对端于是收到裸的括号，`bash: -c: line 12: syntax error near unexpected token '('`
  —— 这一次正好死在序列中间，**在对端留下一个活着的控制器**
  （之后它一直发 Keep Alive，把下一次连接也搅了）。

现在序列是 `linux/f5_dirb_check.sh`，由脚本 scp 到对端家目录再 `sh` 执行：
**文件没有引号可丢**。另外每条 `nvme` 都要 root（否则
`Failed to open /dev/nvme-fabrics: Permission denied`），统一用 `sudo -n`，
免得卡在没人能回答的密码提示上。

> 附带一条环境事实：对端的 `~/nvmeof/linux` 是 **root 所有**（当初用 sudo 建的），
> 普通用户写不进去，所以上传目标是家目录；要更新那几个 `.sh` 本身，
> 需要一次带密码的 `sudo install`（`/etc/sudoers.d/` 的白名单只列了三个脚本的绝对路径）。

#### (16) ⛔ 方向 B 脚本自己的三个坑：root 拥有的临时文件、没有清理陷阱、残留控制器

这三个都是"跑第二次才暴露"的：

1. `sudo nvme read -d /tmp/f5back` 会把文件建成 **root 所有**，
   下一次 `rm -f /tmp/f5back` 就是 `Operation not permitted`，
   在 `set -e` 下**当场中止序列中间**。现在数据文件放在 `mktemp -d` 的私有目录里
   （目录归自己所有，里面的 root 文件照样能删）。
2. 中止时没有任何清理 → 对端留着一个活控制器。现在有 `trap ... EXIT INT TERM`，
   **不管怎么退出都断开**。
3. 上一次异常退出留下的控制器**会自己重连**。这时再 `nvme connect` 同一个 NQN，
   内核回 `could not add new controller: failed to write to nvme-fabrics device`，
   而真正握手的是**旧控制器的重连**。所以现在先断开、**等到设备真的消失**再连，
   并且 connect 失败重试 3 次。

#### 判据本身就是缺陷来源：没有 `[FAIL]` 不等于通过

第一版方向 A 的判据是"日志里没有 `[FAIL]`"。问题是
**一条命令都没发出去就崩掉的日志里同样没有 `[FAIL]`**。
现在要求同时满足：`[FAIL]` 为 0、出现 `initiator failures: 0`、`[PASS]` ≥ 10，
否则报 **INCONCLUSIVE**。方向 B 同理：光看退出码不够，必须同时有对端的
`CMP: identical` 和本机 target 的计数器（`written/read/keepAlives/unknown*`）。

#### 收工后的机器状态（拆桥 + 回读验证，§8.45 的收尾）

`f5_session.ps1 -Down` + `-Action RestoreMtu` 之后逐项回读：

```
桥:            不存在（netsh bridge show adapter 里所有 IsBridged = No）
TCP/IPv4:      以太网 2/20/23/24/25 全部 Enabled=True   ← 用户最初报的那个问题
地址:          以太网 20 192.168.1.2/24, 以太网 23 192.168.100.3/24, 以太网 24 192.168.100.2/24
DNS:           以太网 20 → 192.168.1.1；以太网 25(RNDIS) → 10.246.250.157
默认路由:      只有以太网 25 → 10.246.250.157
MTU/Jumbo/RoCE: 23/24 都是 NlMtu 4074、Jumbo 4088、RoCE Max Frame 2048
外部:          路由器/笔记本 SSH/DNS 解析/8.8.8.8 全部可达
```

> ⚠️ **上面这一行当时写错了，必须更正**：那几个"外网可达"的检查走的是当时**唯一**的
> IPv4 默认路由 —— 用户手机的 USB 共享（`以太网 25` RNDIS，10.246.250.74）。
> 有线口 `以太网 20` **根本没有 IPv4 默认路由**，也就是说"机器能上网"成立，
> "有线网卡能上网"不成立。用户随后指出"网卡还没好，之前一直用手机给你上网"，
> 才把这件事翻出来，见 §8.48。教训与 §8.46 那条一样：
> **测出"能用"不等于测的是你以为的那条路**，凡是关于路径的结论都必须写明经由哪个接口。

### 8.48 ⛔ "网卡坏了"的真相：有线口只有 IPv6 出去的路，IPv4 少了网关

现象是"这台机器上不了网，一直靠手机 USB 共享顶着"，而 Windows 自己的网络检查器
又报过"该适配器没有启用 TCP/IP 服务"（那是 §8.45 的桥接副作用，已解决）。
真实情况用四条只读命令就能分开：

| 检查 | 实测 | 结论 |
|---|---|---|
| `Get-NetAdapter` 有线口 | `Up / 1 Gbps / Connected` | 网卡、网线没问题 |
| `Resolve-DnsName www.microsoft.com -Server 192.168.1.1` | **返回了记录** | 路由器有上游，DNS 也在工作 |
| `Get-NetRoute 0.0.0.0/0`（按接口看） | 以太网 20 **0 条**，只有以太网 25 | **IPv4 出不去的就是这一条** |
| `ping -S 192.168.1.2 8.8.8.8` | `PING: transmit failed. General failure.` | 没有路由时的典型报错 |
| `ping -6 2400:3200::1` | 12 ms，0% 丢包 | **IPv6 一直是通的**（RA 给的默认路由） |

`以太网 20` 是手工配置：地址 192.168.1.2/24、DNS 192.168.1.1、**没写网关**、DHCP 关闭。
所以 IPv4 没有默认路由，而 IPv6 靠路由器 RA 自动拿到地址和默认路由 ——
**同一个网卡"一半能上网"**，这也是为什么它看起来时好时坏、容易被误判成"网卡坏了"。

修法（只加一条备份路由，不动手机那条）：

```powershell
New-NetRoute -InterfaceAlias '以太网 20' -DestinationPrefix '0.0.0.0/0' `
             -NextHop 192.168.1.1 -RouteMetric 50      # 手机的 metric 是 0，仍然优先
```

验证（**源绑定**，证明走的是有线口而不是手机）：

```
ping -S 192.168.1.2 223.5.5.5                     -> 0% loss, 19 ms
curl --interface 192.168.1.2 https://www.microsoft.com  -> status=200
Find-NetRoute 8.8.8.8                             -> 仍然选 以太网 25（首选未变）
Resolve-DnsName www.baidu.com                     -> 39.156.70.239
```

**为什么用备份而不是直接改成首选**：手机那条是当下唯一确定能用的路，
换成首选等于在用户正在用的东西上做实验。metric 50 的效果是
**手机在的时候照旧，手机一拔自动接管** —— 用户想验证，拔掉手机再打开网页即可。

> 一句诚实的边界：我**无法证明**这条网关是被我早先的桥接实验弄丢的。
> 我最早的记录（`interop_link_state.json`，10:26 保存）里就已经没有这条路由了，
> 而"最早的记录"只和我的第一次会话一样老 —— 如果它在那之前就丢了，
> 那次回滚只会把"已经坏掉的状态"当成原始状态恢复回来。
> 无论原因是什么，结论是：**回滚脚本记录的"原始状态"必须尽早建立，
> 而且回滚后要按接口逐项回读，不能只看"全世界是否可达"。**

> 另一条**与本次改动无关**、免得下次误判的环境记录：
> 这台机器上 `curl https://www.example.com` 报
> `schannel: SEC_E_UNTRUSTED_ROOT`，而同一台机器
> `curl -k` 拿到 302、`https://www.microsoft.com` 能完成 TLS（返回 403）。
> 也就是说 **DNS、路由、TLS 本身都是好的，只是那一条证书链不被信任**；
> 根证书库有 97 条（含 DigiCert Global Root CA、ISRG Root X1）。
> 本轮没有碰过任何证书/根存储/WinHTTP 设置。

### 8.49 ✅ 多 I/O 队列：两端都做，并用真 Linux 主机验证（8 条队列、6 条真的被用到）

用户的目标是"与 Linux 全部兼容"。差距清单里排第一的是**多 I/O 队列**：原来的 target
只服务 1 条 I/O 队列，`Set Features Number of Queues` 就只敢回 1 —— 一处"能跑但不像
真控制器"的地方。这一节把它做到两端，并给出对端证据。

#### 实现

| 位置 | 改动 |
|---|---|
| target | `kMaxIoQueues = 8`：真的创建 8 个 queue pair / 连接器；每条 I/O 队列**独占**一套 capsule ring + response ring（`kIoCapOff + q*kIoCapStride`），上下文 id 编码 `(队列, 槽位)`，否则两条队列会互相覆盖 capsule |
| target | 每条队列一套 in-flight 槽位表 `st.io[q][slot]`；主循环逐个 accept（一次只挂一个 `GetConnectionRequest`，靠 listener backlog 兜住突发）并逐个轮询 |
| target | 每条 I/O 队列的 RDMA-CM 私有数据**逐条校验**，包括 `cntlid`：不匹配回 `NVME_RDMA_CM_INVALID_CNTLID (0x09)`；I/O Connect 命令里的 qid 为 0 或超过已授予数则回 `CONNECT_INVALID_PARAM (0x82) + DNR` |
| target | 汇总行新增 `ioQueuesConnected=` 和 `target io per queue: q1=… q8=…` |
| initiator | 按 `-queues N` 申请，然后**按对端授予数**建队列：每条队列各自的 Bind/Connect/CompleteConnect + 各自的 fabrics Connect（qid=i+1），I/O 不再只走第一条 |
| 新测试 | "每条队列同时各跑一条命令" + "并发读各回各的数据"：8 条队列各自不同的数据图案，队列之间串了就会被数据校验抓住 |

#### 对端证据（2026-10-01，`src/f5_session_20261001-131432.txt`）

| 方向 | 实测 |
|---|---|
| A 我们的 host → Linux `nvmet` | `Set Features: Number of Queues - asked for 4, granted 128`；**4/4 条 I/O 队列各自建连**；`every I/O queue runs its own command at the same time - 4 of 4 queues answered with success, 0 queues with wrong data`；`a parallel read across every queue returns each queue its own data`；36 项检查、`initiator failures: 0` |
| B Linux `nvme-cli` → 我们的 target | 主机要 8 条（`--nr-io-queues=8`），日志逐条 `I/O queue 1..8 connected`，每条私有数据 `qid=N cntlid=1` 都通过校验；`CMP: identical` 以及 `CMP: identical for 4 concurrent reads (multi-queue)`；`ioCommands=34 written=65536 unknownAdmin=0 unknownIo=0`、`target failures: 0` |
| **队列分布** | `target io per queue: q1=1 q2=1 q3=0 q4=2 q5=11 q6=16 q7=0 q8=4` —— **真主机把命令分散到 6 条队列上**。这一条才是"多队列真的在用"的证据：只连不用的主机，这个分布会是 `q1=N 其余全 0` |

自检：`run_all.ps1` **8/8、99 秒**；F5 自测默认改成 **4 条队列**（`run_f5.ps1 -queues`，
默认 4），所以每次自检都会走多队列路径。

#### 这一轮我又犯的三个错（都记下来）

**(1) ⛔ 我"发明"了一个 cntlid 校验：Connect 命令里根本没有这个字段。**
给 I/O 队列加严格校验时，我在 Connect 命令的偏移 20 读了 `cntlid`——那是保留字节，
读出来是 0。结果**我们自己的 initiator 被自己的 target 拒了**（`fabrics Connect qid 1`
FAIL）。翻 `ref/linux_nvme.h` 才确认：`struct nvmf_connect_command` 没有 cntlid，
它在 **connect data 的偏移 16**，只有 admin 队列会传这份数据；I/O 队列的控制器身份
是走 **RDMA-CM 私有数据**的（`nvme_rdma_connect()` 用 `ctrl->cntlid` 填）。
教训和 §8.44 一模一样：**校验也要先读参考实现，不能凭印象写**。

**(2) ⛔ 2 MiB 的 Identify 落地缓冲一直压在 namespace 里面。**
重排缓冲区时才发现：namespace 从约 1 MiB 开始，而给对端 RDMA Write 用的
`kOutOff = 2 MiB` 正好落在 namespace 内部 —— 主机写第 ~2176..2303 块就会覆盖
"刚写进去的 Identify 数据"，反过来 Identify 也会毁掉那些块。之前没炸只是因为
Linux 主机读的块恰好不在那一段。现在 `kOutOff` 从 namespace 末尾向上取整到 2 MiB
（对齐要求是实测出来的，不能丢），并加了两条 `static_assert` 钉住
"不重叠"和"保持 2 MiB 对齐"。

**(3) ⛔ 回滚把"机器真实状态"覆盖成"我自己的产物"，于是没恢复默认路由。**
`-Action Up` 会把 LAN 的默认路由搬到桥上；而 `f5_session.ps1` 紧接着又调用
`-Action LowMtu`，**它也调用 `Save-State`** —— 于是状态文件里的 `Routes` 被写成了
"网桥 → 192.168.1.1"，`-Action Down` 按"找 LAN 网卡的记录"去恢复，什么也找不到，
只打印"left alone - none was recorded for this NIC"。
这一次没断网，**纯粹是因为我之前手工加过一条持久化路由**，桥一消失 Windows 就把它
重新应用了。修了两处：`Save-State` 跳过桥上的默认路由（不把自己的产物当原始状态）；
`-Down` 允许把"记录为桥"的那条恢复回 LAN 网卡，并且**恢复后回读确认它真的在**，
不在就报 Bad。

#### 一条工具链陷阱：编辑会把 UTF-8 BOM 吃掉

`interop_link.ps1` 里有中文接口名（`以太网 20`），必须带 BOM，否则 PS 5.1 按 ANSI 读，
名字变成 `浠ュお缃?20`，脚本就找不到网卡了。这轮编辑之后它就变成了无 BOM 版本，
`-Action Check` 立刻显示出乱码——**这是 §8.45 那个坑的第二次出现，只是原因不同**：
上次是 `Set-Content -Encoding UTF8` 不写 BOM，这次是编辑工具重写文件时把 BOM 丢了。
改完含非 ASCII 的 `.ps1` 后要检查前三个字节是不是 `EF BB BF`，顺手一条命令就能补回。

#### 还没做到的（本阶段剩余）

1. **discovery controller**：`nvme discover` 现在连不上我们（没有 discovery 子系统）。
2. **DH-HMAC-CHAP 认证**：nvmet 支持、nvme-cli 支持，我们两端都没有。
3. **重连/错误恢复语义**：KATO 超时、QP 重连、controller reset 之后的行为还没有专门的验证。
4. admin 面剩余覆盖（更多 Identify CNS / Async Event 类型 / Set Features）。

### 8.50 ✅ discovery controller：`nvme discover` 现在能列出我们，我们的 host 也能问 Linux

`nvme discover` 是真实部署里"先找目标、再连"的第一步，也是与 Linux 兼容清单的第二项。
它看起来像一个小功能，实际上要求三件事同时成立：

1. **接受一个"众所周知的"子系统名**：`nqn.2014-08.org.nvmexpress.discovery`
   （`ref/linux_nvme.h:28`），并把这条连接当成 **discovery controller** 而不是 I/O
   controller：Identify CNS 1 要报 `cntrltype = 2`、subnqn = discovery NQN、`nn = 0`、
   `ioccsz = iorcsz = 0`（没有 I/O 队列）。
2. **回答 Get Log Page LID 0x70**：1024 字节头（genctr / numrec / recfmt）+ 每条 1024 字节的
   记录，并且**支持 log page offset 分块读**（内核是一块一块读的，忽略 offset 会把第一块
   反复发回去）。
3. **按控制器数量服务**：`nvme discover` 和 `nvme connect` 是**两个控制器**，
   各有自己的 admin queue pair，discovery 那个读完就走。

#### 实现（两端）

| 位置 | 改动 |
|---|---|
| `nvmeof_wire.h` | discovery 常量与两个构造器（`nvmeof_disc_hdr_set` / `nvmeof_disc_entry_set`），每个数字都注明 `ref/linux_nvme.h` 行号 |
| target | admin Connect 里读 connect data 的 subsysnqn：是 discovery NQN 就标记为 discovery 控制器；**其它不认识的 NQN 回 `CONNECT_INVALID_PARAM`**（nvmet 的行为）；Identify 按控制器类型填；Get Log Page LID 0x70 返回两条记录并支持 offset；**discovery 控制器拒绝非 discovery 的日志页** |
| target | `-serve N`：一次运行服务 N 个控制器（默认 1，保持 F7"主机消失就退出"的断言；0 = 不限） |
| initiator | `-discover`：连 discovery NQN → Identify（校验 ctrltype/subnqn）→ 读 LID 0x70 头 + 全部记录（必要时用 offset 再读）→ 打印每条记录 |

#### 证据：拿 Linux 当参考实现来读，而不是凭印象写

| 谁 | 实测 |
|---|---|
| 我们的 host → **Linux nvmet** | Connect 到 discovery NQN 成功（cntlid=1）；Identify `ctrltype=2 subnqn='nqn.2014-08.org.nvmexpress.discovery' nn=0`；`genctr=2 numrec=2 recfmt=0`；记录 #0 `subtype=3 cntlid=65535 subnqn=<discovery NQN>`、#1 `subtype=2 cntlid=65535 subnqn=nqn.2024-01.local.rdma:linux-nvmet`；**在 discovery 控制器上 LID 1/5 被 nvmet 拒绝（sct=0 sc=0x02）** |
| **Linux `nvme discover` → 我们的 target** | 对端输出 `DISC: the discovery log names nqn.2024-01.local.rdma:windows-nd`；本机 target 日志：controller 1 = `discovery=1 discLogReads=3`（内核分三次读完），controller 2 = I/O 控制器 `ioQueuesGranted=8 ioQueuesConnected=8 ioCommands=34 written=65536`；汇总 `controllers=2 discLogReads=3 unknownAdmin=0 unknownIo=0`，`target failures: 0` |
| 自测（两端都是我们） | discovery 全绿：`ctrltype=2`、`numrec=2 recfmt=0`、两条记录（subtype 3 与 2）、`discLogReads=1`；随后同一个 target 进程再服务一个 4 队列的 I/O 控制器，`initiator failures: 0 / target failures: 0` |

**记录里的 subtype 是从对端抄来的**：第一版我们自己那条写的是 subtype 1
（"referral to ANOTHER discovery controller"，是另一种主张），而 nvmet 实测用的是
**subtype 3**（NVME_NQN_CURR，"我就是当前这个 discovery 子系统"）并且把 discovery NQN
填在 subnqn 里。这类"两边都能跑、只有一方合乎规范"的地方，就是本项目一直在踩的坑。

#### 这一轮我犯的四个错

1. **⛔ 参数解析把最后一个参数吃掉了。** `for (i = 1; i + 1 < argc; i++)` 永远看不到
   命令行**最后一个**参数，于是 `... -discover` 什么都不做、静默走了 I/O 路径。
   现在改成遍历全部参数、只有取值的那几个才向前看一位。
2. **⛔ Get Log Page 的长度又写错了半边。** NUMDL 在 CDW10 高 16 位（字节 42..43）、
   NUMDU 在 CDW11 低 16 位（字节 44..45）；我只写了 44，于是 NUMDL=0 → target 认为
   我要 4 字节 → 回 `SGL_INVALID_DATA`，而我的循环把"没有记录"打印成了 `numrec=0`
   —— 日志页其实一直都在。**同一个字段我们已经在 §8.44 踩过一次**，这是第二次。
3. **⛔ `MatchInfo.Matches[2]` 不是"第 2 个捕获组"。** `Select-String` 结果里的
   `.Matches` 是**这一行里所有匹配**的数组，捕获组要写
   `.Matches[0].Groups[2].Value`。这个错误让会话脚本把"授予了 128 条队列"打印成
   "授予了 条队列"（空值），差一点又要点错结论。
4. **⛔ 拿 I/O 控制器的断言去套 discovery 控制器。** LID 1/5 两个日志页探针在
   discovery 控制器上必然失败——因为 nvmet 就是拒绝它们的。这不是"跳过断言"，
   而是**换成更准确的断言**：discovery 控制器上这两个日志页**应当**被拒绝，
   现在我们的 target 也照做了（拒绝，`Invalid Field`）。

#### 还没做到的（本阶段剩余）

1. **DH-HMAC-CHAP 认证**（`nvme connect --dhchap-secret`，nvmet 的 `attr_authentication`）。
2. **重连/错误恢复语义**：KATO 超时、QP 重连、controller reset 之后的完整行为
   （`-serve N` 只是"能连续服务多个控制器"的第一步）。
3. admin 面剩余覆盖（更多 Identify CNS / Async Event 类型 / Set Features）。

### 8.51 ✅ DH-HMAC-CHAP 两端都接进 RDMA 路径，并用真 Linux 双向验过（含"我们的 host 认证到 nvmet"）

认证是"与 Linux 完全兼容"里最大的一块。这一节记录**已经完成并验证的部分**、**CNG 走不通的实测结论**、
**这一轮我自己犯的七个错**（全是"看起来能用、其实错"的那一类），以及**还差的那一项为什么没做**。

#### 先把参考实现读成规格，而不是凭印象写

认证的字节布局、HMAC 拼接顺序、DH 群参数，任何一处猜错都会表现为"HMAC 不对"——
一个不告诉你哪里错的结果。所以我让一个子代理把 Linux 源码读成了规格：
`drivers/nvme/{host,target}/auth.c`、`target/fabrics-cmd-auth.c`、`common/auth.c`，
报告落在 **`ref/NVME_DHCHAP_PROTOCOL_REPORT.md`**（逐条带函数名与行号）。几条关键事实：

| 事实 | 为什么容易写错 |
|---|---|
| 认证发生在 **fabrics Connect 成功之后**；触发方式是 Connect **完成结果的 bit 17（ATR）**，低 16 位仍是 cntlid | 不是状态码，我原本以为 Connect 会带一个"需要认证"的状态 |
| `Ks = H(共享密钥)`，而共享密钥是**正好 p_size 字节、大端、左侧补零**（`mpi_write_to_sgl`） | 任何 bignum 库默认都会把前导零去掉 → HMAC 全错。报告把它列为 1 号坑 |
| Reply 里**永远**有 2×hlen 字节的 rval 区，即使 `cvalid=0` | 只发 16+h+dhvlen 会被判 INCORRECT_PAYLOAD |
| Challenge 的 DH 值在偏移 `16+h`，Reply 的在 `16+2h` | 两个位置不一样 |
| Success2 **没有 HMAC**（16 字节裸头） | 想当然会给它加一个 |
| NQN 不在认证报文里，来自 Connect data；主机密钥绑定 **hostnqn**，控制器密钥绑定 **subsysnqn** | 容易以为认证报文自带 NQN |
| 认证用的群是 **RFC 7919 的 ffdhe**（不是 RFC 3526 MODP），生成元全是 2 | 两者都是"2048 位 MODP 群"，选错就永远对不上 |
| 任何失败都走 `nvmet_ctrl_fatal_error()` → 置 CSTS.CFS 并**断开连接** | 失败不是"回个错继续跑" |

#### Windows 的 CNG 做不了这个 DH（实测，不是推断）

| 尝试 | 结果 |
|---|---|
| `BCryptSetProperty(hAlg, BCRYPT_DH_PARAMETERS, ...)` 装入自定义群 | **`STATUS_NOT_SUPPORTED (0xC00000BB)`** |
| `BCryptImportKeyPair(BCRYPT_DH_PRIVATE_BLOB)` 自带私钥指数 x（公钥字段留空） | **`STATUS_INVALID_PARAMETER (0xC000000D)`** —— 它会校验 `pub == g^x mod p`，也就是说**它偏偏不肯替我们做那唯一需要它做的运算** |

所以 DH 只能自己写。`src/nvmeof_bignum.h` 是定长 Montgomery 模幂（32 位肢、无除法例程），
`src/gen_dhgroups.ps1` 从 **RFC 7919 原文**抓取 ffdhe2048/3072/4096 并逐条验证
（长度、首尾全 1、`p mod 8 == 7`、`(p-1)/2` 为素数——CNG 要求安全素数，规范里的群正好都是）。
抓取过程中踩到两个格式坑：RFC 的**分页页眉插在长十六进制块中间**，以及**每个群的最后一行很短**
（只有 `FFFFFFFF FFFFFFFF`），按"≥32 个十六进制字符才算一行"会把 4096 位素数截成 1008 个字符。

#### 已经验证的部分

```
$ .\f5_interop.exe -authselftest
=== DH-HMAC-CHAP primitives, against published vectors
  [PASS] SHA-256("abc") / SHA-384 / SHA-512            (FIPS 180-4 向量)
  [PASS] HMAC-SHA-256 / HMAC-SHA-512 (RFC 4231 case 1)
  [PASS] DH: both sides derived the same secret
  (src/run_authselftest.ps1 recomputes the [dh] values with BigInteger)
=== DH-HMAC-CHAP protocol pieces (target half)
  [PASS] CRC32-IEEE check value - 0xCBF43926 (want 0xCBF43926)
  [PASS] a DHHC-1 key parses and its CRC verifies
  [PASS] a key with a broken CRC is rejected
  [PASS] the target answers a Negotiate with a Challenge
  [PASS] the target verifies a correct response and authenticates the host
  [PASS] a tampered response is rejected with Failure1
  [PASS] Negotiate carries auth_type COMMON (0x00), not DH-HMAC-CHAP
  [PASS] Failure2 carries auth_type COMMON (0x00)
  [PASS] host and target agree on a one-way exchange, and both are done
  [PASS] a bidirectional exchange verifies the TARGET's response too
  [PASS] a target that holds the WRONG controller key is rejected by the host
```

* **密码学原语没有自证**：`run_authselftest.ps1` 用 .NET `BigInteger` 独立重算
  `g^x mod p` 与共享密钥（CNG/BigInteger 无共享代码），四条断言全过——这是 §8.46 那条
  "自检证明内部一致，不证明正确"在密码学上的落实。
* **后三条是两半互跑**：它们证明的是"我们的 host 与我们的 target 对同一份参考拼接的理解一致"，
  仍然**不证明与 Linux 互联**——那由下面的对端实测负责，而也正是它抓到了 R1 漏掉 t_id 的缺陷。

#### 这一轮我犯的六个错（全是"自认为对"的）

1. **`dblMod` 的"下溢判断"根本不是下溢判断**：我用归约结果最高位判断减法是否下溢，
   而模数 `0xFFFF…` 的最高位本来就是 1 —— 于是几乎每次归约都被丢弃，模幂输出全 0。
2. **`char hex[200]` 装 256 字节的值**：`toHex` 写 513 字节，栈溢出把旁边的值冲掉，
   交叉校验拿到的全是 0。
3. **一个 `printf` 里四个 `toHex` 共用同一块缓冲区**（逗号表达式求值顺序未定义）：
   四个值都打印成最后一个，看起来像"全零"。
4. **`CryptStringToBinaryA` 的 `pcbBinary` 是"进：缓冲区大小 / 出：实际长度"**，
   我传了 0 → 解码失败，而失败长得就像"这个密钥不是合法 base64"。
5. **`sscanf("%2s")` 会在冒号处 NUL 截断**，所以用 `hh[2] == ':'` 检查永远是假 →
   所有合法密钥都解析失败。
6. **nvme-cli 的密钥带结尾冒号**，直接喂给 base64 解码器会失败 → 必须先截掉。

六个里没有一个是"算法想错了"，全是**边界与 API 语义**。"写下来能跑"和"真的对"之间的距离，
和 §8.46 那 11 个缺陷是同一件事。

#### 接进 RDMA 路径（target 侧）

`-authkey <DHHC-1:..>` 让 target 要求认证。接线点是两处，都很小，但每处都有一条"不做就死"的理由：

* **Connect 的完成结果置 bit 17（ATR）**。低 16 位仍是 cntlid，所以"result = cntlid"看起来
  完全正常 —— 一个只读低 16 位的 host 永远不会知道要认证，然后它发的每条命令都会被 0x4191 拒掉。
  主机密钥来自 Connect data 的 hostnqn、子系统和子系统的 NQN，**必须在读到 Connect data 之后**
  才能初始化认证状态机（认证报文里没有 NQN）。
* **gate**：配了主机密钥、且该 admin 队列还没认证时，**非 fabrics** 命令回 `0x191 | DNR`（=0x4191）。
  fabrics 命令（Connect / Property Get/Set / 认证本身）必须放行，否则 host 连"我需要认证"都问不出来。
  I/O 队列不设 gate —— 这是参考实现的规则（`nvmet_check_auth_status()` 对 qid>0 直接返回 true）。

两条**从参考实现读出来、但很容易做反**的规则：

* **discovery 控制器永远不认证**。`nvmet_setup_auth()` 在子系统是 discovery 时直接返回，
  `nvmet_has_auth()` 为假、ATR 永不置位。我们一开始对所有控制器一视同仁地置 ATR ——
  那会让 `nvme discover` 直接失败：这条命令不带密钥，host 看到 ATR 后会发现"我没有密钥"并以
  `-ENOKEY` 结束整个 connect。
* **一出错必须回 Challenge/Reply 之外的东西**：`onAuthSend` 最初要求 `auth_type == 0x01`
  （DH-HMAC-CHAP），而 Negotiate 的 auth_type 是 **0x00**（COMMON）。自测里我手写的 Negotiate
  也写了 0x01，于是**两端一致、世界不同意**——§8.29 那个模式第 N 次出现：
  `auth_type` 在一次交换里是混用的（Negotiate/Failure1/Failure2 是 COMMON，
  Challenge/Reply/Success1/Success2 是 DH-HMAC-CHAP），现在有一个断言专钉这一条。

#### 自己打自己：HostAuth 环回测出来的三个真缺陷

把主机半边也写出来之后，加了一个"host 对 target"的环回自测（`-authselftest` 的后半段）。
它抓到的三个都不是笔误，是**读代码看不出来**的：

1. **R1 里漏了 `LE16(t_id)`**。参考拼接是
   `Ca1 || LE32(s1) || LE16(t_id) || sc_c || "HostHost" || hostnqn || 0x00 || subsysnqn`，
   我写的公共函数从 `Ca1 || LE32(s1)` 直接跳到 `sc_c`。HMAC 只是"不对"，没有任何线索指向这里。
   同一份拼接在文件另一处的手写版里是**对的**——所以两个版本互跑才抓得到。
2. **单向认证时 target 什么都不回**。我原来在"没有控制器密钥"时 `pendingLen = 0`，
   而 nvmet 的 `nvmet_auth_success1()` **永远**构造 Success1（没有控制器响应时 rvalid=0）。
   host 那边在等 Success1，收到 16 个零字节会报 INCORRECT_MESSAGE 并放弃整个认证。
   Linux 主机对我们会走这条路，所以这是一个"和 Linux 互联必挂"的缺陷。
3. **Success2 完全没处理**。双向认证的最后一条是 host 发 Success2（16 字节，无 HMAC），
   target 收到它才算 `authenticated`。原来 `onAuthSend` 把它归到 "unexpected auth_id"。

#### 对端实测（这一轮的硬证据）

**(1) 真 Linux 主机 → 我们的 target，带认证**（`f5_session.ps1 -Auth`，nvme-cli 3.1 / kernel 6.18）：

```
== case 1: connect with the shared host key (the target requires auth)
  [ok] nvme connect succeeded with DH-HMAC-CHAP
  [ok] device /dev/nvme2n1 (serial NDVMEOF...) - our target, not a real disk
   [44503.168094] nvme nvme2: qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048
== case 2: write / flush / read back over the authenticated controller
  [ok] CMP: identical (65536 bytes written, flushed and read back)
== case 3: connect with a DIFFERENT host key - must be refused
  [ok] the wrong key was refused by the target
== case 4: bidirectional (host key + controller key)
  [ok] bidirectional connect succeeded - the host verified the target's response
  [ok] CMP: identical over the bidirectional controller
RESULT: PASS - a Linux host authenticated to our target and moved data
```

target 侧同一轮的计数：`authSends=7 authReceives=6 authRefused=0 authFatal=1`
——第 3 个控制器的 `authFatal` 就是**故意用错密钥**那一次，Failure1 发出去了、控制器随后
照着 `nvmet_ctrl_fatal_error` 的语义作废。`authRefused=0` 是这条链最重要的一个数：
一个有认证的控制器在整轮 I/O 里**没有一条命令**因为"没认证"被拒。

> **注意第 4 条**：Linux 主机在**双向**模式下才会发 `cvalid=1` 的 C2，我们的 target 才会
> 回 `rvalid=1` 的控制器响应，主机验证通过后发 Success2。也就是说 Success2 这条路径
> 是被真对端点亮的，不是自测点亮的。

**(2) 我们的 host ↔ 我们的 target，六个用例**（`run_f5_auth.ps1`，写进了 `run_all.ps1`）：

| 用例 | 期望 | 结果 |
|---|---|---|
| 1 两边同一个密钥 | 认证通过，之后 `Property Get CAP` 正常 | PASS |
| 2 host 没有密钥 | host 主动停（`-ENOKEY` 不重试），target 记录 `authSends=0` | PASS |
| 3 host 用**另一个**合法密钥 | Failure1，`rescode_exp=0x01` | PASS |
| 4 双向 | Success2 发出且 target 记录 | PASS |
| 5 target 的控制器密钥是错的 | **host 拒绝对端**（`the TARGET's response did not verify`） | PASS |
| 6 `-authskip`：无视 ATR 直接发非 fabrics 命令 | target 回 `sct=1 sc=0x91`，`authRefused=13` | PASS |

第 5 条是容易省掉、也最该留着的一条：**一个从不检查控制器响应的实现能过掉 1–4 全部**。

**(3) 同一项当时的 blocker（已在 §8.52 完成）**

写下这段时这一项**没做完**，原因是权限，不是技术：nvmet 的认证配置只能写 configfs
（`/sys/kernel/config/nvmet/hosts/<nqn>/dhchap_key`），而笔记本上的免密 sudo 白名单是
**逐条精确路径**的（三个 `.sh` + `modprobe`/`rdma`/`nvme`/`dmesg`/`ip`），configfs 写入不在里面。
内核本身没问题：`zcat /proc/config.gz | grep NVME_TARGET_AUTH` → `=y`。

支持已经写进 `linux/nvmet_setup.sh`（`AUTH_KEY_FILE=... ` 建 host、写密钥、关掉 `allow_any_host`、
挂 `allowed_hosts/` 符号链接）。**对端装上之后这一格在 §8.52 跑通了**（双向、真 ffdhe2048、
对端内核日志可查）。这一段保留下来，因为"谁需要动手、为什么"仍然是以后加特权步骤时要照抄的形状。

#### 顺带修掉的两个老缺陷（都不是认证引入的，是这轮踩出来的）

* **`interop_link.ps1 -Action LowMtu` 会把"降低后的值"记成基线**。`Save-State` 只在值为空时才
  保留旧值，所以在一个**已经**是 1500/1024 的路径上再调一次 LowMtu，记录里就变成 1500/1024，
  下次 `-Down` 忠实地"恢复"成 1500/1024。基线就这样一个窗口降一格，直到本机两口直连的
  4096 字节传输全部 `ND_IO_TIMEOUT`（这轮真的发生了：`run_all` 从 8/8 掉到 2/9）。
  修法是"不要记录你马上要覆盖的基线"：已经在 1500 就不写。事后把 `以太网 24` 手工恢复到
  4074/4088/2048，`run_all` 回到 9/9。
* **对端脚本用 `sleep 1` 等设备节点**。`nvme connect` 只要**控制器**建成就会返回，
  namespace 扫描是之后在 workqueue 上跑的，所以 `/dev/nvmeXn1` 可能晚几秒。原来等 1 秒就断言
  "没找到 NDVMEOF 设备"——那一次控制器认证完全成功、内核 udev 都已经探测过 namespace 了。
  现在改成轮询最多 30 秒，并且失败时把 `nvme list`、`/dev/nvme*`、`dmesg` 尾巴一起打出来
  （"设备还没出现"和"设备永远不会出现"是两件事，只有后者是失败）。`f5_dirb_check.sh` 里
  同样的竞态也一并修了。

#### 命令

```powershell
# 生成密钥（同一个字符串两端都要用）
.\f5_interop.exe -genkey              # DHHC-1:01:<base64>:

# 本机两口直连，六个认证用例
.\run_f5_auth.ps1

# 和真 Linux 主机互操作（桥窗口内）
.\f5_session.ps1 -Auth -LaptopIp <ip> -LaptopUser <user>      # 两个方向
.\f5_session.ps1 -Down                                        # 收工拆桥
```

### 8.52 ✅ "我们的 host 认证到 nvmet"完成；admin 面用真主机扫出 4 个缺口；以及一次把对端 nvme 栈弄挂的事故

#### 最后一格：我们的 host → 要求认证的 nvmet（双向，真 DH）

对端把 `linux/nvmet_setup.sh`（带 `AUTH_KEY` 的版本）装到根属主路径后，重建、再跑方向 A：

```
[auth] the Connect result asks for authentication (ATR set, result=0x20001)
[auth] Challenge: hash=0x01 dhgroup=1 seqnum=3256517884
[PASS] Auth Send delivered the response
[PASS] the target accepted the host's response (Success1)
[PASS] Success2 sent (the controller's own response verified)
[PASS] DH-HMAC-CHAP authentication completed
...
initiator failures: 0            (36 条断言 + 4/4 多队列)
```

对端内核自己的话（这是关键：不是我们的日志）：

```
nvmet: Created nvm controller 1 for subsystem nqn.2024-01.local.rdma:linux-nvmet
       for NQN nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001 with DH-HMAC-CHAP.
nvmet_rdma: enabling port 1 (192.168.100.5:4420)
```

三件事同时被证明：**ATR 触发**（我们没猜错触发方式）、**真 ffdhe2048 DH**（`dhgroup=1`，
共享密钥是我们自己写的 Montgomery 模幂算出来的，对端是内核 crypto）、**双向**——
nvmet 配了控制器密钥，所以它回 `rvalid=1`，我们的 host 用控制器密钥验了 R2 才发 Success2。
`Success2 sent` 这一行意味着"我们不只会证明自己，还会检查对端"。

顺带一条对端日志也值得记：`nvmet: connect by host nqn...:e9924114... not allowed` —— 那是它自己用
**别的** hostnqn 去连被拒，说明 host 允许列表确实生效（也说明我们的 NQN 是**被显式允许**的那个）。

#### admin 面：真主机扫出 4 个我们答不上的命令

新的对端脚本 `linux/f5_dirb_admin.sh` 把 `nvme-cli` 能对 fabrics 控制器发的 admin 命令扫了一遍
（16 条：Identify 各 CNS、4 个 log page、6 个 Get Feature、1 个 Set Feature），判据不是"都成功"
——原型 target 有权拒绝——而是 **target 自己的 `unknownAdmin` 计数**：

```
[admin] unhandled Get feature fid=0x06     (Volatile Write Cache，两次)
[admin] unhandled Get feature fid=0x04     (Temperature Threshold)
[admin] unhandled Get feature fid=0x0B     (Async Event Configuration)
target summary: ... unknownAdmin=4 unknownIo=0
```

`unknownIo=0` 说明数据面没有缺口；缺口全在 feature 面。修法照抄参考实现：

| fid | nvmet 的行为 | 我们现在 |
|---|---|---|
| 0x06 Volatile WC | `nvmet_set_result(req, 1)` —— 常量 1，即使没有写缓存 | 同样返回 1；Set 接受且不改变任何状态 |
| 0x0b Async Event Config | 存 `aen_enabled`，Set 和 Get 都把值放 result | 存 `aenMask`，Set/Get 都回它（自测里做了 Set→Get 往返断言） |
| 0x04 Temperature Threshold | 落在 `default:` → Invalid Field \| DNR（它自己 `#if 0` 掉了） | 同样 Invalid Field \| **DNR** |
| 其它 | 同上 | 同上（原来漏了 DNR） |

**这里我又犯了一次"凭记忆编常量"，而且这次是被工具抓住的**：我先把 0x04 命名成
`NVMEOF_FID_APST`（"0x04 通常是 APST"），`run_xref.ps1` 直接报
`[no reference] NVME_FEAT_APST`。参考头文件里 **0x04 是 `NVME_FEAT_TEMP_THRESH`，
APST 是 0x0c（`NVME_FEAT_AUTO_PST`）**。这是 §8.29/§8.44 那个模式第 N 次出现，
也是这套交叉核对第三次直接救了我（前两次是 SGLS 与 fctype）。

自测里加了永久断言（`run_f5.ps1` 的 initiator 段）：0x06 必须成功且 result==1、0x0b 必须
Set→Get 往返一致、0x04 必须 `sct=0 sc=0x02` 且带 DNR、0x07/0x0f 必须仍然成功
（只查新三条会漏掉"改坏了旧的"）。

#### ⛔ 我的一次事故：`nvme reset` 把对端的 nvme 栈卡死了

scan 脚本原本最后一步跑 `nvme reset`（控制器复位 → 拆连接 → 重连），想验证"我们能不能被重连"。
它撞出**两个**问题：

1. **我们的 target 只等 20 秒**（"no further connection within 20 s"），而控制器复位后的重连
   按内核自己的节奏来，不止 20 秒。于是 target 在 host 回来之前就退出了 → host 卡在
   `nvme reset` 里直到 Ctrl Loss Timeout（600 秒，正好是我那次工具调用超时的时间）。
   修法：`-reconnectwait <秒>`（默认 20，F7 的断言不变；互操作用 180）。
2. **对端的 nvme host 栈被卡住了**：控制器删除路径上有一个任务进了 D 状态
   （`nvme_ns_remove` → `nvme_remove_namespaces` → `nvme_do_delete_ctrl`），
   `dmesg` 每 120 秒报一次 `INFO: task nvme blocked for more than 737 seconds`，
   `nvme list` 和 `nvme disconnect` 也各自进了 D 状态，之后**任何 `nvme connect` 都返回
   rc=1 且既无输出也无内核消息**。`/sys/class/nvme/nvme2` 还在，删不掉也连不上。
   这台机器要重启才能恢复 —— 方向 A（我们的 host → nvmet）完全不受影响，方向 B 用不了。

`f5_dirb_admin.sh` 现在把 `nvme reset` 改成 **opt-in（`-r`）**，并在执行前检查 target 是否
在答 ping；脚本头部把上面这段原样写上去了。教训写成一句话：
**"删除一个对端已经不在的控制器"是这台对端唯一一种不可恢复的操作，而它长得像一个普通的测试步骤。**

#### 对端脚本的第三次幂等性缺陷（这次是我的假文件系统测试骗了我）

`nvmet_setup.sh` 在真机上重跑，死在：

```
ln: failed to create symbolic link '.../allowed_hosts/nqn...': File exists
setup rc=1
```

`ln -sfn` 在 **configfs 上不是幂等的**：configfs 不支持 unlink，所以 `-f` 删不掉要被替换的
链接，直接 EEXIST，而 `set -e` 就在那里结束脚本。端口那条链接（`ports/1/subsystems/<nqn>`）同理。
两处都改成"已存在就跳过"。

**为什么我本地的假文件系统测试没抓到**：普通文件系统上 `ln -sfn` **成功替换**，所以那个测试
无论有没有守卫都是绿的。修法是把 `ln` 覆盖成一个"目标已存在就 EEXIST"的壳，模型才和 configfs 一致
（`linux/test_links.sh`，7/7，其中一条专门断言"第二次运行**没有尝试**替换链接"）。
这是假测试的边界：它只能验证你建模对了的那部分语义。

#### 还修了一个同类问题：`-Down` 不恢复 MTU

`interop_link.ps1` 的 `-Down` 只拆桥、搬回 IP 和路由，**不碰 MTU**（那是单独的 `-Action RestoreMtu`）。
于是一个"Down 完就走"的窗口会把 RoCE 口留在 1500/1514/1024，下一轮 `-Action Up` 把这组值
**记成基线**，再下一轮 `-Down` 忠实地恢复它 —— 基线一个窗口降一格，直到本机两口直连的
4096 字节传输全部 `ND_IO_TIMEOUT`（这轮 `run_all` 从 8/8 掉到 2/9 就是它）。
现在 `-Down` 直接调用 `Restore-Mtu` 并回读验证；`LowMtu` 的"别记录马上要覆盖的基线"判断也从
`$RdmaAdapter` 改成真正被降的 `$KeepAdapter`（之前查错了网卡，守卫等于没生效）。
回归测试：连续两次 `LowMtu` 之间不 `Down`，记录里的 4074/4088/2048 必须不变。

#### 重启之后：admin 面清零，以及控制器复位暴露的两个 CC/CSTS 缺陷

对端重启后（`/tmp` 被清空、`ip addr add` 的地址和 rxe0 都不持久，两个都要重建）重跑一遍，
`f5_dirb_admin.sh` 16 条命令扫下来：

```
battery: 16 command(s), 1 refused by the host's own error handling
controller 1 summary: ... unknownAdmin=0 unknownIo=0
```

唯一"被拒"的是温度阈值（0x04），而那是**参考实现也拒**的那一条。清零的过程里还发现两处我们
自己的问题：

* **0x04 不该记进 `unknownAdmin`**。它现在是一个显式的 `case`：日志写
  `Temperature Threshold -> Invalid Field | DNR (as nvmet)`，但**不**计数。
  计数器要回答的问题是"真主机发了什么我们看不懂的东西"，而 0x04 我们看得懂、只是照着 nvmet 拒。
* **battery 自己有两行根本没到我们**：`effects-log` 在 nvme-cli 3.1 需要 `-c <csi>`，
  `set-feature` 的值选项是 **`-V`**（大写）而不是 `-v`（那是 verbose）。两条都"看起来失败了"，
  实际上一次都没发出去。判据是 target 端的计数，不是 host 端的 rc —— 这就是为什么这个脚本
  把 rc 和 target 的 summary 分开报。

#### ⛔ 控制器复位：我们的 target 让 host 白等 60 秒

`nvme reset`（-r）第一次跑出来的 host 内核日志：

```
[680.028947] nvme nvme2: resetting controller
[740.805671] nvme nvme2: I/O tag 1 (4001) opcode 0x7f (Property Get) QID 0 timeout
[740.805798] nvme nvme2: Property Get error: 881, offset 0x1c
[741.693811] nvme nvme2: creating 4 I/O queues.          <- 60.8 秒之后才重连
```

**原因是我们自己在 `CC.EN 1→0` 上做得太过**：那两处 `if (st.disabledByHost) break;` 会立刻跳出
控制器循环、销毁 queue pair —— 而 host 的"禁用控制器"流程是**写完 CC.EN=0 之后轮询 CSTS.RDY 直到读 0**。
它的 Property Get 正好撞上我们刚拆掉的队列，于是等满 60 秒命令超时。

nvmet 不是这么做的：它清掉 CSTS.RDY，**把队列留给 host 去拆**（transport teardown 是 host 的动作）。
改成同样语义之后，同一条测试：

```
[976.480299] nvme nvme2: resetting controller
[977.423429] nvme nvme2: queue_size 128 > ctrl sqsize 32, clamping down
[977.639918] nvme nvme2: mapped 4/0/0 default/read/poll queues.     <- 0.94 秒
```

同一个 target，同一个 host，**60.8 秒 → 0.94 秒**。而且我们还多了一条正面证据：target 日志里
出现了**第二个控制器**（`[conn] controller 2 private data` + 它自己的 summary），也就是说
"host 拆掉再连回来"是我们服务出来的，不是自己恢复的。

顺带修掉邻居缺陷：**CSTS.SHST**。host 的断开流程是写 `CC.SHN=1` 然后等 `CSTS.SHST=complete`
（bits 3:2），而我们的 CSTS 只报 RDY，于是每次断开都以
`nvme nvme2: Device not ready; aborting shutdown, CSTS=0x1` 收尾。现在按 `nvmet_update_cc` 的规则
在 SHN 的 0→非零跳变上置 `SHST_CMPLT`、回到 0 时清掉；修完之后那条日志消失了，
target 侧能看到 `shutdown notification requested (CC.SHN=1) -> CSTS.SHST=complete`。
同一批还把 `CSTS.CFS` 接上（认证致命失败时置位），因为 nvmet 的 `nvmet_ctrl_fatal_error` 就是
这么告诉 host "控制器坏了，不是忙"。

**同时撤回我自己写错的一条断言**：battery 原本要求"设备必须消失"才算复位成功。
这台内核上 fabrics 控制器复位是**原地重连**，namespace 不会消失 —— 那条断言在一次耗时 1.07 秒的
成功重连上直接报了 FAIL。现在改成断言真正区分"重连"和"什么都没发生"的两件事：
内核必须出现 `resetting controller` 且**没有** Property Get 超时，并且整个复位在 20 秒内完成。

#### 又一次：MTU 基线（这次修在源头）

上一轮我在 `LowMtu` 里加了"已经在 1500 就别记录"的守卫，结果**又被咬了一次**：这次的破坏者不是
`LowMtu`，而是**下一轮的 `-Action Up`** —— 它不知道路径还是低 MTU 状态，照样 `Save-State`。
而且我的守卫只查 `$KeepAdapter`（RoCE 端点），漏了 `$RdmaAdapter`（被桥接的那口），
所以端点被保护、桥接口照样把 1500 记了进去。

规则现在放在 `Save-State` 里（所有调用者都经过它）：**当前 NlMtu 是 1500 而记录里不是 1500，
就保留记录**——因为 1500 是本脚本自己降的，不是机器的配置。这条和 §8.49 那条"不要把桥接带来的
默认路由记成机器的状态"是同一个规则，只是当时只做了路由那一半。
回归测试：`LowMtu` → **在低状态下跑 `Up`** → `Down`，记录必须仍是 4074/4088/2048、
`Down` 必须把**两个口**都恢复到 4074（实测两口的 artifact 提示都出现，恢复完整）。

### 8.53 ✅ 一个真能用的卷；以及它逼出来的三条"我们自己永远测不出来"的差异

这一轮的目标是**能拿给人看的东西**：让 Linux 主机把数据写进我们的 target，而这些字节
真的落到 Windows 的 `F:` 盘上的一个文件里。做法是 `-nsfile`，结果在 §(5)。
但真正有价值的是过程中被抓出来的三条差异 —— 又是一节"写下来能跑 ≠ 真的对"。

#### (1) `-nsfile`：namespace 就是 F: 上的一个文件

| | |
|---|---|
| 用法 | `f5_interop.exe -target 192.168.100.2 4420 -serve 0 -nsfile F:\nvmeof-ns.img` |
| 新建 | 文件不存在就按 `kNsBytes`（48 MiB）建出来（`SetEndOfFile`） |
| 几何 | `nsze/ncap` = **文件大小 / 512**，不是内存 region 的大小 —— 主机必须看到后端真实的块数，否则它会写到文件末尾之外（实测 `nsze=0x18000` = 98304 块 = 48 MiB） |
| 持久化 | WRITE 只改内存镜像，**FLUSH 才回写文件**（`nsFileFlush`）；这正是文件系统依赖的语义 |
| 打开方式 | `FILE_SHARE_READ | FILE_SHARE_WRITE`。原来只有 `FILE_SHARE_READ`，于是**target 在跑的时候谁也别想读那个文件** —— 而"主机写进去的字节，你去看啊"正是这一轮要证明的事，一个独占句柄让这件事无法验证 |

#### (2) ⛔ 我们自己的 target 把主机的一条命令挂死了

第一次跑宽扫描（`linux/f5_dirb_demo.sh`）时，整条控制器在 **connect 之后 7.6 秒**死掉。
主机侧的内核日志说：

```
nvme nvme2: I/O tag 1 (3001) opcode 0x18 (Keep Alive) QID 0 timeout
nvme nvme2: starting error recovery
nvme nvme2: Property Set error: 880, offset 0x14      ← 连 CC.EN=0 都发不出去了
```

**症状指向 keep-alive，真正的原因在几条命令之前。** 我们的 target 日志里最后两条是：

```
[admin] Get Log Page lid=13 numd=127        -> 512 bytes ... (zero-filled)   ← 我们回了成功
[admin] Get Log Page lid=13 numd=4294967295 -> 0 bytes   ... (zero-filled)   ← 挂在这里
```

链路是这样接起来的：

1. `nvme persistent-event-log` 先读 **LID 0x0d 的 512 字节头**。nvmet 对 LID 0x0d 回
   `Invalid Field`，主机就到此为止；**我们回了"成功、全零"**，于是主机认为头是好的，继续发第二条；
2. 第二条是同一个日志页、`numd = 0xFFFFFFFF`、**SGL 长度 0**；
3. 我们的长度算法是 `(numd + 1) * 4`，`uint32_t` 下 `0xFFFFFFFF + 1` **回绕成 0** ——
   一个 16 GiB 的请求看起来像 0 字节，于是它和主机的 0 字节 SGL "长度相符"，检查通过；
4. 接着 post 了一个 **0 字节的 RDMA Write**。0 字节的 RDMA 操作**不是空操作**：请求发出去、
   **完成项永远不来**；而这个 target 每条队列一次只处理一条 capsule，**这条队列就此停住**；
5. admin 队列不再前进 → 主机的 Keep Alive 没人应答 → KATO 超时 → 主机进 error recovery、
   拆掉队列对 → 我们这边所有队列收到 `ND_CANCELED`。

修法两条，缺一不可：

* **长度用 64 位算**，`asked64 != sgl.len` 时按 nvmet 回 `SGL_INVALID_DATA`；
* **哪几个日志页可以回"成功、全零"是按参考实现量出来的，不是随便挑的**（见 (3)）；
* 顺带加了 `carryData()`：**0 字节的传输一律不 post、直接算成功**，并把"写完成项的
  BytesTransferred"从日志里去掉 —— 这个 provider 给 RDMA Write 的 `BytesTransferred`
  与请求无关（512 字节的写报成 `bytes=643`，两次都一样），把它当长度打印等于编了一个数。

#### (3) 参考表：把 Linux nvmet 当规格量出来（`linux/f5_nvmet_ref*.sh`）

对端那台 nvmet 可以**环回自连**（`192.168.100.5` 在它自己网卡上），所以"参考实现怎么答"
随时可量、不需要桥。三个脚本 + 一个 DSM 探针，全部逐行记录：

**Get Log Page（LID 策略）**

| LID | nvmet 实测 | 我们（修后） |
|---|---|---|
| 0x01 0x02 0x03 0x0c | 成功 | 成功（全零） |
| 0x05 (cmd effects) | 512 B 时 SGL 长度无效 | 成功（我们真的实现了它） |
| 0x04 / 0x09 / 0x12 / 0x16 | Internal Error / Invalid NS / SGL 无效 | `Invalid Field | DNR` |
| **其它（含 0x0d）** | **`Invalid Field | DNR`** | **`Invalid Field | DNR`** |
| 0x70 | 非 discovery 控制器上被拒 | discovery 控制器上才答 |

那四行"nvmet 报的是别的错"是 **nvmet 自己产不出这一页**，不是"正确答案"；错误就是错误，
所以我们对它们统一回最普通的那个。**真正要改的是 0x0d 那一行。**

**Get Features 带数据缓冲**：nvmet 对**任何**带数据缓冲的 Get Features 都回
`Data SGL Length Invalid` —— 量了 8 行，包括它自己实现、答案放在完成项里的 fid（0x01/0x06/0x07），
以及 nvme-cli **自己就会带缓冲**的那些（0x03 LBA range / 0x0c auto PST / 0x0e timestamp /
0x16 host behavior / 0x17 sanitize）。也就是说这个检查在**看 fid 之前**就做了。
我们原来是"按 fid 逐个猜"（回 `Invalid Field`），现在照参考实现改成统一的长度检查。

**Set Features**：`set-feature -f 0x06 -v 1`（Volatile Write Cache）**nvmet 拒绝**（它的
`nvmet_execute_set_features` 里没有这个 case，落到 `Invalid Field | DNR`），而我们原来"友好地
接受并说明什么都没存"。Get 回常量 1 是对的（两边一致），Set 必须拒 ——
**一个比参考实现更友好的回答，正是会掩盖真差异的那类回答。**

**Write Zeroes / Dataset Management**：nvmet 两个都 `rc=0`，我们两个都回 `Invalid Command Opcode`
→ 已实现（见 (4)）。

#### (4) 新实现：Write Zeroes (0x08) 与 Dataset Management (0x09)

* **Write Zeroes**：无数据阶段（dptr 长度必须为 0，否则 `SGL_INVALID_DATA`），
  按 NLB 把 namespace 区域清零；`DEAC` 位只影响日志文案 —— 对文件后端来说"已释放"和
  "读出全零"是同一件事。
* **DSM**：范围表是**target 要读的数据阶段**，这里用同步读（和 admin 路径读 Connect 的 1024 B
  同一个形状）；**先全表校验、再统一清零**，一条命令要么全做要么全不做。
  **"哪个 AD 位算数"是量出来的**：

| `nvme dsm` 形式 | nvmet 实测 | 我们（修前） | 我们（修后） |
|---|---|---|---|
| `-b 8 -d`（只有 CDW11 的 AD） | **读出全零** | 什么都不做 | **读出全零** |
| `-b 8 -a 4`（只有范围里的 AD） | 什么都不做 | 读出全零 | 什么都不做 |
| `-b 8 -a 4 -d`（两个都有） | 读出全零 | 读出全零 | 读出全零 |
| `-b 8`（都没有） | 什么都不做 | 什么都不做 | 什么都不做 |

nvmet 认的是 **CDW11 的 AD 位**，忽略范围描述符里的属性；我们原来正好相反 ——
于是 `nvme dsm -d`（nvme-cli 和内核 discard 路径产出的形式）**回 rc=0 却什么都没干**。
"rc=0 且什么都没发生"是任何只看返回码的测试都抓不到的（`linux/f5_dsm_check.sh` 会读回介质来判）。

#### (5) 成果：Linux 写进来的字节，真的躺在 F: 上

`linux/f5_dirb_demo.sh` 一轮跑完（宽扫描 20 条 + 演示 + 清零验证）：

```
  [ok] nvme write at LBA 8 (4 KiB)
  [ok] CMP: identical immediately after the write
== disconnect and reconnect (a different controller, a different queue pair set)
  [ok] CMP: identical after a DISCONNECT + RECONNECT (the bytes outlived the controller)
== zeroing commands: a pattern first, so 'reads back as zeros' cannot be luck
  [ok] WRITE ZEROES: all 4096 bytes read back as zero
  [ok] DATASET MANAGEMENT (deallocate): all 4096 bytes read back as zero
RESULT: PASS
```

然后在 **Windows 侧、target 还在跑的时候**直接读那个文件（`FILE_SHARE_*` 那一条改动的用处）：

```
F:\nvmeof-ns.img   50331648 字节
--- 文件偏移 4096：
    NVMEOF-DEMO from Linux via our own NVMe-oF/RDMA stack
    host    : yinshibai-asustufgamingf15fx507vvfx507vv
    kernel  : 6.18.41-1-lts
    written : 2026-10-01T17:54:01+08:00
    target  : 192.168.100.2:4420 nqn.2024-01.local.rdma:windows-nd
--- LBA 3000（write-zeroes 之后）    : 4096 字节里 0 个非零
--- LBA 3100（dsm deallocate 之后）  : 4096 字节里 0 个非零
```

#### (6) memory window / STag：从"已知风险"变成"两种配置都绿"

`DESIGN §7` 第一条风险一直是"memory window 的 STag 路径本机从没跑过"。这一轮它跑了 ——
而且**我先前对它的判断是错的，必须撤回**：

> 我上一轮记的是"`stag_smoketest` 红着，根因大概是缺 `ndWireKey` 交换"。**两条都不对。**
> 第一次看到的失败是**桥开着**的时候跑的：被桥接的两块网卡把 TCP/IP 交给了桥，
> `NdOpenAdapter` 直接 `C0000141`，测试根本没连上（这一轮的日志里 `0xC000003E`、
> `client failures: 4` 就是那种"看起来像代码 bug"的环境噪声）。
> 桥拆掉之后剩下的失败也不是 rkey：`run_stag.ps1` **只跑 `readlimit=0` 一种配置**，
> 而 `outboundReadLimit = 0` 的含义是**这条连接不允许发起 RDMA Read**（`nvmeof_rdma.h`
> 开头就写着这条，代价是"一个在 ndstatus.h 里没有名字的 0xC000003E"）。
> 测试却在这种配置下断言 Read 成功 —— 于是它报 4 个失败，其中 3 个是"第一个被拒的 Read
> 把 QP 打成 ND_CANCELED 之后的连带损伤"。

改法不是"放宽断言"，而是**让每种配置断言它真能证明的东西**：`readlimit=0` 时断言
**Read 被拒**、其余读路径报 `[SKIP]`（并说明原因）；`readlimit=64` 时跑完整读路径。
`run_stag.ps1` 现在两种都跑，并已加进 `run_all.ps1`：

| 配置 | 结果 |
|---|---|
| `readlimit=0` | `[PASS] RDMA Read is REFUSED when outboundReadLimit == 0 - status=0xC000003E`；读路径 5 条 `[SKIP]`；**client/server failures 均为 0** |
| `readlimit=64` | `[control B] RDMA Read via the plain MR rkey`、`client RDMA Read through the WINDOW rkey`（65536 B、首字节 0x5A）、`client RDMA Write into the WINDOW rkey`、`remote RDMA Write landed exactly inside the bound window`（窗口内全改、窗口前后一字节未动）、`server RDMA Read from client's MR`、`post-invalidate Read with the stale token fails` —— **全部 PASS，failures 均为 0** |

也就是：**Bind → 远端 Read/Write 命中绑定子区间 → 越界被拒 → Invalidate 真的吊销**，
这条链现在有本机证据了。`run_stag.ps1` 已加进 `run_all.ps1`：

```
total 222 s; 10 of 10 suites passed
run_stag.ps1    PASS   22  client failures=0, server failures=0
```

（这一轮改动同时碰了 admin 与 I/O 两条路径，所以回归是必须的：**10/10、222 秒**。）

#### (7) 这一轮我犯的错

| # | 错 | 代价 / 修法 |
|---|---|---|
| 1 | 把 STag 的红判成"缺 rkey 交换"，还派了个子代理去分析这个不存在的 bug | 已在 (6) 撤回；真因是桥 + `readlimit=0` |
| 2 | `-serve 3` 跑演示脚本：一轮脚本要连 3 次控制器，第二次重跑就没 target 了 | 演示用 `-serve 0`（不限） |
| 3 | 演示脚本的清零段**先 `wait_dev` 再 `connect`**（等一个还没连上的设备 30 秒然后放弃） | 改成先 connect 再等设备 |
| 4 | 读出 `dsm` 的 `nlb=513`、`attr=0` 之后，第一反应是"我们的解析错了" | 其实是我用错了 nvme-cli 的参数（`-b` 是块数、`-c` 是 CDW11），**测试的期望错了，不是被测代码错了** |
| 5 | `sudo -n timeout 15 nvme ...` | 白名单是**精确路径** `/usr/bin/nvme`，套一层 `timeout` 就变成"需要密码"，整份参考表 20 行全废；正确写法是 `timeout 15 sudo -n nvme ...` |
| 6 | awk 里用空变量拼模式 `'/'"$SERIAL"'|NDVMEOF/'` | 空模式匹配到表头，`DEV` 变成字面量 `Node`，后面每条命令都在"设备 Node"上跑 |
| 7 | 把写完成项的 `BytesTransferred` 当传输长度打印 | 512 字节的写报成 643；已改为打印"请求的长度" |
| 8 | 为了让 Windows 侧能读文件，第一版只加了 `FILE_SHARE_READ` | 读仍然失败；要 `FILE_SHARE_READ | FILE_SHARE_WRITE` |

#### (8) 还没做到的

1. **并发控制器**：`-serve N` 是**串行**服务 N 个控制器，不是同时；真阵列上多主机并发连
   是常态。
2. **target 侧的 KATO 过期**：主机静默消失时，target 应该自己进 CFS 并断开（现在只有
   主机侧的超时被测过）。
3. **reservation（resv-*）**：nvmet 也回 `Invalid Command Opcode`，所以我们与参考一致；
   但真机上是支持的，这是"一致地不支持"。
4. **telemetry-log / persistent-event-log 的真实内容**：现在按参考实现拒绝，没有实现内容。
5. **`effects-log` 的 CSI**：nvmet 对 CSI=1 回 `Invalid Log Page`，我们回我们自己的
   effects log —— 一处已知的小偏差，见 (3) 的对照表。

### 8.54 ✅ Windows 上的 NVMe-oF 盘符（X:）：Linux 的命名空间，挂进资源管理器

用户要的是"能在 Windows 上看见 NVMe-oF"。这一节记录做出来的东西、它**到底是什么**、
以及为它踩出来的四个缺陷（其中两个是"报告成功但数据没到"那一类）。

#### (1) 先说清楚这是什么，不是什么

Windows **没有**内置的 NVMe-oF initiator（那是 Windows Server / S2D 的能力，客户端 SKU 没有），
所以盘符不可能来自系统自己的栈。这里让**我们自己的 initiator 去当那个角色**：

```
Linux nvmet  --NVMe-oF/RDMA（我们的 NetworkDirect 栈）-->  F:\nvmeof\nvmeof-volume.img
                                                              |
                            加上 512 字节 VHD 尾（fixed VHD）--+
                                                              |
                       Mount-DiskImage -> NTFS -> X:  <--------+
```

* X: 是一个**真的 Windows 卷**：资源管理器里能打开、能建文件、能算校验和；
* 它的字节**双向**都走我们自己的 NVMe-oF/RDMA 实现：`-dump` 取回、`-Unmount` 回推并 FLUSH；
* **它不是活动块设备**：往 X: 里写不会立刻到达对端，要等 `-Unmount` 回推 —— 因为 Windows
  没有任何办法把用户态进程的缓冲当成磁盘交给卷栈。把这句话写清楚，是"演示"和"宣称"的区别。

命令：

```powershell
.\mount_nvmeof.ps1                      # 从 Linux nvmet 取回命名空间并挂成 X:
.\mount_nvmeof.ps1 -TargetIp 192.168.100.2 `
     -Subnqn nqn.2024-01.local.rdma:windows-nd   # 或者从我们自己的 target（不需要 Linux）
.\mount_nvmeof.ps1 -Unmount             # 卸载 + 回推
```

#### (2) 证据（Linux 命名空间 64 MiB，全程走线）

| 步骤 | 实测 |
|---|---|
| 取回 | `f5_interop.exe -initiator 192.168.100.5 4420 192.168.100.2 -dump ...`：131072 块 / 64 MiB，**28.2 MiB/s**，每条命令成功 |
| 挂载 | `Mount-DiskImage` → NTFS、卷标 NVMEOF、62.9 MiB、盘符 **X:** |
| 写入 | `X:\FROM-WINDOWS.txt`（文本，含时间戳）、`X:\random-2mb.bin`（2 MiB 随机，SHA256 `67D543F7…`）、`X:\a-folder\nested.txt` |
| 回推 | 64 MiB，2.27 s，`every command succeeded`，随后 FLUSH |
| **对端读回** | Linux 用 `nvme read` 把 64 MiB 全读出来：LBA 0 是 `33 c0 8e d0 bc 00 7c 8e c0`（Windows MBR，**偏移 0**），`sha256 = 76e38bd1…` **与本机镜像逐字节相同**，并且 grep 到了 Windows 写的那段文本 |
| 再取回 | 重新 `-dump` + 挂载：三个文件都在，2 MiB 随机文件的 SHA256 仍是 `67D543F7…` |

也就是：**Windows → Linux → Windows 一整圈，字节级对得上。**

#### (3) ⛔ 缺陷一：VHD 尾的校验和算早了

`New-VhdFooter` 先算校验和、后写 unique id，而校验和覆盖**全部 512 字节**。Windows 的回答是
`The file or directory is corrupted and unreadable` —— 没有一个字指向尾部的校验和。
修法：**所有字段写完（包括 16 字节 GUID）再算校验和**。判据也不是猜的：脚本里同时按同样的
规则独立重算一遍，两者必须相等（第一版就是这一句话把差值 0x826 暴露出来的）。

#### (4) ⛔ 缺陷二：回推的是 `.img`，而 Windows 写的是 `.vhd`

卷挂上之后，**Windows 写的是被挂载的那个 VHD 文件**；`.img` 停在被包装的那一刻。
第一版 `-Unmount` 直接回推 `.img`，于是**回推报成功、每一块都传了、对端拿到的是写入之前的旧副本**。
这正是"测试通过而没有数据"的样子。修法：卸载后先把 VHD 的**尾部 512 字节剥掉**写回 `.img`
（`Split-Vhd`），再回推。附带一处：`-Unmount` 在"当前没挂载"时原来直接 `exit 0`，
于是"更新过的卷"就留在 VHD 里没被推回去 —— 现在无论挂没挂载都继续走回推。

#### (5) ⛔ 缺陷三：卷搬运跑完之后，自检又往卷里写了一遍

`-push` 报"每条命令成功"，对端读回来却是：块 0 = `00 11 22 33 44 55 … 10 21 32 43 …`
—— 那是我们**自己的多队列测试图案** `k * 17 + q * 31`（`f5_interop.cpp` 里那两行），
几百行之后的自检把刚推上去的引导扇区盖掉了。修法：**`-dump`/`-push` 时先搬运、然后直接返回，
所有会写介质的自检一律不跑**；顺带加了逻辑块大小守卫（只搬 512/4096 B 的命名空间）。
一个会在用户的卷上跑 I/O 自检的卷工具，不是卷工具。

> 一条**没有解释清楚、但现在已经不影响**的观察，照实记下来：修之前那次读回里，
> Windows MBR 出现在**非 512 对齐**的偏移 66059861。协议里任何写入都是块对齐的，
> 所以我先怀疑的是**我自己的 dump 脚本**（被拒绝的 2 MiB 读可能留下半个暂存文件，
> 重试后在 `cat` 时多拼了一段）。把暂存文件改成每次读之前都删掉之后，dump 的
> **sha256 一模一样** —— 也就是说那不是原因。真正的原因是 (5)；修完之后回推是逐字节相同的
> （sha256 相等），所以 `-push` 本身没有问题。那个偏移从何而来我没有查清，不再猜。

#### (6) 又一个"拿 I/O 控制器的断言去套 discovery 控制器"（这一类第三次）

Linux 窗口里跑完整会话，`direction A: PASS 41 项`、`direction B: PASS`，**只有 discovery 那一步报 3 个 FAIL**：

```
[FAIL] Get Features 0x06 Volatile Write Cache is answered - result=0 (nvmet answers the constant 1)
[FAIL] Get Features 0x0b Async Event Configuration round-trips - set->0x00000000 read-back->0x00000900
[FAIL] Get Features 0x07 and 0x0f still answer after the sweep
```

失败的**不是我们的 target，也不是 nvmet**，是我们 initiator 的断言：它对着 **discovery 控制器**
问 I/O 控制器的特征，而 discovery 控制器没有写缓存、没有命名空间、没有事件掩码，
nvmet 全部拒绝（这是对的）。§8.50(4) 修过一次日志页、§8.53 修过日志页 LID 策略，
这是第三次。现在 discovery 分支断言的是**拒绝**：

```
[PASS] Get Features 0x04 is refused with Invalid Field | DNR, as nvmet - sct=0 sc=0x02
[PASS] a discovery controller refuses the I/O-controller features (0x06, 0x07) - as nvmet - sc=0x02 / sc=0x02
initiator failures: 0
```

**一个永远不可能通过的检查，比没有检查更糟**：它让一个正确的对端看起来是坏的。

#### (7) 这一轮的回归

```
run_all.ps1（10 套）：10 of 10 passed，228 秒
f5_session.ps1（不认证）：direction A PASS 41 项 / discovery PASS / direction B PASS
f5_session.ps1 -Auth：direction A PASS / discovery PASS /
                      RESULT: PASS - a Linux host authenticated to our target and moved data
                      （target: authRefused=0，无 CFS）
```

#### (8) 还没做到的

1. **活动的盘符**（写 X: 立刻到对端）需要一个 Windows 内核驱动或 S2D，不在本工程范围内；
   现在这条是"按需同步"，这一点在脚本头部和本节的 (1) 都写明了。
2. 并发控制器（`-serve N` 仍是串行）、target 侧的 KATO 过期、reservation、telemetry 内容
   —— 与 §8.53(8) 相同。

### 8.55 ✅ Windows 里出现了一块真的 iSCSI 磁盘：`Get-Disk` 认它，GPT 分区表读得出来

**这一节是"1 TB Linux 盘挂到 Windows"这件事的技术核心落地。** Windows 客户端 SKU 没有
NVMe-oF initiator 内核驱动（Server 有），也没有任何用户态进程能把自己变成一块盘；它有的
是内核里的 iSCSI initiator。于是本工程实现了 iSCSI 的**对端**：iSCSI 进、NVMe-oF 出。
`src/nvmeof_iscsi.h` 是那个 target，`f5_interop.cpp` 里 `NvmeIscsiBackend` 把每条 SCSI
命令翻译成同一条队列对上的 NVMe 命令——没有第二个连接、没有 IPC、没有中间拷贝（除了
下面第 6 条说明的注册内存暂存）。

结果（本机 40G 两口直连、namespace 是 48 MiB 的 GPT+NTFS 测试卷）：

```
Get-Disk | ? BusType -eq iSCSI
Number FriendlyName        SerialNumber         PartitionStyle OperationalStatus SizeMB
     3 NVMEOF iSCSI-NVMeoF NDVMEOF0000000000001 GPT            Online                48

Get-Disk | ? BusType -eq iSCSI | Get-Partition
PartitionNumber DriveLetter SizeMB Type
              1               47.9 Basic
```

**分区表读得出来**是这里的证据：它证明读路径真的通了（Windows → iSCSI → 桥 → NVMe-oF/RDMA
→ namespace），而不只是登录成功、枚举出一个空壳设备。

#### (1) 从"登录就卡死"到"盘出现"之间，一共是 7 个 bug

每一个都有它自己的证据，按发现的顺序：

1. **收到 PDU 时没有跳过 data segment 的 4 字节对齐填充**（`readPdu`）。
   iSCSI 的 DataSegmentLength **不含**填充（RFC 7143 §11.7），而 discovery 的 login 文本
   正好 88 字节（4 的倍数，没有填充），**普通会话的 login 文本是 127 字节**（1 字节填充）。
   漏掉那 1 字节 → 之后每个 PDU 都错位 1 字节：initiator 第二个 Login Request 的 opcode
   `0x43` 落进了我们的 flags 字段，我们把它读成 opcode `0x00`（NOP-Out），回了一个 NOP-In，
   于是 Windows 一直等一个永远不会来的 Login Response，直到 30 秒登录超时。
   **这一条是整件事的根因**，而它之所以藏了这么久，是因为只记录"我们发了什么"的日志
   看不出"我们读到了什么"——见第 7 条。
2. **单会话串行**：一次只服务一条连接，一条卡住的会话就把整个 target 冻住——连 discovery
   都不再被 accept。改成每连接一个对象 + 一个线程，backend 仍由互斥量串行化（一条队列对
   不能并发提交）。证据：`AddTargetPortal` 的 discovery 曾经完全无人应答，而日志里什么都
   没有——PDU 躺在 listen backlog 里。
3. **MaxCmdSN 从不前进**（停在 1）。RFC 7143 §6.3.1：initiator 不得发送 CmdSN > MaxCmdSN。
   于是 Windows 只发得出**一条**命令，然后合法 CmdSN 用完，会话活着、什么都不报。
   现象：READ CAPACITY(10) 回了 GOOD，之后永远沉默。修法是 `maxCmdSn = expCmdSn + kCmdWindow - 1`。
4. **发送 Data-In 时没有补 4 字节填充**（`sendDataIn`）。INQUIRY 的 VPD 页表是 7 字节，
   没有填充就把后面的 SCSI Response 顶偏 1 字节，initiator 读到垃圾、直接断会话——
   而 target 日志里每条命令都是 GOOD。这也是第 1 条的对偶：**同一个规则，收发两边都踩。**
5. **短传输却报 residual 0**：INQUIRY 要 255 字节、只回 7 字节、然后说"没有残留"——
   两条自相矛盾的信息。现在小描述符命令直接返回**整个 allocation length**（零填充），
   彻底绕开 residual；大传输的 residual 由 `sendScsiRsp` 自动推导（U 位置 1）。
6. **我自己引入的回归**：把会话改成多线程时，把调用方预分配、**已注册**的 scratch 缓冲区
   换成了每个线程新建的空 `std::vector` → `data()` 是空指针 → 对端日志里出现
   `sgl(addr=0x0 ...)`，NVMe 读命令取不到数据、只回一个没有解释的状态字。
   现在 READ 落到桥自己的注册暂存区（`kIscsiOff`）再拷出来，WRITE 反之；这也是这一节
   唯一一处额外拷贝。
7. **日志的盲区本身是个 bug**：`setvbuf(stdout, _IONBF)`（重定向到文件时 CRT 是 4 KiB 块缓冲，
   而长驻进程是被 kill 的，最后几 KB 直接丢掉——日志会**对最后一条事件撒谎**）、
   每个收到的 PDU 打一行 `<- opcode/flags/datalen`、每条 SCSI 命令打一行 CDB、每个响应打一行
   status/residual。第 1 条和第 3 条都是被这几行直接指出来的；没有它们，能看到的只有
   "initiator 不说话了"和"target 说 GOOD"，两者都真实，两者都不够。

#### (2) 方法上的教训（第三次同一类）

第 1 条这个 bug 无法从"我们的实现"里读出来，只能从**参考实现**里读出来：LIO 自己解析
填充、Windows 的 PDU 布局是量出来的。这一轮做对的一件事是没有再从记忆里编码 PDU 布局，
而是做了一个**字节级 A/B 探针**（`src/tools_login_probe.ps1`）：把 Windows 真实发出的
security login 原样重放给 LIO 和我们的桥，再重放 operational login（168 字节，`T=1 CSG=1 NSG=3`），
两边逐字节对比。它给出的两份 Login Response 只差 alias 文本长度和 StatSN——**正是这个"几乎
完全相同"把方向从"我们的应答字节不对"扭到了"应答之外的某处"**，最后落到填充上。

#### (3) 1 TB 盘的最后一公里：卡在物理链路上，不在代码里

Linux 侧完全就绪（本节确认）：

```
/sys/kernel/config/nvmet/subsystems/nqn.2024-01.local.rdma:linux-nvmet/namespaces/1/device_path = /dev/nvme1n1
enable=1，端口 1 = rdma 192.168.100.5:4420 ipv4，allow_any_host=1，盘未挂载、无 holders（安全检查通过）
```

但 Windows 现在**没有**到那台机器的 RDMA 路径：

```
Get-NetAdapterRdma            -> 只有 以太网 23 / 以太网 24（HP/Mellanox 40G 两口）
Get-NetNeighbor 以太网 23      -> 192.168.100.2 = 48-0F-CF-F0-16-82 = 以太网 24 自己的 MAC
```

也就是说**两个 40G 口是互插的一对**（这正是本工程 10/10 自测跑的本地 RoCE 环回），
而对端笔记本只有一块 Intel I211 千兆（`192.168.1.9` 在 以太网 20 上 ARP Reachable，
TCP 可达），**它不具备 RDMA 能力**。所以 `-initiator 192.168.100.5` 的 RDMA-CM connect
失败，是链路问题，不是本工程的问题：

```
[FAIL] admin RDMA-CM connection to the target - connect failed
Test-Connection 192.168.100.5 -> False   （两端 ARP 互不响应：Windows 侧 Incomplete、peer 侧 FAILED）
```

要让 1 TB 盘真的出现在 Windows 上，需要三选一（都要人做物理决定）：

1. **给笔记本一条 RDMA 链路**：一块 Mellanox ConnectX-3/4（或支持 iWARP 的 Intel X520/X710）
   插进笔记本，与 Windows 的 40G 口同 L2（QSFP→SFP+ 转接 + 10G 模块）。之后本节这条命令
   一条不用改，只把 `-initiator` 的目标地址换成对端地址。
2. **把 1 TB NVMe 盘直接插到 Windows 机器上**：立刻可用，且验证的是完整性能路径
   （`-serve` 导出 + `-iscsi` 桥接）。
3. 只想要盘符、不要求走本工程的 NVMe-oF 栈：在 Linux 侧用 LIO/targetcli 做 iSCSI。
   注意 LIO 那条路在本机内核上也还没走通（dmesg 里是 `Bad lun_ci, not a valid lun_ci pointer`，
   与 §8.54 记录的"ACL LUN 映射不上"是同一件事）。

#### (4) 复现命令

```powershell
# 1) 本机：target 导出 namespace，桥把它变成 iSCSI 盘（只读，默认）
Start-Process .\f5_interop.exe -ArgumentList '-target','192.168.100.2','4420','-serve','0','-nsfile','F:\nvmeof\gpt-ns.img'
Start-Process .\f5_interop.exe -ArgumentList '-initiator','192.168.100.2','4420','192.168.100.3','-iscsi','3260'

# 2) Windows 自己的 initiator：发现 + 登录（不需要任何内核驱动）
iscsicli AddTargetPortal 127.0.0.1 3260
Connect-IscsiTarget -NodeAddress iqn.2024-01.com.nvmeof:bridge0 -IsPersistent $false

# 3) 看盘
Get-Disk | Where-Object BusType -eq 'iSCSI'
Get-Disk | Where-Object BusType -eq 'iSCSI' | Get-Partition

# 4) 字节级 A/B（把 Windows 的真实 login 重放给参考实现和我们自己）
.\tools_login_probe.ps1 -Server 192.168.1.9 -Port 3261 -TargetName iqn.2024-01.local.rdma:linuxtarget -Stage2
.\tools_login_probe.ps1 -Server 127.0.0.1   -Port 3260 -TargetName iqn.2024-01.com.nvmeof:bridge0 -Stage2
```

**只读是默认，而且是安全栏**：桥默认 `-iscsirw` 关闭，MODE SENSE 报 WP，WRITE 一律
拒绝——nvmet 没有只读 namespace 属性，所以"别写坏那块盘"这件事只能在这一侧保证。

### 8.56 ✅ 1 TB 真盘挂到 Windows：`E:`，931.51 GB，在线，读得出内容；SMART 证明全程零写入

**目标达成。** Linux 上那块 `CT1000P3PSSD8`（`/dev/nvme1n1`，`nsze=0x74706db0` = 1953525168 × 512 B）
经 nvmet 导出，被我们自己的 NVMe-oF/RDMA initiator 接住，再由我们自己的 iSCSI target
交给 Windows 自带 initiator，成为一块活动的网络盘。完整实测输出见 `EVIDENCE-1TB.md`。

```
Get-Disk | ? BusType -eq iSCSI
Number FriendlyName        SerialNumber         PartitionStyle OperationalStatus SizeGB IsReadOnly
     3 NVMEOF iSCSI-NVMeoF 4888a61af919cf3f63b5 GPT            Online            931.51       True

分区 1 = 300 MB ESP，分区 2 = 16 MB MSR，分区 3 = 931.2 GB NTFS -> 盘符 E:
E:\ 里是这台机器上的真实文件（开发工具、游戏、下载等具名目录 + 发行版 ISO + pagefile.sys，
顶层 60 项；名字属于私人内容，见 EVIDENCE-1TB.md 第 3 节），时间戳是真的。
```

#### (1) 我上一轮的错误结论，先撤回

我曾在汇报里写"1 TB 盘卡在物理链路，要买 RDMA 网卡"。**错。** 本仓库的
`src/interop_link.ps1` 早就为此写好了：把 LAN 网卡（`以太网 20`）与 **一个** CX3 口
（`以太网 23`）桥接，LAN 段与 CX3 直连段就合成一个 L2，另一个 CX3 口（`以太网 24`）留作
RoCE 端点；再 `-LowMtu`，因为 CX3 侧的 2048 字节 RoCE 帧过不了 1500 的 LAN（不是"慢"，
是丢，症状是 connect 超时且任何日志里都没有原因——脚本头部写着）。做完之后：

```
ping 192.168.100.5 = True
Get-NetNeighbor 192.168.100.5 -> BE-74-DA-40-ED-E3  Reachable  以太网 24
```

教训和 §8.55(2)、§8.29/§8.40/§8.44 是同一条：**先读自己的仓库，再下结论**。我当时的
证据（两个 40G 口互插、对端只有千兆、`Get-NetAdapterRdma` 只列出两个 CX3 口）全是真的，
但"因此做不到"是错的——缺的是那一步桥接，而它已经在仓库里躺着了。

#### (2) 只读这件事，用盘自己的计数器证明

"没写"不能靠"我打算不写"来主张，得让**盘**说话：

```
SMART 基线（挂载前） Data Units Written : 32884752
SMART 之后           Data Units Written : 32884752      <- 完全没变
                     Data Units Read    : 144241018     <- 从 144240893 涨上来：读真的发生了
```

写侧还有两道独立的闸，都实测过：

```
> New-Item E:\__rdma_write_probe.tmp
写入被拒绝: The media is write protected.

桥日志： [iscsi] WRITE lba=651264 blocks=16 REFUSED (read-only bridge)
         [iscsi] -> itt 0x0000003D status 0x02 residual 0 sense 18
```

`lba=651264` 恰好是 NTFS 卷的首扇区（`333447168/512`）——Windows 挂载时想写"脏位"，
被桥按只读挡成 CHECK CONDITION（18 字节 sense）。747 条 CDB 里只有这**一次**写尝试。
三层保护各自独立：桥的 `-iscsirw` 默认关、MODE SENSE 报 WP、Windows 侧 `IsReadOnly=True`。

#### (3) 内容是真的：两次字节级对照，两个方向

**(a) 文件 → 物理块。** `fsutil file queryextents E:\telemetry.db` 给出 `LCN 0xce2e`、
5 个簇；加上分区 3 的偏移 333447168（**这一步我第一遍漏了，Linux 侧读出全 0；补上偏移后
逐字节相同**——错的是一次算术，不是桥）：

```
绝对 LBA = (333447168 + 0xce2e*4096)/512 = 1073520
Windows 经 E: 读          前 16 字节: 53 51 4C 69 74 65 20 66 6F 72 6D 61 74 20 33 00
Linux 裸读 LBA 1073520    前 16 字节: 53 51 4c 69 74 65 20 66 6f 72 6d 61 74 20 33 00
sha256(512) 两侧: dec615ae97b8b81cd5e395fd4f9b1fd379332c3516a16ab122c1c1a57a5dd397
```

**(b) 裸扇区 → 裸扇区。** ESP 引导扇区（LBA 40）两侧 sha256 都是
`802b1462…a2452293`，OemName 两侧都是 `MSDOS5.0`。

**卷标**：这块盘两个卷（ESP/FAT32 与 NTFS）的卷标字段**本来就是空的**，两侧读到的字节
完全一致（`'           '` / `Volume Name :` 空）——是盘上没写过卷标，不是读不出来。

#### (4) 已知小问题

桥与 nvmet 的 keep-alive 在 584 次连续 `READ(10)` 期间超时过 2 次
（`keep-alive to the NVMe target failed`）。会话没掉、数据没错，但 KATO 120 s 加大读流下
`tick()` 的等待策略值得放宽或重试。属于健壮性项，记在这，不藏。

#### (5) 复现与收工

`EVIDENCE-1TB.md` 第 7 节给了完整命令。注意 `-subnqn nqn.2024-01.local.rdma:linux-nvmet`
必须给（nvmet 上不存在本工程默认的 subnqn，少给会得到 `sct=1 sc=0x82 DNR`），
收工后要 `interop_link.ps1 -Action Down` + `-Action RestoreMtu` 把 LAN/CX3 的协议绑定
还给系统。

### 8.57 ✅ 串流压测：把"能用"和"能持续用"分开；以及一个 50 倍的自我伤害

**为什么要做**：到 §8.56 为止，桥只被小命令验证过（枚举、读扇区、几百条 READ）。"能不能持续
串流"是另一个问题，而它一压就露出了三件事。

**方法**：两个方向各压一遍。
1. **裸栈**（`f4_pipeline`，本机 40G 两口直连，另一套 target 实现）：20 轮 × 512 MiB/相位。
2. **整条 Windows 路径**（Windows 自带 iSCSI initiator → 桥 → NVMe-oF/RDMA → 48 MiB namespace），
   顺序读/写、按块大小扫描。

**裸栈结果**（20 轮全部有效，178 秒，共约 20 GiB 流量）：

```
write: 平均 1230 MiB/s   最小 883   最大 1852
read : 平均 1179 MiB/s   最小 664   最大 1610
```

`-rounds 512`（1 GiB/相位）失败在 `Register 0xC0000017`，即这台 7.9 GB 的机器**内存不够**
——F4 把整个工作量放在内存里，这是工具的限制，不是协议问题。

**Windows 路径结果（同一块 48 MiB namespace，64 KiB 块，6 秒一档）**：

| 块大小 | 带日志 | 关掉日志 |
|---|---|---|
| 64 KiB | 1.23 MB/s（19.6 次/秒） | **60.2 MB/s（963 次/秒，1.04 ms/命令）** |
| 256 KiB | 2.60 MB/s | **85.7 MB/s（343 次/秒）** |
| 1 MiB | 2.58 MB/s | **91.5 MB/s（91.5 次/秒，10.9 ms/命令）** |

#### (1) 第一个发现：日志把吞吐压掉了 50 倍

F5 的 target 每条命令都打印 trace（`[wire]` 那几行是十六进制转储），桥也打。把两侧 stdout
都丢进 `NUL` 之后，64 KiB 块从 1.23 MB/s 变成 60.2 MB/s。**控制台 I/O 就是那个瓶颈**，
而它在此前所有"小命令"测试里完全无害——因为那些测试一秒只发几条命令。

于是有了这条方法学：**任何吞吐数字，必须注明被测进程的 stdout 指向哪里**；否则测的是日志。

#### (2) 第二个发现：延迟受限，不是带宽受限

关掉日志后仍然能看出形状：64 KiB 时每条命令 1.04 ms，1 MiB 时 10.9 ms（≈16 × 64 KiB 的顺序
往返）。桥是**严格串行**的：一条 SCSI 命令、内部 N 次 `submitCommand` 逐块等待，所以
吞吐 = 块大小 / 往返延迟，而不是链路带宽。要真正快，需要让同一个命令的多个块**并行提交**，
或者让多条 SCSI 命令同时在飞（`MaxCmdSN` 已经开了 64 的窗口，但桥一次只处理一条）。

#### (3) 三个我自己踩的坑，都是"测量工具本身"

1. **用 `GetTickCount64` 量微秒级的东西**：它的粒度是 15.6 ms，于是所有测量值都"恰好"是
   15/31/47 ms 的整数倍。我据此错判了两次（先怪 Nagle、再怪 target 的 Sleep），
   直到换成 `std::chrono::steady_clock` 才看到真相。
2. **先按猜测动手**：我加了 `TCP_NODELAY`（iSCSI 确实该加，见 §8.55 的代码注释）——但它
   **不是**瓶颈，加了以后数字一模一样。改动本身保留（对交互式协议是对的），但当时的结论是错的。
3. **写测试的访问模式写错**：`[System.IO.File]::Open(drive,'Open','Read','ReadWrite')`
   第三个参数是访问模式，我传了 `'Read'`，于是 57 秒里全是 "Stream does not support writing"
   异常——**测量脚本自己的 bug 被当成了被测对象的失败**。

### 8.58 ❌ 写路径是坏的：R2T 被 Windows 判为非法 PDU（未修复，附完整证据）

这是本轮压测最重要的产出，而且是个**坏消息**，所以放在这里而不是埋在正文里。

`-iscsirw` 打开后再写盘，Windows 报 `The request could not be performed because of an I/O
device error`。桥侧完整的时序（每次重试都一样）：

```
[iscsi] <- SCSI CDB 2A 00 00 00 40 00  lun=0 itt=0x00000002 edtl=65536 cmdsn=2
[iscsi] R2T itt=0x00000002 ttt=1 sn=0 offset=0 len=65536 -> sent
[iscsi] session ended ... connection closed: 2 command(s), 0 bytes in, 36 bytes out
```

也就是说：CDB 到了 ✓、**R2T 发出去了** ✓、然后 **Windows 一个字节的 Data-Out 都没回，直接关连接**。
Windows 侧对应的事件把原因写得很清楚（Provider `iScsiPrt`，8 分钟内 99 次）：

```
Id=23  Target sent an invalid iSCSI PDU. Dump data contains the entire iSCSI header.
dump: 00 00 30 00 01 00 00 00 00 00 00 00 17 00 00 C0 ... 31 00 00 00 ...
                                                         ^^ opcode 0x31 = R2T（我的 PDU）
```

`0xC0000017` 是 `STATUS_NO_MEMORY`，出现在 iScsiPrt 处理这个 R2T 的路径上；被拒的 PDU 就是
R2T。**结论：这条写路径从来没有跑通过**——§8.55 那次 1 TB 演示全程只读，写路径一直没被触发
（B-3 早就把它列为"从未端到端验证"，现在知道了：它不只是没验过，它是不通的）。

**和 §8.55 那个 B-1 改动的关系**：B-1 把 `InitialR2T`/`ImmediateData` 从"不回答（于是沿用
initiator 的 `InitialR2T=No, ImmediateData=Yes`）"改成显式回答 `InitialR2T=Yes, ImmediateData=No`。
现在的行为是"initiator 等 R2T、我们发 R2T、initiator 判非法"——所以**很可能是 B-1 让这条本来就
有问题的路径从"靠侥幸"变成了"必然失败"**：旧协商下 Windows 会先发未经请求的数据，桥的
`readPdu` 循环也许正好把它吃掉。两种可能都还没证实，**这就是下一步要做的第一件事**。

**为什么这不算"把功能搞坏了"**：出厂的默认是**只读桥**（`-iscsirw` 不开），1 TB 真盘那条
演示路径不受影响（SMART 证明零写入）。但要恢复写能力，必须修 R2T：
按 RFC 7143 §11.9 逐字段核对（opcode 0x31 / F / DataSegmentLength / **LUN 8 字节** /
ITT / TTT / StatSN / ExpCmdSN / MaxCmdSN / R2TSN / Buffer Offset / Desired Data Transfer Length），
并与 LIO 的 R2T 做一次字节级 A/B（`tools_login_probe.ps1` 那套方法可以直接复用：
让 LIO 接受一次写，把它发的 R2T 抓下来当参考）。

## 7. 已知风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| memory window 在 CX3/WinOF 5.50 上的实际行为未验证 | WinOF 支持 `ND_MR_FLAG_ALLOW_REMOTE_WRITE` 是确定的，但 `CreateMemoryWindow` + `Bind` + `Invalidate` 这条 STag 路径本机从没跑过 | F3 之前先写一个最小 STag 冒烟测试（bind → 远端 Read → invalidate → 确认失效） |
| `max_mb` / 读限额与 in-capsule 空间的关系 | NVMe-oF 要求 host 依 `max_rdma_size` 切分数据 | 从 Connect 的 private data 和 Identify 里读，不猜 |
| CX3 的 MR 大小上限 | **本轮实测修正**：namespace 512 MiB / region 517 MiB 的单个 MR 注册与传输都正常（F4 命令大小扫描）。旧记录"191 MB 可用、268 MB 挂死"更可能是当时的策略护栏，不是硬件上限 | 暂不设人为上限；大 MR 反而省掉了分块注册的复杂度 |
| 单机双端点 | 两个端点同主机，数据路径比裸 RDMA 多一层 capsule/Target 块层，吞吐必然低于 2.53 GB/s | 报告里给"占裸 RDMA 的百分比"，不假装它是网络存储的性能 |
| **远端 STag 失效（SGL subtype 0xf）我们做不到** | `IB_WR_SEND_WITH_INV` 需要"发应答的同时作废对端 STag"；`IND2QueuePair` 只有本地 `Invalidate()`，`SendAndInvalidate` 只在更新的 `IND` 接口里（`ndspi.h:970`） | **本轮修正**：不再拒绝（§8.40(2)）。照做传输 + 普通 SEND，因为 **Linux host 的完成路径在没有收到 invalidation 时会自己本地 `IB_WR_LOCAL_INV`**，不依赖我们。真正做不到的只是"代替 host 作废"，而 host 不需要我们代劳 |

