# D0 / D1：Windows 内核态 NVMe-oF（StorPort 微型端口）的准备

这份文档是**准备**，不是实现。它把"要不要写内核驱动、怎么写、第一步怎么证明它可能"整理成可以直接
开工的形态，并且把**本机已经确认的事实**和**还没确认的东西**分开写。

---

## 0. 一句话：D0 要回答什么

**内核态 RDMA 在这台机器上到底能不能用。** 用户态那套（NDSPI，`IND2Adapter`/`IND2QueuePair`/…）
与内核态那套（**NDKPI**）是**两套完全不同的接口**，我们这一整轮的代码全部建立在 NDSPI 上。D0 不成立，
D1（StorPort 驱动）整条路线就不成立，所以它必须排在所有内核工作之前，成本只有 2–4 人日。

## 1. 已确认的本机事实（今天查的，不是假设）

| 事实 | 值 |
|---|---|
| Windows Kits 10 Include/Lib | 存在 |
| SDK 版本 | `10.0.22621.0`、`10.0.26100.0` |
| **`ndkpi.h`** | `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km\ndkpi.h` ✅ |
| **`storport.h`** | `...\10.0.26100.0\km\storport.h` ✅ |
| VS | `F:\Microsoft Visual Studio\18\Community`（MSVC 14.44.35207 / 14.51.36231） |
| 测试签名状态 | **未确认**（`bcdedit` 需要管理员，本轮读不到） |
| RDMA 端点现状 | **坏的**（`stag_smoketest` 连两进程连接都建不起来，见 DESIGN §8.75(8)）—— D0 与它无关，NDKPI 探针不需要这对端点 |

**结论：工具链这一半是齐的。** 缺的那一半是"NDKPI 提供程序（WinOF 5.50）是否愿意让一个内核客户端
打开适配器" —— 这正是 D0 要测的。

## 2. 已经看过的 API 形状（只写看到的，不凭记忆补全）

在 `ndkpi.h` 里确实存在：

- `NDK_VERSION`（第 49 行附近，作为参数出现在第 155、872 行）；
- `NDK_CONNECTOR` / `NDK_LISTENER` 类型，以及 627–787 行一组以 `_In_ NDK_CONNECTOR *pNdkConnector`
  为参数的函数指针，其中包括 **`NdkConnect`**（787）与 **`NdkConnectWithSharedEndpoint`**（788）；
- `NdkListen`（846）；
- 位于 872–933 的**创建函数**：其中一个接收 `NDK_VERSION`，另有 `_Outptr_ NDK_CONNECTOR **ppNdkConnector`
  （921）与 `_Outptr_ NDK_LISTENER **ppNdkListener`（933）。

**开工第一步是把这个头文件读完并列出准确的函数名与调用顺序**（`NdkOpenAdapter` 之类的名字我**没有**
在这里确认过，所以不写进文档 —— 本项目对 wire/API 的规矩是"从文件读，不凭记忆写"）。
需要确认的是：适配器打开、完成队列创建、队列对创建、以及 **内核客户端如何发现提供程序**
（NDKPI 用的是 provider 注册/扩展接口的机制，这一条决定探针的骨架长什么样）。

## 3. D0 探针的规格（通过/不通过的判据必须可执行）

探针是一个**最小内核驱动**，只做一件事：证明它能通过 NDKPI 完成一次 **QP 建立**，能的话再完成一次
**RDMA Read**。

| 步骤 | 通过判据（可执行） |
|---|---|
| 0. 工具链 | 能产出一个 `.sys`，并且**能被加载与卸载**（`sc create` + `sc start` + `sc stop`，`DbgPrint` 输出可见） |
| 1. 打开适配器 | NDKPI 适配器对象被打开，返回 `STATUS_SUCCESS`；打印提供程序版本 |
| 2. 建 CQ + QP | 至少一个完成队列与一个队列对被创建 |
| 3. 建连 | 与用户态对端（或第二个内核端）完成一次 connect，或至少 `NdkListen` 成功并接受一个连接 |
| 4. 一次 RDMA Read | 从对端内存读回已知内容，逐字节比对一致 |

**判据 4 才是真正的答案**：只是"打开适配器成功"说明不了数据路径可用。
如果 1–3 成功而 4 失败，结论是"NDKPI 可用但我们的用法不对"；如果 1 就失败，结论是
"这条路在这张卡/这个提供程序上不成立"，D1 直接停止 —— 这正是 D0 存在的意义：**用 2–4 人日买一个
确定的是/否，而不是先写三个月的驱动再发现**。

## 4. 构建与加载（本机可用的路径）

**构建**：WDK 的 MSBuild 集成是最省事的路径（`msbuild /p:Configuration=Debug /p:Platform=x64 project.vcxproj`
配合 WDK 的 Visual Studio 扩展）。若那套集成没装全，退化路径是用 WDK 的 `Lib\10.0.26100.0\km\x64`
手工链接（`cl /kernel` + `link /DRIVER:WDM /SUBSYSTEM:NATIVE` + `ntoskrnl.lib`）—— **这两条哪条能用，
本身就是 D0 步骤 0 要报告的事实**。

**加载**：内核驱动需要签名。三种可行方案，按代价排序：

1. **测试签名**：`bcdedit /set testsigning on` + 自签证书（需要管理员，且会改变启动配置）；
2. **禁用完整性检查**：`bcdedit /set nointegritychecks on`（更弱的保证，不推荐作为长期方案）；
3. **不加载**：只在能访问的测试机上加载 —— 本机是用户日常使用的机器，**这一步必须先问用户**。

## 5. 风险（按"会不会让整条路不成立"排序）

| 风险 | 影响 | 现在能做的准备 |
|---|---|---|
| NDKPI 提供程序不注册 / 不支持内核客户端 | **D1 不成立** | D0 步骤 1，成本最低 |
| 需要测试签名并改启动配置 | 影响用户的机器 | 先问，不擅自动 `bcdedit` |
| StorPort 微型端口需要 WDK 的完整构建环境 | D1 的构建时间 | 用 D0 步骤 0 顺手验证工具链 |
| 内核态没有 NDSPI 的便利封装（内存注册、CQ 轮询都要自己写） | D1 工作量 | 先读 `ndkpi.h` 把对象模型列出来 |

## 6. 状态与下一步

- **本轮没有写任何驱动代码**：一份不能被编译、不能被加载的骨架，在这个仓库的规矩里不算成果
  （"写了代码"与"做完了"是两件事）。**准备做到"环境事实 + 探针规格 + 构建/加载路径 + 风险"为止。**
- **下一步（按顺序）**：① 读完 `ndkpi.h`，把准确的函数名与调用顺序列进这份文档；② 用 D0 步骤 0
  验证工具链能产出 `.sys`；③ 问用户是否允许测试签名/加载；④ 写探针并跑步骤 1–4。
- **与 RDMA 端点故障无关**：D0 可以在当前"用户态端点坏着"的状态下进行 —— 它测的是内核接口是否可用。

---

## 7. 本轮实测结果（2026-10）：D0 步骤 0 **已完成**

用户已禁用驱动强制签名，于是把步骤 0 做完了 —— 而且**两步都是实测，不是推断**。

### (1) 工具链能产出 `.sys`（不需要 MSBuild 集成）

`src/driver/ndprobe.c` 是一个最小内核驱动，里面**故意没有任何 RDMA**：先排除"工具链产不出东西"和
"内核不接受它"这两种失败，它们与"提供程序说不行"是完全不同的结论。

能用的命令（手工路径，写下来省得再试）：

