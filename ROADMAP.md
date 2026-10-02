# 路线图：从"能用"到"完备"的 NVMe-oF

这份表是给排期用的。它把"距离完备还差什么"拆成可独立交付的条目，每条都带**验收判据**——
没有判据的条目不进这张表（这是本仓库一贯的规矩：一个不会失败的检查不是检查）。

## 0. 怎么读

| 列 | 含义 |
|---|---|
| 工作量 | **人日**区间，例如 `M (3–5)`。档位：**S** 1–2 / **M** 3–5 / **L** 6–12 / **XL** 13–40。单人、熟悉本仓库、**含自测与文档** |
| 对端 | **本机** = 不需要 Linux、不需要额外硬件；**Linux** = 需要那台 Linux 机器；**外部** = 需要第二台机器/交换机/其他厂商设备 |
| 验收判据 | 这一项"做完了"的可复现证据。做不到就只能算"写了代码" |

**估算不是承诺。** 这份仓库里唯一实测过的数字都在 §1；§3 的人日都是范围判断，置信度：A、B 高，
C 中，D、E 低（见 §6）。**每个里程碑与合计的人日都由 §3 逐条相加得出**，不是另行估计的。

## 1. 现状快照（已完成、已推送、有证据）

| 能力 | 证据 |
|---|---|
| 方向 A：我们的 Windows initiator → Linux `nvmet` | 8 条队列对、字节级一致、admin 队列 keep-alive、控制器 reset 后重连 |
| 方向 B：Linux `nvme-cli` → 我们的 Windows target | 认证、Write Zeroes、DSM/deallocate；Linux 写入的字节落在 `F:` 并跨断连存活 |
| 真盘挂载 | 1 TB 盘在 Windows 上 `BusType iSCSI / NVMEOF / 931.51 GB`，逐字节校验、零写入、可拆除（`EVIDENCE-1TB.md`） |
| 纯链路 | 官方工具 2.94–2.98 GB/s（64 KiB–4 MiB），2.42 GB/s（8 MiB）；512 B 往返 4.5–6.3 µs |
| NVMe-oF 数据面 | 代码注释记录的 **~1.2 GB/s**（**本轮未复测**，见 G1） |
| iSCSI 桥 | 写路径逐字节一致（4 MiB/16 MiB 两处见证）；可装成 Windows 服务；initiator 调优可装可还 |
| 桥的实测上限 | **每会话 ~70–80 MB/s、命令深度恒为 1**（Windows iSCSI initiator 的性质，非本代码） |
| 门禁 | 本地 `run_all.ps1` **13/13 通过、181 秒**；CI 跑 4 套无硬件套件 + `hygiene.ps1` |
| 认证 | DH-HMAC-CHAP（ffdhe2048 / hmac-sha256）双向通过；六个用例常驻自测 |

## 2. 差距清单（查证自代码，非印象）

| # | 差距 | 查证位置 |
|---|---|---|
| G-1 | `OACS = 0` → 无 Format NVM / 固件 / Device Self-test / Sanitize / Namespace Management / Security Send-Recv | `fillIdentifyCtrl()`（`f5_interop.cpp`） |
| G-2 | `NN = 1` → 单命名空间，无热插拔 | 同上 |
| G-3 | **`ONCS` 从未写入** → Compare / Write Zeroes / DSM 都实现了、也测过，但**没通告**，正常 Linux 挂载不会启用 discard | `f5_interop.cpp` 全文无 `OFF_ONCS` |
| G-4 | **`NSFEAT` 未写** → thin-provisioning/deallocate 未通告，块层 `blkdiscard` 不启用 | `fillIdentifyNs()` |
| G-5 | 不通告 `ANASUP` → 无 ANA / 多路径 | `fillIdentifyCtrl()` |
| G-6 | NDSPI 无 `IB_WR_SEND_WITH_INV` → 远端 STag 作废只能靠 host 本地 invalidation | DESIGN §7 风险表 |
| G-7 | 无 SRQ、无多 QP 负载均衡、CQ 轮询（非中断） | DESIGN §2 非目标 |
| G-8 | initiator 是**用户态进程** → 不能做系统卷、不出现在磁盘管理、无真实队列深度 | 架构事实 |
| G-9 | 只有 RDMA 一种 transport（无 NVMe/TCP、无 FC-NVMe） | `nvmeof_wire.h` 只有 fabrics/RDMA 路径 |
| G-10 | CI 只跑 4 套（无硬件）；无 fuzz、无 soak；互操作只对过 Linux `nvmet` 与 LIO | `.github/workflows/ci.yml` |

