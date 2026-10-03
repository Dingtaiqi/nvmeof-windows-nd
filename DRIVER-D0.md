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