```
cl /nologo /c /kernel /GS- /W4 /D_AMD64_ /I"<kits>\Include\10.0.26100.0\km" ^
   /I"<kits>\Include\10.0.26100.0\shared" /I"<kits>\Include\10.0.26100.0\ucrt" ndprobe.c
link /nologo /DRIVER:WDM /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64 ^
   /LIBPATH:"<kits>\Lib\10.0.26100.0\km\x64" ndprobe.obj ntoskrnl.lib hal.lib /OUT:ndprobe.sys
```

产物 `ndprobe.sys` = **3584 字节**。

**踩到的第一个坑值得记下来**：少了 `/D_AMD64_` 时，`shared\ntdef.h` 用
`#error "No Target Architecture"` 拒绝编译 —— `VsDevCmd -arch=x64` **不会**定义这个宏，它是 WDK
构建环境（或命令行）给的。

### (2) 内核接受未签名的驱动

```
sc create ndprobe type= kernel start= demand binPath= <...>\ndprobe.sys     -> SUCCESS
sc start  ndprobe                                                            -> STATE: 4 RUNNING
sc stop / sc delete                                                          -> 已清理
```

**一个内核驱动用 `sc create type= kernel` 就能加载，不需要 INF。**

### (3) 一个必须知道的注意事项：这个豁免很可能不是持久的

`bcdedit /enum {current}` 里**没有** `testsigning` 行，也没有 `nointegritychecks` —— 也就是说
禁用签名**没有写进 BCD**，很可能是开机时选的"禁用驱动程序强制签名"（F7），**只对当次启动有效**。
后果：**重启之后这次加载大概会失败**，报 `577`（镜像哈希无效）。

要让它在重启后仍然可用，二选一（都需要管理员 + 重启）：

```
bcdedit /set testsigning on          # 持久化测试签名（需要 Secure Boot 关闭 —— 本机已确认是关闭的）
```
或者每次开机走 F7。

### (4) 下一步：D0 步骤 1–4（NDKPI 本身）

步骤 0 只证明"这条工具链和这个加载路径能用"。真正的答案在步骤 1：**通过 NDKPI 打开一个 ND 适配器**。
那需要先把 `ndkpi.h` 读完并列出准确的函数名与调用顺序（本文件 §2 只写了**确认过**的几个，
`NdkOpenAdapter` 之类的名字尚未确认）。步骤 4 的判据仍然是一次 **RDMA Read 并逐字节比对** ——
"适配器打开成功"说明不了数据路径可用。

---

## 8. D0 步骤 1 实测：WSK 在，**NDK 扩展不在**（2026-10，未签名的驱动已能加载）

`src/driver/ndkprobe.c` 是真正的探针：它以 **WSK 客户端**身份注册，取到提供程序 NPI，然后用控制码
`WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH`（`((ULONG)'NDKD')`）去要 **NDK 扩展派发表**，拿到就
`WskOpenNdkAdapter`。结果写文件（不需要内核调试器）。

### 实测输出

```
WskRegister                        = 0x00000000
WskCaptureProviderNPI              = 0x00000000   client=FFFFD58E08558D70 dispatch=FFFFF8015D1181A0
WskControlClient('NDKD')           = 0xC000000D   ndk dispatch=0000000000000000 (0 bytes)
```

`0xC000000D` = **STATUS_INVALID_PARAMETER**。第一次跑时我把 `OutputSizeReturned` 传了 NULL，而
`wsk.h` 明确写着查询式调用要求它非空 —— 那是我自己的 bug，修掉之后**结果不变**，所以它不是参数问题。

### 这条路径是"从头文件读出来"的，不是猜的

| 头文件 | 提供什么 |
|---|---|
| `ndkpi.h` | **只有提供程序侧**：`NDK_FN_*` 派发表、`NDK_ADAPTER`/`NDK_CQ`/`NDK_QP`/`NDK_MR`/`NDK_SGE`。**没有任何客户端入口** —— 这就是为什么第一次找 `NdkOpenAdapter` 什么也没找到 |
| `ndisNDK.h` | 微型端口侧：`NDIS_NDK_PROVIDER_CHARACTERISTICS`，网卡驱动注册 `OpenNDKAdapterHandler` |
| **`wskndk.h`** | **客户端侧**：NDK 是 **WSK 的扩展** —— `WSK_PROVIDER_NDK_DISPATCH { WskOpenNdkAdapter, WskCloseNdkAdapter }`，控制码 `((ULONG)'NDKD')` |

### 构建这两个坑值得记（省下一次重复）

1. **WDK 里没有 `wsk.lib`。** `WskRegister`/`WskCaptureProviderNPI`/`WskReleaseProviderNPI`/`WskDeregister`
   的导入库是 **`netio.lib`**（符号由 `netio.sys` 导出；`ndis.sys` 不含）。在整个 Kits 目录里
   3550 个 `.lib` 中搜符号才找到这一条。
2. `RtlStringCbVPrintfA` 会落到 CRT 的 `__stdio_common_vsprintf`，在内核里链接不上 —— 定义
   **`NTSTRSAFE_LIB`** 并链接 `ntstrsafe.lib` 即可。
3. `sc delete` 一个**仍在运行**的驱动只把它标记为删除，**`.sys` 文件直到重启才会解锁**（再链接会
   `LNK1104`）。换个输出名即可绕开。

### 怎么读这个结果（**不要**读成"内核这条路死了"）

症状是"WSK 提供程序在，但它不认 `NDKD`"。有两种解释，而且第二种现在很可疑：

1. NDK 的 WSK 扩展在这个提供程序上确实不提供；
2. **ND/NDK 提供程序此刻是不健康的** —— 而这是已知事实：同一台机器的**用户态** RDMA 端点也是坏的
   （`stag_smoketest` 连两进程连接都建不起来，DESIGN §8.75(8)）。**两个症状可能同一个根因**：WinOF 的
   ND 提供程序状态不对，于是它既不接受用户态连接，也不向外提供 NDK 扩展。

**下一个实验就是区分这两者**：等 RDMA 提供程序恢复（重启，清掉我这一轮用强杀弄脏的状态）之后，
**立刻再跑一次这个探针**。如果 `NDKD` 那时回答了，两个症状同源，D0 的答案是"要提供程序健康才行"；
如果仍然 `0xC000000D`，那才是"内核这条路在这张卡上不成立"。

探针不需要重新编译：`sc create ndkprobe3 type= kernel binPath= <...>\ndkprobe2.sys` 即可重跑。


---

## 9. ★ D0 结论：**内核态 RDMA 在这台机器上能用**（2026-10，实测）

这一节推翻了我上一节的假设，并且给出了真正的答案。

### (1) 决定性实测：Windows 自带的内核态 NDK 客户端跑通了

排查中发现这台机器上装了 **`C:\Windows\System32\NDKPing.exe` 与 `NDKPerfCmd.exe`** —— 它们不是
Mellanox 的工具，而是 **Windows 自己的组件**（WinSxS 里是 `microsoft-windows-ndkping-setup` /
`microsoft-windows-ndkperf-setup` 包；驱动是 `NDKPing.sys` / `NDKPerf.sys`，服务可由 `sc start` 启动）。

用它做本机两口的**内核态** RDMA 往返（server 在 ifIndex 36 = 192.168.100.2，client 在 41 = .3）：

```
NDKPing.exe -S -ServerAddr 192.168.100.2:18515 -ServerIf 36 -TestType rping -W 25
NDKPing.exe -C -ServerAddr 192.168.100.2:18515 -ClientAddr 192.168.100.3 -ClientIf 41 -TestType rping -V
```