## 3. 工作计划表

### A. 收尾与自洽（全本机，最快见效）· 小计 **6–12 人日**

> **状态（2026-10）：A1–A5 全部完成**，逐项证据见 §10。A2 还剩一个只能在 Linux 侧做的验证。

| 编号 | 任务 | 工作量 | 对端 | 验收判据 |
|---|---|---|---|---|
| A1 | 同步 `DESIGN.md` §2"非目标"清单——**它已经过期**：里面写着 Discovery、认证、Write Zeroes/DSM 都不做，实际都做了 | S (1–2) | 本机 | 清单每一条都能在代码里找到对应实现或"确实未做"的依据 |
| A2 | 补齐能力位通告：`ONCS`（Compare/Write Zeroes/DSM）、`NSFEAT`（thin provisioning）、`DMS`、`DLFEAT` | S–M (2–4) | Linux | Linux 挂载后 `lsblk -D` 显示 discard 能力；`blkdiscard`/`fstrim` 真的下发 DSM；读回全零且逐字节校验；`run_iscsi.ps1` 增加"通告位与实现一致"的自检 |
| A3 | `VWC`/Flush 语义对齐：现在是 `VWC=0`（命名空间是进程内存）却实现了 Flush | S (1–2) | Linux | 二选一并写进 DESIGN：要么 `VWC=1` + Flush 真正落盘（已有 RAM cache + FLUSH 写回），要么保持 0 并在文档里说明"无掉电保护"；两种都要有测试 |
| A4 | `INTEROP_F5.md` 英译（24 KB 中文） | S (1–2) | 本机 | 与中文版逐节对应；术语与 `README.md` 一致；无乱码（已纳入 `hygiene.ps1`） |
| A5 | `DESIGN.md` §7 风险表清理：把已解决的（如"远端 STag 失效我们做不到"的误解）移出或标注解决版本 | S (1–2) | 本机 | 每条风险要么有缓解证据，要么仍是开放项 |

### B. Target 协议完备性（实现本机可做，验证需要 Linux）· 小计 **19–31 人日**

| 编号 | 任务 | 工作量 | 对端 | 验收判据 |
|---|---|---|---|---|
| B1 | **Namespace Management**：`OACS` 位置位 + NSMGMT(0x0D) + Identify CNS 0x03/0x04 + create/delete/attach/detach + 支持 `NN > 1` | L (8–12) | Linux | `nvme create-ns / attach-ns / delete-ns` 全部成功；**两个命名空间同时 I/O 互不干扰**；非法 CNS/越界 NSID 被正确拒绝（有 status 证据）；自测覆盖 CNS 与命令编解码 |
| B2 | Device Self-test：`OACS` bit + Get Log Page 0x06 + Device Self-test 命令（可先实现"未运行/立即完成"语义） | M (3–5) | Linux | `nvme self-test` 返回可解析结果；未运行时结果日志格式正确；自测覆盖 |
| B3 | Format NVM：`OACS` bit + Format（对文件 backend = 清零/截断），含 `Secure Erase` 语义 | M (3–5) | Linux | `nvme format` 后读回全零且逐字节校验；进行中的 I/O 被正确拒绝；自测覆盖 |
| B4 | 保留（Reservations）：`ONCS` bit5 + Register/Acquire/Release/Report + 跨重连持久化 | M (3–5) | Linux | 两个 host 场景：A 持有保留时 B 的写被拒；重连后保留仍在；自测覆盖 |
| B5 | 固件下载/提交：**或者**实现（写进文件槽），**或者**明确回"Invalid Field"并在 Identify 里保持该位为 0 | S (1–2) | Linux | 两条路都行，但必须有测试固定住当前语义 |
| B6 | 明确不做项的正确置位 + 测试：Sanitize、Telemetry、Persistent Event Log、ZNS、KV | S (1–2) | 本机 | Identify 对应位为 0；相关命令返回正确 status（不是崩溃）；`run_iscsi.ps1` 覆盖 |

