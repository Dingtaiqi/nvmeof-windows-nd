# Windows 上的 NVMe-oF over RDMA（自研 NetworkDirect 栈）

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