结果（客户端输出与日志）：

```
32768 bytes of data successfully transferred     (重复 13 次，全部成功)
NDKPing Client completes test (rping) on interface 41.
```

**这就是 D0 的答案：内核态 RDMA 可以工作。** D1（StorPort 微型端口）的立足点是成立的 —— 而且这个
结论不是我的代码"看起来对"，是**操作系统自己的内核客户端在同一对网卡上完成了数据传输**。

### (2) 撤回：上一节"提供程序不健康"的假设是错的

§8 里我写"用户态端点坏了，所以 ND/NDK 提供程序可能也不健康，两者同源"。**内核 RDMA 明明跑通了，
所以提供程序是健康的**，那个假设不成立。它同时说明：

- 我探针上的 `0xC000000D` **不是提供程序的问题**，是**我的调用约定不对**；
- 用户态端点坏掉（`stag_smoketest`）是**另一件事**，与内核路径无关 —— 这条线索本身值得追：
  同一对网卡、同一个提供程序，**内核能用而用户态不能用**。

### (3) 我的管线没错，错的是"取扩展"这一步

`dumpbin /imports NDKPing.sys` 显示它导入的 WSK 函数与我的探针**完全一致**，且都来自 `NETIO.SYS`：

```
WskRegister / WskCaptureProviderNPI / WskReleaseProviderNPI / WskDeregister
```

也就是说：注册与捕获提供程序这一步我做对了（也印证了导入库是 **`netio.lib`**，WDK 里没有 `wsk.lib`）。

差别只在**扩展调用**。用"按字节找立即数"（并用我自己的二进制做方法自检，确认搜索有效）验证：

| 二进制 | 含 `NDKD` 立即数 | 含 `0xC0000007` |
|---|---|---|
| `NDKPing.sys` | **0** | 0 |
| `NDKPerf.sys` | **0** | 0 |
| `NDKPing.exe` | **0** | 0 |
| `ndkprobe2.sys`（自检） | 1 ✅ 方法有效 | — |

**所以微软自己的客户端根本不使用 `WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH ('NDKD')` 这个控制码。**
`wskndk.h` 只给了这个宏，而它显然不是客户端取派发表的方式（`NPIID` 是 GUID，也与这个 ULONG 不匹配）。

### (4) 下一步（收窄到一个具体问题）

**问题**：`NDKPing.sys` 是怎么从 WSK 拿到 `NDK_ADAPTER` 的？

候选：(a) `WskControlClient(..., SIO_WSK_REGISTER_EXTENSION, WSK_EXTENSION_CONTROL_IN{NpiId=某个 GUID})`
—— 需要一个 WDK 里没有给出的 GUID；(b) 另一个 WSK 提供程序（而非默认的 tcpip 那个）。

**做法**：`dumpbin /disasm C:\Windows\System32\drivers\NDKPing.sys`，定位
`WskCaptureProviderNPI` 调用点之后那段代码，看它传给 `WskControlClient`（通过提供程序派发表间接调用）
的**立即数控制码**和 **GUID 指针**。这比继续猜要快，也比编一个 GUID 诚实。

**已经足够支持 D1 的判断**：内核态 RDMA 可用，所以剩下的问题是"我们的客户端怎么接上"，而不是"这条路
是否存在"。

---

## 10. ★ 找到 `0xC000000D` 的真正原因：**传出的是指针大小，而不是结构体大小**

上一轮我用"字节搜索 + 方法自检"确认了微软自己的内核 NDK 客户端**不使用** `'NDKD'` 立即数，据此推断
"`WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH` 不是客户端取派发表的方式"。**这个推断是错的** —— 搜索本身
没错，但它证明不了"没用这个宏"，因为宏的参数是 `sizeof` 和取地址，编译进二进制的是**结构体大小 16**
和一条 `lea`，而不是那四个字符。

真正的答案来自**微软自己的示例代码**（`NdkWrapper.c`，微软版权，公开在 `ccpgames/SDN` 仓库的
`NDKCI/RdmaSample/` 下）：

```c
static WSK_PROVIDER_NDK_DISPATCH WskNdkDispatch;      // ← 结构体本体
...
status = WskProviderNpi.Dispatch->WskControlClient(
    WskProviderNpi.Client,
    WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH,
    0, NULL,
    sizeof(WskNdkDispatch), &WskNdkDispatch,          // ← sizeof(结构体) = 16
    NULL, NULL);                                      // ← OutputSizeReturned 就是 NULL
```

对照我的实现：调用形状**完全一致**（连 `OutputSizeReturned = NULL` 都一样，我早先还专门"修"过这一处，
其实它本来是对的），唯一实质差别是我声明成**指针**、传 `sizeof(pointer)` = 8 字节。提供程序校验输出
缓冲区大小，太小就回 **`STATUS_INVALID_PARAMETER`**。改成结构体本体后……（见 §11 实测）

**方法论教训**（值得单独记）：字节搜索能证明"某个立即数存在"，**不能**证明"某个源码常量被使用"——
`sizeof(struct)` 和 `&struct` 在二进制里不留那四个字符。我据一个否定结论改了方向，代价是一轮。

## 11. 探针扩展：适配器信息 + PD/CQ/QP/Listener/Connector（步骤 1–2）

按示例的**异步转同步**形状补齐（每次 create 配一个 KEVENT + 完成回调；不等待的话驱动会"加载成功、
什么都不打印"，看起来和挂死一样）：`NdkQueryAdapterInfo`、`NdkCreatePd`、两个 `NdkCreateCq`、
`NdkCreateQp`、`NdkCreateListener`、`NdkCreateConnector`，并在卸载时逆序关闭。

---

## 12. ★ 探针蓝屏的根因：**我的驱动没有 CFG 插桩**（`0x139` 子码 10）

加载探针时机器**当天蓝屏四次**（14:18、17:04、19:02、19:13，转储在 `C:\Windows\Minidump`）。
崩溃码与参数（从 `Microsoft-Windows-WER-SystemErrorReporting` 事件读出）：

```
0x00000139 (0x000000000000000a, 0x0, 0x0, 0x0)
```

`0x139` = `KERNEL_SECURITY_CHECK_FAILURE`，**参数 1 = 10 = `FAST_FAIL_GUARD_ICALL_CHECK_FAILURE`**
—— **控制流保护（CFG）在一次间接调用上失败**。

### 证据（静态对照，不需要再加载任何东西）

```
                        DLL characteristics    Guard CF function count
我的 ndkprobe4.sys      0x2160 (无 CFG)          0        <- 完全没有函数表
微软 NDKPing.sys        0x4160 (有 CFG)          --       <- 能正常加载的那个
修复后 ndkprobe_l1.sys  有 CFG                   8
修复后 ndkprobe_l2.sys  有 CFG                   0xA
```

**原因**：我用手工 `cl /kernel` 编译，**丢掉了 WDK 驱动工程默认的 `/guard:cf`**。于是：
我的驱动把回调函数（`ProbeCreateCompletion`）交给提供程序，而提供程序（微软组件，**有** CFG）做起
间接调用时，内核的 CFG 校验发现**我的函数不在合法目标表里**（表为空）→ 立刻
`FAST_FAIL_GUARD_ICALL_CHECK_FAILURE` → 蓝屏。

这解释了为什么**任何把回调交给提供程序的探针版本都会崩**，也解释了为什么崩溃发生在"加载后几秒"
而不是加载瞬间。