### C. 可用性：多路径与恢复（需要两块口或 Linux 双路径）· 小计 **16–27 人日**

| 编号 | 任务 | 工作量 | 对端 | 验收判据 |
|---|---|---|---|---|
| C1 | ANA：`ANASUP` + ANA log page（已有骨架）+ ANAGRPID + ANA Change Notice 异步事件 + `Set Features` | M–L (5–10) | Linux | `nvme ana-log` 可解析；切换 ANAGRPID 后主机做出预期选择（`nvme list-subsys` 显示路径状态变化） |
| C2 | Initiator 侧多路径与故障切换：双 QP/双目标口、路径失效切换、可配策略 | L (8–12) | Linux | 拔掉一条路径后 I/O **不中断**（有持续的 fio 日志与切换时间）；恢复后自动回切 |
| C3 | 控制器 reset 的完整恢复：在途 I/O 重放、AEN 驱动的命名空间重扫 | M (3–5) | Linux | 对端强制 reset 后，未完成 I/O 有明确结局（重试成功或明确失败），无挂死、无静默丢数据 |

### D. Windows 系统集成（最大的一块）

| 编号 | 任务 | 工作量 | 对端 | 验收判据 |
|---|---|---|---|---|
| D0 | **先做可行性验证**：NDKPI（内核态 RDMA）在这张 CX3 Pro / WinOF 上是否可用 | S–M (2–4) | 本机 | 一个最小内核态程序能用 NDKPI 建 QP 并完成一次 RDMA 读；**D1 是否成立全看这一步** |
| D1a | StorPort 虚拟微型端口驱动骨架：单队列、只读 | L (8–12) | 本机 | Windows 出现一块本机磁盘（不是 iSCSI 总线），可读且逐字节校验 |
| D1b | 多队列 + 写 + Flush + 掉电/超时语义 | L–XL (10–18) | 本机 | 队列深度 >1 生效（有深度证据）；写逐字节一致；超时路径有界退出 |
| D1c | 系统卷可用性、驱动签名、升级/卸载、蓝屏防护 | XL (15–30) | 本机 | 能在测试机上作为数据卷长期运行；签名可安装；有明确的失败恢复路径 |
| D2 | **替代路线**：不做内核驱动，把 iSCSI 桥做"够用"——多 target、配置文件、遥测/健康检查、MSI + 签名 | M (8–12) | 本机 | 一份安装包能在干净机器上装好并挂上盘；健康检查能报出会话/深度/吞吐；多 target 可配 |
| D3 | 调研：某个 Windows SKU 是否已自带 NVMe-oF initiator（会改变 D 的排序） | S (1–2) | 本机 | 一份结论 + 出处；结论若为"有"，D1 的优先级重排 |

- **D1 路线**（D0+D1a+D1b+D1c+D3）= **36–66 人日**
- **D2 路线**（D2+D3）= **9–14 人日**

### E. 传输种类

**E1 要拆成两半，理由见 §9 的调研结论**：Windows 侧没有可采用的成熟实现，但 **Linux 侧有**
（内核 `nvme-tcp`/`nvmet-tcp`、SPDK、libnvme 都是成熟的）。所以先做 **target 侧**——那才是能立刻
被现成 initiator 用上、也才有强对照可验证的一半；initiator 侧等 target 侧跑通、且 D2/D1 的定位
清楚之后再定。