**修复**：`cl /guard:cf` + `link /GUARD:CF`（WDK 工程默认就有，手工构建必须自己加）。
顺带修掉第二个潜在缺陷：`cleanup()` 里 close 调用曾传 `NULL` 完成回调，而 NDKPI 的签名是 `_In_`
—— 微软示例专门有个 `DoNothing` 回调就是为此，现已照做。

**新增分级开关**：`PROBE_LEVEL=1` 只做"注册 + 取 NDK 派发表"（不创建对象、不传回调），
`PROBE_LEVEL=2` 才做适配器查询与对象创建。先跑 1 再跑 2 —— 崩溃的代价是整机重启，不应该用
最复杂的那一级去试探。

**教训（写下来）**：手工 cl/link 构建内核驱动，会**静默丢掉 WDK 工程默认的一整套开关**
（CFG、`/GS`、`/hotpatch`、`/guard:cf` 的链接标志……）。丢掉的那些**不会报错**，只会让驱动在
别人（这里是提供程序）回调你的时候把整机打蓝。以后手工构建的驱动必须显式列出这些开关。

---

## 13. ★★★ D0 步骤 1 与步骤 2 **全部通过**（2026-10-03，实测，零失败）

修好 CFG、并用测试证书给驱动签名之后（测试模式**要求驱动有签名**；F7 才允许完全未签名 —— 这两者
的区别让 `577` 又出现了一次），探针一次跑通：

```
WskRegister                    = 0x00000000
WskCaptureProviderNPI          = 0x00000000   client=FFFFCD89BDAF5CD0
WskControlClient('NDKD')       = 0x00000000   Open=FFFFF8044578FB80  Close=FFFFF8044578FB30
WskOpenNdkAdapter(v2.0, if36)  = 0x00000000   adapter=FFFFCD89BB9EA200

NdkQueryAdapterInfo            = 0x00000000  (104 bytes of 104)
  provider 2.0  vendor 0x02C9 device 0x1007
  MaxRegistrationSize=17592186044415 MiB  MaxWindowSize=0 MiB  MaxTransferLength=1048576 KiB
  MaxInboundReadLimit=16  MaxOutboundReadLimit=128  FRMRPageCount=511
  MaxReadRequestSge=32  MaxInitiatorRequestSge=32  MaxReceiveRequestSge=32
  MaxCqDepth=4194303  MaxInitiatorQueueDepth=16351  MaxReceiveQueueDepth=16351

NdkCreatePd                    = 0x00000000  pd=FFFFCD89B3D6B210
NdkCreateCq (receive, depth 64) = 0x00000000  cq=FFFFCD89708F3EC0
NdkCreateCq (send, depth 64)    = 0x00000000  cq=FFFFCD89B4927520
NdkCreateQp (16 recv / 16 send) = 0x00000000  qp=FFFFCD89B7FE38E0
NdkCreateListener              = 0x00000000  listener=FFFFCD89B4358C40
NdkCreateConnector             = 0x00000000  connector=FFFFCD89B3D48D20
```

**这是本项目第一次由我们自己的内核驱动打开 ND 适配器并建立 NDK 对象。** 三个修正是必要条件，缺一
不可，而每一个都有独立证据：

1. **`sizeof(结构体)` 而不是 `sizeof(指针)`**（§10）—— 否则 `WskControlClient('NDKD')` 回
   `STATUS_INVALID_PARAMETER`；
2. **`/guard:cf`**（§12）—— 否则提供程序回调我们时，CFG 函数表为空 → 整机 `0x139` 蓝屏；
3. **测试证书签名**（本节）—— 测试模式**要求有签名**，未签名只在 F7 启动下放行。

适配器能力与我们**用户态**测到的完全一致（MaxTransferLength 1 MiB、读限制 16/128、SGE 32），
这从内核侧交叉验证了用户态那套代码一直在读同样的东西。

**一个已知的小问题**：`sc stop` 没有让驱动停下（查询仍为 RUNNING），说明 `DriverUnload` 没被调用
—— 探针在 DriverEntry 之后不再做任何事，所以无害，重启即清。D1 里必须解决（StorPort 驱动要能停）。

---

## 14. 蓝屏闭环：**转储证据**（`kd`/`cdb`，WDK 自带）

用 WDK 的 `cdb.exe` 分析 `C:\Windows\Minidump\100326-20671-01.dmp`（19:13 那次崩溃），
把 §12 的推断变成了直接证据：

```
BUGCHECK_CODE:  139
BUGCHECK_P1:    a
Arg1: 000000000000000a, Indirect call guard check detected invalid control transfer.
SYMBOL_NAME:    mlx4eth63+785f7
IMAGE_NAME:     mlx4eth63.sys
FAILURE_BUCKET_ID: 0x139_a_GUARD_ICALL_CHECK_FAILURE_mlx4eth63!unknown_function

STACK_TEXT:
  nt!guard_icall_bugcheck+0x1e
  tcpip!ndkpiCloseCompletionCore+0x7e
  tcpip!NdkpiCloseCompletion+0xb
  tcpip!NdkpiBind+0x11f
```

**读法**：WSK/NDK 的公共层在 `tcpip.sys`（`NdkpiCloseCompletion`），它回头调用**我提供的完成回调**，
而最终那次间接调用的目标落在 `mlx4eth63.sys`（Mellanox 的 ND 提供程序）—— 内核对这个目标做 CFG
校验，发现**不在合法函数表里**，于是 `guard_icall_bugcheck` 直接 fast-fail 整机。

**两个修正是同一处的两面**，缺一不可：

1. **`/guard:cf`** —— 让我的函数进入 CFG 函数表（0 项 → 8/0xA 项），这样别人回调我才合法；
2. **close 调用传真正的空回调而不是 `NULL`** —— 栈上正是 close 完成路径，`NULL` 也不是合法目标。

**修复后的验证**（比"没再崩"更强）：同一个驱动、同一批调用（含全部 create/close），在 19:35 之后
连续两次加载**全部成功、零转储**。此外转储列表里**最后一次崩溃停在 19:13**，即修复之前。

**方法论**：这台机器上就装着 WDK 的调试器（`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`），
一条命令就能把"蓝屏码 + 栈 + 出错模块"读出来。以后内核工作出问题，**先读转储，再猜**。

---

## 15. 步骤 3–4 的实现清单（NDKPI 接口面已从 `ndkpi.h` 读出，非记忆）

要调用的成员（结构体 → 成员，逐个从头文件确认）：

| 结构体 | 成员 |
|---|---|
| `NDK_ADAPTER_DISPATCH` | `NdkQueryAdapterInfo` `NdkCreateCq` `NdkCreatePd` `NdkCreateConnector` `NdkCreateListener` `NdkBuildLAM` `NdkReleaseLAM` |
| `NDK_PD_DISPATCH` | `NdkCreateMr` `NdkCreateMw` `NdkCreateQp` `NdkGetPrivilegedMemoryRegionToken` |
| `NDK_CQ_DISPATCH` | `NdkGetCqResults` `NdkGetCqResultsEx` `NdkArmCq` |
| `NDK_QP_DISPATCH` | `NdkSend` `NdkReceive` `NdkRead` `NdkWrite` `NdkInvalidate` `NdkFastRegister` `NdkFlush` `NdkBind` |
| `NDK_MR_DISPATCH` | `NdkRegisterMr` `NdkDeregisterMr` `NdkGetRemoteTokenFromMr` `NdkGetLocalTokenFromMr` `NdkInitializeFastRegisterMr` |
| `NDK_CONNECTOR_DISPATCH` | `NdkConnect` `NdkCompleteConnect` `NdkAccept` `NdkReject` `NdkGetConnectionData` `NdkGetLocalAddress` `NdkGetPeerAddress` `NdkDisconnect` |
| `NDK_LISTENER_DISPATCH` | `NdkListen` `NdkGetLocalAddress` `NdkControlConnectEvents` |

调用形状（来自微软示例 `NdkWrapper.c`，逐字对照过）：

```c
NdkCreateListener(Adapter, ConnectEventHandler, Context, CreateCompletion, &ctx, &Listener);
NdkListen(Listener, Address, AddressCbLength, Completion, &ctx);
NdkConnect(Connector, Qp, LocalAddr, LocalLen, RemoteAddr, RemoteLen,
           InboundReadLimit, OutboundReadLimit, PrivateData, PrivateDataLen, Completion, &ctx);
NdkCompleteConnect(Connector, DisconnectEventCallback, Socket, Completion, &ctx);
NdkAccept(Connector, Qp, InboundReadLimit, OutboundReadLimit,
          PrivateData, PrivateDataLen, DisconnectEventCallback, Socket, Completion, &ctx);
NdkGetConnectionData(Connector, &InboundReadLimit, &OutboundReadLimit, NULL, &PrivateDataLen);
NdkBuildLAM(Adapter, Mdl, BytesToMap, Completion, Context, Lam, &LamCbSize, &Fbo);
NdkRead(Qp, Request, Sgl, SgeCount, RemoteAddress, RemoteToken, Flags);
NdkGetCqResults(Cq, Results, MaxResults);
```

**步骤 3–4 的实现顺序**（同一驱动内自建两端：.2 做 listener，.3 做 connector）：

1. 分配非分页内存 → 建 MDL → `NdkBuildLAM` → `NdkCreateMr` + `NdkRegisterMr`（或 FRMR + `NdkFastRegister`）
   → `NdkGetRemoteTokenFromMr` 取 rkey，把 **buffer 地址 + rkey** 塞进私有数据；
2. .2 侧 `NdkListen`，连接事件回调里 `NdkGetConnectionData` 取对端私有数据 → `NdkAccept`；
3. .3 侧 `NdkConnect`（带自己的地址/rkey）→ 对端 accept 后 `NdkCompleteConnect`；
4. .2 往 buffer 写一个已知模式 → .3 `NdkRead` → 轮询 CQ → **逐字节比对**；
5. 两端对象按逆序关闭（全部用真实回调，不用 NULL —— §14）。

**仍然待读的确切签名**（下一步第一件事，不猜）：`NdkRegisterMr`、`NDK_FN_CONNECT_EVENT_CALLBACK`、
`NDK_SGE` 的字段名、`NDK_RESULT` 的字段名。

---

## 16. 步骤 3 的第一次尝试：`0x7E` 蓝屏，以及它的根因（**异步完成上下文放在栈上**）

步骤 3 的驱动（`ndkstep3.c`）编译链接签名一次通过，第一次加载也**没有立刻崩**，跑到了：

```
server if36 / client if41: 两个适配器都打开、PD/CQ/QP/Connector/Listener 全部 0x00000000
server: NdkListen                      = 0xC0000141
```

`0xC0000141` 是**我自己拼错 IP**：192.168.100.2 应为 `0xC0A86402`（192=0xC0、168=0xA8、100=0x64），
我写成 `0x0A64C602`，那是 **10.100.198.2** —— 一个不存在的地址。用户态代码用 `InetPton` 所以从未踩到；
内核里手拼 sockaddr 就错了。**已修，并把实际请求的地址写进日志**，让下一个同类错误只需一眼。

改对 IP 之后的那次加载**蓝屏**了：

```
BUGCHECK_CODE: 0x7e (SYSTEM_THREAD_EXCEPTION_NOT_HANDLED)
BUGCHECK_P1:   0xFFFFFFFFC0000005 (access violation)
ExceptionAddress: nt!ExpWorkerThread+0x2d6
栈: nt!ExpWorkerThread <- nt!PspSystemStartup <- nt!KiStartSystemThread
FAILURE_BUCKET_ID: AV_nt!KiStartSystemThread
```

**访问违例发生在系统工作线程里** —— 正是我为规避 IRQL 而排的那个接受连接的 work item。

**根因（已从签名确认不是别的原因）**：`NdkGetConnectionData` 的第 4 个参数确实是缓冲区、
`DisconnectEvent` 确实是 `_In_opt_`（传 NULL 合法），所以问题在别处：

> **我把异步操作的"完成上下文"放在了栈上。**
> `NdkListen` / `NdkConnect` / `NdkCompleteConnect` / `NdkAccept` 都可能返回 `STATUS_PENDING` 并在
> **之后**调用完成回调。而这个上下文原本是 `STEP3_ASYNC c;`（**栈变量**），于是当完成晚于所在函数
> 返回时，提供程序往**已经失效的栈地址**写 → 内存被破坏 → 工作线程里访问违例 → 整机 `0x7E`。

**为什么 L1/L2 用同样的写法却没崩**：那两级的 create 恰好**同步完成**（返回 `STATUS_SUCCESS`），
没有"迟到"的写入。**"it worked so far" 在这里恰恰是最危险的信号。**

**修复**：四处完成上下文全部改为 **static**（`STEP3_ASYNC c;` → `static STEP3_ASYNC c;`），
并把这个理由写在代码旁边。重新构建签名：`ndkstep3c.sys`。

**另一条更普遍的教训**：内核里凡是把指针交给"之后会被调用"的东西（完成回调、DPC、work item、
定时器），**它的生命周期必须长于那个调用**，而栈上的东西**永远不满足**这个条件。

---

## 17. 步骤 3 的第二次尝试（栈修复后）：**骨架打通，三个缺陷待修**

`ndkstep3c.sys`（完成上下文改静态后）加载、运行、**没有蓝屏**，输出：

```
server: NdkListen            = 0x00000000                                  <- 监听成功
client: NdkConnect           = 0x00000000                                  <- 连接请求发出
server: NdkGetConnectionData = 0xC0000023  peerIn=1 peerOut=1 privLen=56   <- 连接事件真的触发
server: peer private data    = magic=0x4E564D46 rkey=0x00000000 addr=0x0
server: NdkAccept            = 0xC00000B5                                  <- 接受失败
client: NdkCompleteConnect   = 0x00000000
```

**已经打通的**：`.2` 上 `NdkListen` 成功 → `.3` 的 `NdkConnect` 成功 → **服务端的连接事件回调真的
被调用**（我为了规避 IRQL 排的 work item 跑起来了）→ 私有数据交换路径走通。**步骤 3 的骨架是通的。**

**三个缺陷，都已定位**：

| # | 现象 | 诊断 |
|---|---|---|
| 1 | `NdkGetConnectionData` = `0xC0000023`（`STATUS_BUFFER_TOO_SMALL`），返回的 `privLen` = **56**，而我传的缓冲区是 16 字节 | 私有数据没读出来（打印出的 rkey/addr 全是 0 就是因为这个）。签名是 `_Out_writes_bytes_to_opt_(*pPrivateDataLength, *pPrivateDataLength)` + `_Inout_ ULONG*`：**输入是缓冲区容量、输出是实际长度**，容量不够就回这个状态。修法：给足容量（例如 256 字节） |
| 2 | `NdkAccept` = `0xC00000B5`（`STATUS_INVALID_DEVICE_REQUEST`） | 接受失败。要在修好 #1 之后重测 —— 也可能与"在 work item 里而非回调上下文里 accept"有关，需要实验区分，不能猜 |
| 3 | **`ANSWER:` 行宣布"两端完成连接"，而服务端的 accept 明明失败了** | 我的成功判据只检查了客户端 `NdkCompleteConnect` 的状态。**这是一个"不会失败的检查"** —— 本项目反复强调这条规矩，我自己在这里犯了 |