| 编号 | 任务 | 工作量 | 对端 | 验收判据 |
|---|---|---|---|---|
| E1a | **NVMe/TCP target 侧**：PDU 编解码（ICReq/ICResp、H2C/C2H）、数据 digest、（可选）TLS | L (10–18) | Linux | Linux `nvme-cli` 用 TCP transport 连上我们的 target，方向 B 全部用例通过；与 RDMA 路径共用同一套互操作脚本 |
| E1b | **NVMe/TCP initiator 侧**（我们自己的 Windows initiator 走 TCP，去掉 RDMA 网卡依赖） | L (10–17) | Linux | 对 Linux `nvmet` 的 TCP transport 完成方向 A 全部用例；同一机器上无 RDMA 网卡也能跑通 |
| E2 | FC-NVMe | 0 | 外部 | **不做**：没有硬件，写下来是为了不偷偷省略 |

### F. 验证与质量

| 编号 | 任务 | 工作量 | 对端 | 验收判据 |
|---|---|---|---|---|
| F1 | Wire 编解码 fuzz：随机/变异 PDU 灌进 target，不允许崩溃或越界 | M (3–5) | 本机 | 在 CI 里跑固定时长；任何崩溃即失败；**必须能复现一个已知 bug 才算有效** |

> **F1 状态（2026-10）：✅ 完成**（DESIGN §8.75）。`src/iscsi_fuzz.cpp` + `src/run_fuzz.ps1`，
> AddressSanitizer 下 20 万次迭代，覆盖 PDU 构造器 / 登录文本 / 协商助手 / 挂起写表的操作序列；
> **CI 里跑 20 万次固定迭代**（本机约 45 秒，含两次构建）。"必须能失败"这一条不是靠声明，而是靠
> **同一份代码再编一遍并故意越界读一个字节、要求 ASAN 抓住它** —— 抓不到就判套件失败。
> **它第一次跑就抓到一个真 bug**：畸形报价 `"12abc"` 让 `atol()` 读出 12，协商据此把
> FirstBurstLength / MaxRecvDataSegmentLength 定成 12 字节；RFC 7143 §13.14 的下限是 512，
> 低于下限的报价应视为"无约束"。已修（`iscsiNumberMin` 的下限）并加进自检。
| F2 | soak 与断链注入：长跑 + 反复重连 + 中途杀掉对端 | M (3–5) | 本机 | 跑满约定时长（24 h 起）无泄漏、无挂死；每次断链都有界退出并恢复 |
| F5 | CI 扩容：把更多套件做成无硬件可跑 | M (3–5) | 本机 | CI 覆盖套件数从 4 提升；每个新增套件都能在无网卡机器上失败并给出 annotation |

- **F 本机小计 = 9–15 人日**（F3/F4 不计入，见下）
- F3 多厂商互操作矩阵（ESXi / SPDK / 其他 target）· L (6–12) · 外部 · 每个平台一份报告文件，格式与 `EVIDENCE-1TB.md` 一致
- F4 交换式 fabric / RoCEv2 拥塞（PFC/ECN）· L (6–12) · 外部 · 在交换机上跑通并给出吞吐/丢包/重传数据，与直连基线对比

### G. 性能 · 小计 **6–12 人日**

| 编号 | 任务 | 工作量 | 对端 | 验收判据 |
|---|---|---|---|---|
| G1 | NVMe-oF 数据面从 ~1.2 GB/s 往上：**先复测**，再定位 capsule/块层开销，考虑 SQE 批处理、减少 memcpy、多 QP | L (6–12) | 本机 | 有复测基线与优化后对比（同一脚本、同一机器）；报告"占裸 RDMA 的百分比" |
| G2 | 桥的 Windows 侧上限（~70–80 MB/s、深度 1） | 0 | — | **不可在本代码内突破**：要么接受，要么走 D1 |

## 4. 里程碑与关键路径

累计人日 = §3 小计逐项相加（M0–M3 走 **D1 路线**）。

| 里程碑 | 内容 | 本段 | 累计 | "做完了"的定义 |
|---|---|---|---|---|
| **M0 · 自洽** | A1–A5 + D3 | 7–14 | 7–14 | 文档与通告位不再互相矛盾；能力位与实现一一对应；Windows 自带 initiator 这件事已有结论 |
| **M1 · target 协议完备（单路径）** | B1–B6 | 19–31 | 26–45 | Linux 侧能对这块盘做 format / 多命名空间 / 保留，且每条都有报告 |
| **M2 · 可用性与质量** | C1–C3 + F1、F2 | 22–37 | 48–82 | 拔路径不断流；长跑无泄漏；fuzz 能挡住已知 bug |
| **M3 · Windows 原生磁盘** | D0 → D1a → D1b → D1c | 35–64 | 83–146 | Windows 把它当本机磁盘，队列深度 >1，有签名与升级路径 |
| **M4 · 第二条传输**（可与 M3 并行） | E1a →（再定 E1b） | 10–18 | — | NVMe/TCP 的 target 侧被 Linux `nvme-cli` 用 TCP 连上并跑通方向 B |

**关键路径**：`D0（NDKPI 可行性）` 是所有 Windows 原生方案的咽喉；它不成立，M3 就只剩 D2 路线。
**可并行的**：M4 与 M3 互不依赖；A、B、F1/F2 全程可在本机推进。

## 5. 总量

| 工作流 | 人日（估算） | 是否需要外部条件 |
|---|---|---|
| A 收尾与自洽 | 6–12 | 否 |
| B target 协议完备 | 19–31 | 验证需 Linux |
| C 多路径与恢复 | 16–27 | 需 Linux（最好两口） |
| D Windows 集成（D1 路线） | 36–66 | 否（内核驱动需测试签名环境） |
| D Windows 集成（D2 路线） | 9–14 | 否 |
| E NVMe/TCP（E1a target 10–18 + E1b initiator 10–17） | 20–35 | 验证需 Linux |
| F 验证与质量（不含 F3/F4） | 9–15 | F3/F4 另需外部环境 |
| G 性能 | 6–12 | 否 |

| 路线 | 合计（不含 E、不含 F3/F4） | 折合 |
|---|---|---|
| **D1（协议完备 + 内核驱动）** | **92–163 人日** | 约 4.5–8 人月 |
| 　同上 + E1a（只做 TCP target 侧） | 102–181 人日 | 约 5–9 人月 |
| 　同上 + E1a + E1b（TCP 双向） | 112–198 人日 | 约 5.5–10 人月 |
| **D2（产品可用，不做内核驱动）** | **65–111 人日** | 约 3–5.5 人月 |

按 20 人日/人月折算；F3/F4 各自 6–12 人日，且由外部环境决定能否开工。

## 6. 关键风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **NDKPI 不可用或不可控** | M3 整个不成立 | D0 先做，成本 S–M，放在所有内核工作之前 |
| 内核驱动需要测试签名/调试环境，且有蓝屏风险 | M3 的时间与风险都在此 | D1 分三段交付，每段都可单独停 |
| D/E 的工作量置信度低 | 排期不准 | 把 D/E 拆成探针先做：D0；E1 先只做一个 PDU 编解码 + 一个用例 |
| Linux 对端与第二台机器/交换机不确定 | C、F3、F4 卡住 | 不需要它们的 A、B、F1、F2、G 排在前面 |
| 通告位与实现不一致（G-3/G-4 已经发生过一次） | 主机用不到已实现的功能 | A2 的自检把"通告位 ↔ 实现"钉住 |
| **Windows 自带 NVMe-oF initiator**（只在某些 SKU） | 会改变 D 的优先级：Server 有，客户端没有 | **已部分查实，见 §9**：本机（Windows 11 专业工作站版 build 26200）**没有任何** NVMe-oF initiator；Windows Server 2025 据说有，但传输种类未能核实 |

## 7. 决定与仍然开放的事

**2026-10 已定**（用户拍板）：

1. **目标两个都要**：先按 D2 把产品可用的部分做扎实，同时推进 D1；**D1 以 D0 的探针结果为前提**，
   D0 不成立就只剩 D2。
2. **NVMe/TCP 按 §9 的调研结论办**：可采用的成熟实现只在 Linux 侧，Windows 侧没有 →
   **做，但先做 target 侧（E1a）**，initiator 侧（E1b）等 E1a 跑通再定。