**下一轮的顺序**：① 私有数据缓冲区给足并重测 → ② 若 accept 仍失败，用实验区分"上下文"与"次序"两个
假设（例如：直接在回调里 accept 对比在 work item 里 accept）→ ③ **修好判据**（两边都成功才算成功）
→ ④ 再做步骤 4（一次 RDMA Read 逐字节比对）。

---

## 18. 步骤 3 的第三次到第五次尝试：**accept 的完成始终不到**，以及由此暴露的两个自造问题

### (1) 一个价值很高的更正：`0xC00000B5` 是**我自己的哨兵值**

前三轮我把 `NdkAccept = 0xC00000B5` 当成提供程序返回的错误在查。它其实是 **`STATUS_IO_TIMEOUT`** ——
**本文件在 15 秒等待超时时自己赋的值**。提供程序从未返回任何错误。

> 教训：**不要把"看起来像状态码"的常量用作内部哨兵**，并且要把哨兵与提供程序状态**分成两个字段打印**。
> 这一个混淆吃掉了三轮。

### (2) 已排除的次序假设（每种都实测过）

| 次序 | 结果 |
|---|---|
| 在连接事件回调里 `NdkAccept`（合法：`_IRQL_requires_max_(DISPATCH_LEVEL)`） | `STATUS_PENDING`，完成永不到 |
| 先等 accept 完成、再让客户端 `CompleteConnect` | **我自己造成的死锁**（互相等） |
| 客户端先 `CompleteConnect`，再由主线程在 `PASSIVE_LEVEL` accept（等 30 秒 + 完成计数） | 仍 `STATUS_PENDING`，完成永不到，且私有数据始终 0 字节 |

**已经确认没问题的部分**：监听武装 ✓、连接请求送达 ✓、**连接事件回调真的被调用** ✓、客户端侧
`NdkConnect` + `NdkCompleteConnect` 都成功 ✓。**唯一缺的是服务端 accept 的完成。**

### (3) 两个自造问题（都记下来，避免下次再犯）

1. **`sc stop` 停不掉这类 legacy 内核服务**（`1052 ERROR_INVALID_SERVICE_CONTROL`），**只能重启清**。
   一下午累积了 **7 个驻留实例**，各自占着适配器/监听器/PD/CQ/QP。之后某次运行出现
   "客户端 `NdkConnect` 被拒 **而** 服务端回调却触发了"这种自相矛盾的一对结果 —— 那正是**拥塞的栈**
   的样子。**迭代的前提是每次运行不留残留**，这本来就是 D1 必须解决的问题。
2. **"跑完释放"不能放在 `DriverEntry` 里**：我加了 `Step3Cleanup()` 到 `DriverEntry` 末尾，结果
   `WskDeregister`/`WskReleaseProviderNPI` **会等待未完成的 NDK 操作**，而那个 accept 永不完成 →
   `DriverEntry` 死等 → 服务卡在 `START_PENDING`。**释放要放在 `DriverUnload`（或一个能被打断的线程里）**，
   不能挂在一条永不完成的异步操作后面。

### (4) 下一步（明确）

1. **重启一次**清掉 7 个驻留实例（测试模式是持久的，所以重启不额外花操作）；
2. 把 `Step3Cleanup()` 从 `DriverEntry` 移回 `DriverUnload`；
3. **照微软示例逐项对齐对象创建**：示例里 CQ 是用**非 NULL 通知回调**创建的，QP 的深度/SGE 参数也不同
   —— 目前我全用 NULL 通知回调，这对 accept 完成是否有影响**必须用实验回答**，而不是推理；
4. 若对齐示例后 accept 仍不完成，则问题在提供程序（`mlx4eth63`）一侧，届时应把它记录为**该卡/photon
   驱动的限制**，并据此调整 D1 的路线（例如改用 `NdkConnect` 主动连接 + 内核侧不依赖 accept 的拓扑）。

---

## 19. ★ 步骤 3 的阻塞点找到依据：**NDKPI 的端点是按 (本地地址, 端口) 引用计数的**

微软官方文档
[NDKPI Listeners, Connectors, and Endpoints](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/ndkpi-listeners--connectors--and-endpoints)
里有一句直接命中今天全部症状的话：

> "Simply closing the listener does not release the endpoint as long as there are previously accepted
> connectors that are not yet closed. **This means that new NdkListen, NdkConnect and
> NdkConnectWithSharedEndpoint requests for the same local address and port will fail until all such
> connections are closed.**"

**读法**：端点是**按 (本地地址, 端口) 做引用计数**的；只要还有**未关闭的连接**（包括列表器接受过的
connector），同一本地地址+端口上的**新 `NdkListen` / `NdkConnect` 一律失败**。而失败状态正是我反复
见到的 **`0xC0000236` = `STATUS_CONNECTION_REFUSED`**。

**这与今天的内核侧现实完全吻合**：我加载了 **8 个驱动实例**（`ndkstep3*`），几乎每一个都创建过
connector，而**没有一个关闭过**（尤其那个卡在 `START_PENDING` 的实例，它的 connector 和挂起的
accept 永远没被清理）。也就是说 —— **我用自己的残留把自己越锁越死**，而且这一次是在内核侧、`sc stop`
又清不掉。

这与上午用户态那条教训是**同一类错误**：上午是"强杀 target 污染用户态 RDMA 栈"，现在是"未关闭的
connector 占住端点，让后续 connect 全被拒"。**凡是"用一次就占住资源"的东西，测试框架必须保证释放**，
否则后面测到的是自己的脚印。

### 下一步（明确、只差一次干净启动）

1. **重启**（清掉 8 个实例持有的全部端点）；
2. **干净启动后只跑一次** `ndkstep3`（新端口），观察 `NdkConnect`：
   - 若**成功** → 结论是"端点残留导致"，随后要立刻验证 accept 与私有数据（步骤 3 完成）；
   - 若**仍被拒** → 那就不是端点残留，下一步对比 `NDKPing.sys`（已知可工作的内核客户端）的
     `NdkConnect` 调用，必要时反汇编它的调用点看确切参数。
3. 无论哪种结果，**驱动必须做到"跑完即释放"**（已在源码里：清理放在 `DriverUnload` + 成功后主动
   close connector），并且 D1 必须让服务可停 —— 否则每轮实验都要重启一次。

---

## 20. 干净关机重启之后：**排除"端点残留"，本地端口 0 也被拒**

完全关机再开机（0 个驻留实例、驱动 5.2 秒加载成功、`testsigning Yes`）之后，干净机器上的**第一次**
运行结果：

```
server: NdkListen            = 0x00000000
server[callback]: ...        = 0x00000000  peerIn=1 peerOut=1 privLen=0     <- 服务端事件仍触发
client: NdkConnect           = 0xC0000236  (STATUS_CONNECTION_REFUSED)      <- 客户端仍被拒
```

**因此 §19 那个"端点残留"的解释被推翻** —— 它在文档里有依据、也与当天的现象吻合，但干净机器上依然
被拒，说明它不是根因。

随后把**本地端口设为 0**（让提供程序分配隐式端点，NDKPI 文档说 `NdkConnect` 建立的连接拥有"自己的
**隐式**本地端点"）—— **仍被拒**。

### 已经用实测排除的全部变量

| 变量 | 取值 |
|---|---|
| 本地端口 | 与服务端相同 / 服务端+1 / **0** |
| CQ 布局 | 单 CQ 共用 / receive+send 两个独立 CQ |
| CQ 通知回调 | NULL / 真实回调（签名是 `_In_`，微软示例传真实回调） |
| accept 次序（服务端） | 回调内 / 客户端完成前 / 客户端完成后 |
| 端点残留 | **完全关机重启、0 实例**后仍复现 |
| 签名 · CFG · 测试模式 | 逐项验证正常 |
| 内核客户端 → 用户态监听端 | 请求**完全不到达** |
| 内核客户端 → 内核监听端 | **服务端事件触发，客户端被拒** |

### 唯一未解释的矛盾与下一步

**服务端的连接事件会触发，而同一时刻客户端报 `CONNECTION_REFUSED`** —— 这一对事实无法用"请求没送到"
解释，也无法用"服务端没收"解释。它指向 `NdkConnect` 的某个参数或前置条件不满足。

**下一步（读，不再猜）**：反汇编 `C:\Windows\System32\drivers\NDKPing.sys` 中 `NdkConnect` 的调用点 ——
它是**这张卡上、这台机器上唯一已知能工作的内核态 NDK 客户端**（在两端口间完成过 32768 字节 × 13 次
传输）。逐个参数对照它的实际取值，而不是继续穷举我的猜测。这与当初解决 `0xC000000D` 的方法相同
（那次是读微软示例代码，立刻定位到 `sizeof(结构体)` vs `sizeof(指针)`）。

### 参照物的价值

用户态那套能工作的代码里，客户端在 `Connect` 之前有一个**显式的 `IND2Connector::Bind`** 步骤；而
NDKPI **没有对应的调用**（文档称端点是"隐式的"）。这个结构差异是当前最可疑的地方，也正是要从
`NDKPing.sys` 里核对的东西。

---

## 21. ★★★ D0 步骤 3 **成功**：两端连接建立（2026-10-03/04）

```
server: NdkListen                          = 0x00000000
server[callback]: NdkGetConnectionData      = 0x00000000  peerIn=1 peerOut=1 privLen=0
server[callback]: NdkAccept posted         = 0x00000103      (STATUS_PENDING)
client: NdkConnect                         = 0x00000000      <- 连上了
client: NdkCompleteConnect                 = 0x00000000
server: accept wait                        = 0x00000000      <- accept 完成
verdict: client connect=0x00000000 completeConnect=0x00000000  server accept=0x00000000
ANSWER: BOTH sides completed the connection
```

**这是本项目第一次由我们自己的内核驱动完成一次 NDK 连接的两端建立。**

### 根因：**accept 必须与客户端挂起的 connect 并发**（我自己造成的次序死锁）

| 版本 | 服务端 accept 投递位置 | 客户端 `NdkConnect` |
|---|---|---|
| A | 连接事件回调里 | **`0x00000000` 成功** |
| B | 客户端 connect 返回之后（主线程） | **`0xC0000236` 被拒** |

B 是死锁：客户端等对端 accept，而 accept 被安排在客户端 connect 返回之后才投递 —— 于是永远等不到，
CM 超时后回 `STATUS_CONNECTION_REFUSED`。**今天所有"客户端被拒"都是这一个次序问题**，而我在中途
还因为"回调里 accept 也返回 PENDING"而把这条正确的路放弃了 —— 那次 PENDING 是**正常的异步语义**
（异步操作返回 PENDING 后再由完成回调通知），不是失败。**把"PENDING"当成"失败"是这一轮最大的坑。**

### 使这次成功的完整配置（缺一不可的已验证项）

1. accept **在连接事件回调里**投递（`NdkAccept` 声明为 `_IRQL_requires_max_(DISPATCH_LEVEL)`，合法）；
2. 客户端 `NdkConnect` → 成功后 `NdkCompleteConnect`；
3. 主线程等待 accept 的**完成事件**（30 秒超时 + 完成计数）；
4. 每个端点 **两个独立 CQ**（receive / initiator），并带**通知回调**（签名是 `_In_`）；
5. `/guard:cf` + `/GUARD:CF`（CFG 函数表非空）；
6. 测试证书签名（测试模式要求"有签名"）；
7. **每次运行用唯一的 `.sys` 文件名** —— 同一文件不能加载第二次（否则 `sc start` 回一个误导性的
   "找不到文件" exit 2）；
8. 异步完成上下文一律 **static**，绝不放栈上。

### 遗留一项（步骤 4 需要）

`privLen=0`：**私有数据仍未到达**。而步骤 4 的 RDMA Read 恰恰需要通过对端私有数据交换 **buffer 地址
与 rkey**，所以这一项必须先解决。用户态代码的注释给了明确的时机要求：

> "The peer's private data must be read HERE, **between GetConnectionRequest and Accept**. Reading it
> after Accept yields an empty buffer with no error."

我在回调里（= Accept 之前）读，返回成功但长度为 0 —— 下一步是核对**客户端发送侧**：`NdkConnect` 的
`pPrivateData`/`PrivateDataLength` 是否被提供程序接受（长度 16 是否低于某个下限，或是否需要先
`NdkGetLocalAddress` 之类的准备）。

---

## 22. 第六次蓝屏 `0xCE`：**不是我方**（`NDKPing.sys`），以及私有数据的关键线索

### (1) 转储归属：`NDKPing.sys`

```
BUGCHECK_CODE:     ce
IMAGE_NAME:        NDKPing.sys
MODULE_NAME:       NDKPing
FAILURE_BUCKET_ID: 0xCE_IMAGE_NDKPing.sys
```

`0xCE` = `DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS`（驱动带着未完成操作被卸载）。
**出错的是微软自己的 NDKPing 驱动** —— 我为了停止测试用 `Stop-Process -Force` 强杀过 `NDKPing.exe`
两次，它的驱动在进程消失后带着挂起的操作卸载，于是蓝屏。

> **教训（与上午同源，代价更高）**：**不要强杀持有内核态 RDMA 资源的进程**。上午强杀用户态 target
> 污染了 RDMA 栈；这次强杀 NDKPing 直接把机器打蓝。要停它就用它自己的退出方式，或者等它跑完。

同样要记下的是：**今天 6 次蓝屏里 5 次是我方**（CFG 未插桩 ×4、异步上下文放栈上 ×1），**1 次是
Mellanox（`mlx4_bus.sys` 的 `0x9F` 电源状态失败），1 次是微软（`NDKPing.sys` 的 `0xCE`）**。区分归属
很重要：既不能把自己的问题当成环境的，也不能把环境的算成自己的。

### (2) 私有数据：长度查询回 56，真正读取回 0

同一次运行（步骤 3 依然全部成功，作为回归证据）：

```
server[callback]: size query               = 0x00000000  need=56
server[callback]: NdkGetConnectionData      = 0x00000000  peerIn=1 peerOut=1 privLen=0
verdict: client connect=0x00000000 completeConnect=0x00000000  server accept=0x00000000
```

**问题被收窄为**：先做长度查询时提供程序说需要 **56** 字节，而带着 256 字节缓冲区去真正读取时，
`*pPrivateDataLength` 回 **0** 且状态是成功。

**下一步假设（可直接测）**：该提供程序要求第二次调用时把 `*pPrivateDataLength` **设成它announced的
那个值**（56），而不是调用方缓冲区的大小。若这条成立，私有数据就能取到，**步骤 4 需要的 rkey/地址
交换通道即打通**。