3. **外部环境**：以后最多有一台带 RDMA 的笔记本 → **F3/F4 暂时移出计划**，等那台机器到位再排。
4. **内核驱动看情况**：先做 D0（2–4 人日），用它决定 D1 是否值得开工。

**仍然开放**：D1 与 B/C 的先后（先协议完备还是先 Windows 原生盘）由 D0 的结果和你的排期共同决定。

## 8. 不等决定也能开始的（建议第一批）

**A1–A5 + D3 + F1**（小计 **10–19 人日**），全部在本机完成、全部有验收判据、都不依赖 Linux 或外部环境。
其中 **A2** 是本表里性价比最高的一项：改动很小，但它把"已经实现却用不到"的功能真正交给主机
（Linux 侧 discard / thin-provisioning 立刻可用），而且前后差异可以当场测出来。
**注**：D3 的大部分已经在本轮做完（见 §9），剩下的是"Windows Server 2025 的 initiator 支持哪些传输"
这一个问题——它不影响本机（客户端 SKU）的结论。

## 9. 两项调研结论（E1 与 D3）

### 9.1 NVMe/TCP：现成的成熟实现都在 Linux 侧，Windows 侧没有

| 实现 | 平台 | 可被我们采用吗 |
|---|---|---|
| Linux 内核 `nvme-tcp` / `nvmet-tcp` | Linux | 不能（内核代码，且不在 Windows 上跑） |
| SPDK（含 NVMe/TCP initiator 与 target） | Linux / FreeBSD；Windows 移植是实验性质 | 不能（Windows 侧没有可用的成熟移植） |
| libnvme / `nvme-cli`（含 TCP transport） | Linux/Unix 用户态 | 不能（同前） |

**结论：做，但先做 target 侧（E1a）。** 理由不是"没人做过"，而是**能用的那一半正好相反**：
target 侧的对方是 Linux `nvme-cli`——**成熟、现成、可以当强对照**，我们写完立刻能被它验证，
而且这条路让"任何 Linux 机器 + 普通网卡"就能用上我们的 target（不需要 RDMA 网卡）。
initiator 侧（E1b）的对方才是我们自己，价值取决于 D2/D1 怎么定位，所以留到 E1a 之后。

### 9.2 Windows 自带的 NVMe-oF initiator：Server 有，客户端没有（本机已实测）

**本机实测**（Windows 11 专业工作站版，build 10.0.26200）：

```
与本机 NVMe-oF 相关的服务/驱动/命令
  服务 : 无（只有一个无关的 WSAIFabricSvc）
  驱动 : nvmedisk(running)、stornvme(running)   <- 这两个是 LOCAL NVMe 栈，不是 fabrics
  命令 : 无
对照 : msiscsi = Running（Microsoft iSCSI Initiator Service）
```

即：**这台机器有内置 iSCSI initiator，没有任何 NVMe-oF initiator**——这正是 iSCSI 桥存在的理由，
也说明 G-8 在本机依然成立。可选功能列表需要管理员权限，本轮没能枚举（诚实记账）。