（另需注意：56 恰好等于适配器信息里的 `MaxCallerData`，所以也可能是"提供程序支持的上限"而非"对端
实际发送长度"。两种解释对应不同的修法，**用一个明确长度的发送侧测试即可区分**：客户端发 24 字节、
服务端若收到 24 则说明是实际长度，若仍是 56 则是上限。）

---

## 23. ★★★ D0 步骤 3 **完整通过**：连接 + accept + **私有数据（rkey/地址）交换**

```
client: sending 24 bytes of private data (marker 0x3C3C3C3C)
server[callback]: size query          = 0x00000000  need=56
server[callback]: fetch(announced=56) = 0x00000000  got=56
        magic=0x4E564D46   rkey=0xDEADBEEF   addr=0x1122334455667788   marker=0x3C3C3C3C
verdict: client connect=0x00000000 completeConnect=0x00000000  server accept=0x00000000
```

**四个字段全部与发送端一致。** 这就是步骤 4（一次 RDMA Read）所必需的 **rkey + 缓冲区地址** 交换通道，
现在它被证明可用。

### ★ 私有数据的准确调用约定（**必须记录，否则会静默丢数据**）

| 第二次 `NdkGetConnectionData` 的 `*pPrivateDataLength` 入参 | 结果 |
|---|---|
| **256**（调用方缓冲区大小） | `STATUS_SUCCESS` 但 `len=0`、**数据为空**（**静默失败**，最难查的一类） |
| **56**（第一次查询 announced 的值） | `STATUS_SUCCESS`、`len=56`、**数据完整** |

正确流程（照用户态 `GetPrivateData(NULL,&len)` 再做第二次调用的做法）：

```c
ULONG need = 0;
NdkGetConnectionData(conn, NULL, NULL, NULL, &need);          // 第一次：只查长度，得到 need=56
ULONG got = need;                                             // 第二次：把 need 作为长度入参传入
NdkGetConnectionData(conn, &inLimit, &outLimit, buf, &got);   // 数据在这里
```

且读的时机必须是**连接事件回调里（Accept 之前）**；Accept 之后再读拿到的是空缓冲区（用户态注释亦如此
记载）。

### 本次修掉的真实缺陷：覆盖赋值

上一次运行数据"部分到达"（magic/marker 对、rkey/addr 为 0）不是提供程序的问题，而是**本文件里后写的
两行 `g_clientPriv.Rkey = 0; g_clientPriv.Address = 0;`**（早期还没有 MR 时的占位）把先写的值覆盖掉了。
**教训**：占位赋值散落在赋值语句之间时，会产生"看起来像提供程序丢字段"的现象 —— 读回日志里的
**每一个字段**才定位到。

### D0 进度

| 步骤 | 状态 |
|---|---|
| 1 打开适配器 | ✅ |
| 2 建对象（PD/CQ/QP/Listener/Connector） | ✅ |
| 3 两端连接 + 私有数据交换 | ✅ **本轮完成** |
| 4 一次 RDMA Read 并逐字节比对 | ⬜ 下一步 |

---

## 24. 步骤 4 的实现计划 + 已核实的接口签名（**从内核头文件读出，非猜测**）

### 已核实的签名（`ndkpi.h`，10.0.26100.0）

```c
// 1) 建 MR 对象
NTSTATUS (*NDK_FN_CREATE_MR)(NDK_PD *pNdkPd, BOOLEAN FastRegister,
                             NDK_FN_CREATE_COMPLETION CreateCompletion,
                             PVOID RequestContext, NDK_MR **ppNdkMr);

// 2) 把内存注册进 MR —— 注意是 MDL，且必须描述"虚拟连续"的内存
NTSTATUS (*NDK_FN_REGISTER_MR)(NDK_MR *pNdkMr, MDL *Mdl, SIZE_T Length, ULONG Flags,
                               NDK_FN_REQUEST_COMPLETION RequestCompletion, PVOID RequestContext);

// 3) 取远端 token（rkey）—— 通过 MR 的派发表
UINT32 (*NDK_FN_GET_REMOTE_TOKEN_FROM_MR)(NDK_MR *pNdkMr);
//    MR 派发表成员顺序：NdkCloseMr, NdkQueryExtension, NdkRegisterMr, NdkDeregisterMr,
//                       NdkInitializeFastRegisterMr, NdkGetRemoteTokenFromMr, NdkGetLocalTokenFromMr

// 4) 发起 RDMA Read —— 语义：把远端内存读到本地 SGE 描述的内存
NTSTATUS (*NDK_FN_READ)(NDK_QP *pNdkQp, PVOID RequestContext, CONST NDK_SGE* pSgl, ULONG nSge,
                        UINT64 RemoteAddress, UINT32 RemoteToken, ULONG Flags);

// 5) 收割完成（轮询 CQ）
NTSTATUS (*NDK_FN_GET_CQ_RESULTS)(NDK_CQ *pNdkCq, NDK_RESULT Results[], ULONG nResults);

typedef struct _NDK_SGE {
    union { PVOID VirtualAddress; NDK_LOGICAL_ADDRESS LogicalAddress; };
    ULONG  Length;
    UINT32 MemoryRegionToken;      // 本地 MR 的 token（NdkGetLocalTokenFromMr）
} NDK_SGE;
```

### 实现计划（一次运行内完成）

1. **服务端**：分配一个 4 KiB 缓冲区，填入**可校验的模式**（例如每字节 `i & 0xFF`，再在固定偏移写入
   字符串 `NVMEOF-STEP4`）。用 `IoAllocateMdl` + `MmProbeAndLockPages` 建 MDL →
   `NdkCreateMr` → `NdkRegisterMr(Mr, Mdl, 4096, 0, ...)` → `NdkGetRemoteTokenFromMr(Mr)` 取 rkey。
2. **把 `{缓冲区虚拟地址, rkey, 长度}` 放进服务端的私有数据**，随 `NdkAccept` 发给客户端
   （步骤 3 已证明该通道可用，且**必须**用"先查长度、再按 announced 长度取"的两次调用读）。
3. **客户端**：分配同样大小的本地缓冲区（先清零），建 MR 并注册（作为 Read 的**落地目标**），
   `NdkGetLocalTokenFromMr` 取本地 token 填进 `NDK_SGE`。
4. **客户端发起 `NdkRead`**：`pSgl` 指向本地 SGE（`Length=4096`），`RemoteAddress`/`RemoteToken`
   来自服务端私有数据。
5. **轮询 CQ**（`NdkGetCqResults`）等完成，检查 `NDK_RESULT.Status`。
6. **逐字节比对**：本地缓冲区应当等于服务端的模式。**判据必须是逐字节比较的结果**（与用户态那条
   铁律一致：13/14 的电池、`stag` 的 `[PASS] payload` 都是字节级证据）。
7. **失败时的诊断顺序**：先看 `NdkRead` 的返回码，再看 CQ 里 `NDK_RESULT` 的 Status 与
   `Transferred` 字节数，然后才怀疑内容 —— 这样能把"没发起""没完成""完成了但内容不对"区分开。

### 已知的坑（本轮踩过、写下来避免重踩）

- 私有数据第二次调用**必须**传第一次 announced 的长度，否则 `SUCCESS + len=0` **静默丢数据**；
- 读私有数据的时机是**连接事件回调里（Accept 之前）**；
- 同一 `.sys` 文件**不能二次加载**（`sc start` 会回误导性的 exit 2）→ 每次运行用唯一文件名；
- 不在 `DriverUnload` 里做"未等待的清理"（`0xCE`）；
- 不强杀持有内核 RDMA 资源的进程（`NDKPing.sys` 的 `0xCE` 教训）。