**Windows Server 2025 则 reportedly 带了 NVMe-oF initiator**，来源：
[4sysops 的 Server 2025 存储新特性](https://4sysops.com/archives/new-storage-features-in-windows-server-2025-nvme-of-initiator-update-for-s2d-deduplication-for-refs/)、
[NetApp 的 ONTAP SAN host 文档（NVMe/FC for Windows Server 2025）](https://docs.netapp.com/us-en/ontap-sanhost/nvme-windows-2025.html)、
[Pure Storage 的 Windows Server Initiator 实践](https://blog.purestorage.com/purely-technical/nvme-over-fabrics-nvme-of-with-windows-server-initiator-and-everpure/)。
其中 NetApp 那篇给出了两条**值得记住的细节**：

- 它需要厂商 HBA 与驱动参数（`EnableNVMe=1`、`NVMEMode=0`）——**不是纯软件就能用**；
- **Broadcom 在 Windows 上的 "NVMe/FC" 是一个 SCSI ⇄ NVMe 转译驱动**，不是真正的 NVMe/FC 驱动；
  其后果是 NVMe/FC 与 FCP 在 Windows 上性能相同（不如 Linux 上差距那么大），而且命名空间在系统里
  表现为 SCSI LUN（`nvme-list-ns` 输出里带 SCSI Bus/Target/OS LUN）。

**没能核实的**（页面被 403 或 PDF 不支持，如实记账）：Server 2025 的 initiator 支持 **哪些传输**
（FC / TCP / RDMA），以及 **Windows 11 24H2+ 客户端 SKU** 是否也带。

**对本计划的影响**：不动 G-8 在本机的结论，也不影响我们的 **target 侧**；但它意味着"Windows 没有
NVMe-oF initiator"这句话是 **SKU 相关**的，如果将来要在 Server 上部署，D1 的 StorPort 驱动必要性
需要重新评估——那时 D3 的剩余那一问必须先有答案。

## 10. A1–A5 完成情况（2026-10）

| 项 | 结果 | 证据 |
|---|---|---|
| A1 | ✅ | `DESIGN.md` §2 的每一行都标了今天的状态：Discovery、DH-HMAC-CHAP 认证、Write Zeroes/DSM 标"已做"并指到对应章节；多路径/ANA、Format、固件、元数据/PI、SRQ、Namespace Management 标"仍不做"并指到本表的编号 |
| A2 | ✅ 代码 + 三道本机门禁；**Linux 侧待验** | 能力位与实现同源：`kNvmeOfIoCaps` 表 + `nvmeofOncsFromCaps()`，Identify 写出的 ONCS = 0x000C（DSM + Write Zeroes，**不含** Compare/Write Uncorrectable/Reservations/Timestamp）；命名空间侧 `NSFEAT.THIN` + 粒度提示 7（4 KiB）。门禁三条：`run_iscsi.ps1` 的 `test_nvmeof_caps`（无硬件）、`run_xref.ps1`（104 常量 0 不匹配，新增 `NVME_NS_FEAT_THIN`）、`run_f5.ps1` 本机 initiator→target 两条 `[PASS]`。`DLFEAT` 刻意留 0（未报告），理由见 DESIGN §8.74(2) |
| A3 | ✅ 两半都实测 | `VWC = g_nsFile ? 1 : 0`：内存后端 `[ok] Identify VWC=0, which matches the memory-only namespace`，文件后端 `[ok] Identify VWC=1, which matches the file-backed namespace`（同一次运行里能看到 `q1 FLUSH -> 98304 blocks written to D:\nvmeof\ns48.img`，这正是 VWC=1 的依据）。target 每次填 Identify 自检，不一致就打 `run_all.ps1` 会抓的 `[FAIL]` |
| A4 | ✅ | `INTEROP_F5.en.md`（40966 B / 656 行；与中文版行数相同，49 个 `#` 行、26 个代码块一一对应；命令/路径/IP/NQN 原样；`hygiene.ps1` PASS） |
| A5 | ✅ | DESIGN §7 两行标注为已解决（memory window → `stag_smoketest.cpp` 已进 `run_f1/run_f3` 的前置步骤；远端 STag 失效 → 是误解，host 自己本地 `IB_WR_LOCAL_INV`），并新增一行"通告位与实现漂移"的风险（§8.70 与 §8.74 各发生一次） |

**A2 剩下的那一步（只能在 Linux 侧做，命令备好）**：

```bash
mount /dev/nvme1n1 /mnt/ns
lsblk -D                                  # DISC-GRAN / DISC-MAX 应为非零
blkdiscard -o 0 -l 8M /dev/nvme1n1        # 真的下发 DSM
nvme read /dev/nvme1n1 -s 0 -c 8 -z 4096  # 读回全零
fstrim -v /mnt/ns                         # fstrim 是否报出释放量
```

**下一步**：按 §8 的建议第一批里还差 `F1`（wire 编解码 fuzz，3–5 人日，本机可做）。
