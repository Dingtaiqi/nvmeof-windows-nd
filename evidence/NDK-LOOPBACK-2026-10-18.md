# 单机 RDMA：NDK 允许同 HCA 自环的 CM 握手，但第一个 NdkSend 在 provider 内挂死

日期：2026-10-18 01:00–01:15（Asia/Shanghai）
硬件：Mellanox ConnectX-3 Pro 双口，**两口互插**（40 Gbps，双方 Connected）
地址：`192.168.100.2` = 以太网 4（ifIndex 30），`192.168.100.3` = 以太网 5（ifIndex 17）
驱动：`src/driver/nvmeofk.c`（本次新增 listener 半边 + 内核内 nvmet 应答器），
镜像 `sym\k\nvmeofk_d4.sys`，testsigning 已开，签名证书 `CN=NVMeoF Test Driver`。

---

## 0. 先纠正交接文档里的一个结论

交接文档说"**同卡回环被 RDMA-CM 拒绝（0xC0000236，实测）**"。复检证明这个结论**无效**：

- `D:\rdma\rdma_t.out`（上一次实测的输出）只有 4 行，最后一行是
  `NdOpenAdapter failed 0xC0000141` —— 用户态目标 `f5_interop` **根本没打开 adapter**
  （本机没有 `ndv2.dll`，用户态 NetworkDirect 不可用）。
- 因此 `192.168.100.2:4420` 上**没有 listener**。
- `0xC0000236` 在 `ntstatus.h` 里是 `STATUS_CONNECTION_REFUSED` —— 没人监听时的正确回应，
  它**不能**证明自环被拒绝。

真正的自环测试必须有一个**真实存在的 listener**。下面这个就是。

---

## 1. 做法

`nvmeofk.c` 新增（`src/driver/nvmeofk.c`，构建脚本 `src/driver/build_k.cmd`）：

- 每个端口开**一个独立的 NDK adapter**：listener 在 ifIndex 30（.2），
  connector 在 ifIndex 17（.3），两者是同一张卡的两个口；
- listener 侧：PD / CQ / **QP（NdkAccept 必需）** / MR / `NdkCreateListener` / `NdkListen(.2:port)`；
- connector 侧沿用原来的 PD / CQ / QP / connector / MR；
- 全流程在 `DriverEntry` 里同步完成，每一步都把结果写进
  `HKLM\Software\NVMeoFProbe\nvmeofk_log`（ASCII 塞在 REG_SZ 里，逐 UTF-16 单元取两字节解码）；
- 结束前关闭**所有** NDK 对象并把两个 adapter `WskCloseNdkAdapter` 还回去；
- 每次运行用**新文件名 + 新服务名**的镜像：这台机器上 legacy 内核服务被 SCM 标成
  `NOT_STOPPABLE`（`sc stop` → 1052），同一镜像无法卸载重载。

运行脚本：`tools/run_kernel_listener.ps1`（只碰 `nvmeofk*` 测试驱动，
不碰 `nvmeofnd2`/`nvmeofndi`、不碰目标端/桥进程、不重启）。

关键 API 签名（`km\ndkpi.h`，10.0.26100.0，逐行核对）：

- `NdkCreateListener(adapter, NDK_FN_CONNECT_EVENT_CALLBACK, ctx, completion, ctx, &listener)`（924–934）
- `NDK_FN_CONNECT_EVENT_CALLBACK = VOID (PVOID ctx, NDK_CONNECTOR *c)`（807–813）
- `NdkListen(listener, sockaddr, len, completion, ctx)`（824–831）
- `NdkAccept(connector, qp, inLimit, outLimit, privateData, len, disconnectEvent, ctx, completion, ctx)`（694–708）
- 状态码均在 `shared\ntstatus.h` 中核对：`0xC0000023 = STATUS_BUFFER_TOO_SMALL`、
  `0xC0000043 = STATUS_SHARING_VIOLATION`、`0xC0000236 = STATUS_CONNECTION_REFUSED`、
  `0x00000103 = STATUS_PENDING`。

---

## 2. 结果（按运行顺序，全部取自注册表日志）

### 运行 A —— 只有 listener，不连接（端口 4420）：全绿

```
L WskOpenNdkAdapter(ifIndex=30, v2.0) = 0x00000000 adapter=0xffff968211c3f200
L NdkCreatePd        = 0x00000000 pd=0xffff968211ade9a0
L NdkCreateCq        = 0x00000000 cq=0xffff9681fc4d5470
L NdkCreateQp        = 0x00000000 qp=0xffff9682154878e0
L NdkCreateListener  = 0x00000000 listener=0xffff9681fb27ed30
L NdkCreateMr        = 0x00000000 mr=0xffff9681e385fc60
L NdkRegisterMr(flags 0x9) = 0x00000000
L NdkListen(192.168.100.2:4420) = 0x00000000
L NdkGetLocalAddress = 0x00000000 len=16 family=2 port=4420 addr=192.168.100.2
teardown: listener closed / qp / cq / mr / pd / adapter closed
```

证据文件：`D:\rdma\klisten_listen-only.txt`。
`NdkGetLocalAddress` 回读到的就是 `family=2 port=4420 addr=192.168.100.2`（16 字节），
说明 listener **真的绑上了**，不是"提交成功"而已。

### 运行 B —— 自环连接（端口 4420，peerData 缓冲 64 字节）：握手到一半，CompleteConnect 不返回

```
NdkConnect submit  = 0x00000103 (pending)
L connect event    = 0x00000000 events=1 incoming=0xffff968211e76210
L NdkGetConnectionData = 0x00000000 inbound=1 outbound=1 len=56
L peer private data: recfmt=0 qid=0 hrqsize=32 hsqsize=31 cntlid=0
L NdkAccept submit = 0x00000103 (qp ...)
NdkConnect         = 0x00000000
NdkGetConnectionData = 0xC0000023 peer inbound=1 outbound=1 dataLen=148 recfmt=0
（此后无输出；sc query 一直是 START_PENDING）
```

`0xC0000023 = STATUS_BUFFER_TOO_SMALL`：64 字节的缓冲不够，provider 需要 148 字节。
之后 `NdkCompleteConnect` 没有返回（连"returned"标记都没有，见运行 D 加的标记）。

### 运行 C —— 自环连接（端口 4425，peerData 缓冲 1 KiB）：连接**完全建立**

```
NdkConnect submit  = 0x00000103 (pending)
L connect event    = 0x00000000 events=1 incoming=0xffff9682128b0210
L NdkGetConnectionData = 0x00000000 inbound=1 outbound=1 len=0
L NdkAccept submit = 0x00000103 (qp ...)
NdkConnect         = 0x00000000
NdkGetConnectionData = 0x00000000 peer inbound=1 outbound=1 dataLen=0
about to call NdkCompleteConnect (connect done, accept submitted)
NdkCompleteConnect returned 0x00000103 (pending: waiting for the completion)
NdkCompleteConnect completion phase = 0x00000000
（此后无输出；sc query 一直是 START_PENDING）
```

换成 1 KiB 缓冲后 `NdkCompleteConnect` **完成 0x0**，说明运行 B 里它不返回与
`STATUS_BUFFER_TOO_SMALL` 有关，而不是自环被拒。

### 运行 D —— 同上，但在每个数据路径调用前后都落盘（端口 4426）：挂在 `NdkSend`

`evidence/ndk-loopback-d4.log` 的最后 9 行（逐字节）：

```
NdkGetConnectionData = 0x00000000 peer inbound=1 outbound=1 dataLen=0 recfmt=0 crqsize=0
about to call NdkCompleteConnect (connect done, accept submitted)
NdkCompleteConnect returned 0x00000103 (pending: waiting for the completion)
NdkCompleteConnect completion phase = 0x00000000
NdkCompleteConnect = 0x00000000
L NdkAccept        = 0x00000000 (the same card accepted its own connection)
X1 posting the 16-byte response receive on the initiator qp
X1 postResponseRecv = 0x00000000
X2 arming the in-kernel target receive
X2 responderPrepare = 0x00000000
X3 sending the 64-byte capsule
```

`X3` 之后：**没有** `X3 sendCapsule = ...`，**没有** CQ 完成，**没有** 20 秒超时日志
（`sendCapsule` 里 `NdkSend` 的下一条语句就是有界 `reap`，而它会在 80 秒内返回）。
运行 4 分钟后仍然停在 `START_PENDING`。

**结论：是 `NdkSend(g_qp, ..., 1 个 64 字节 SGE, flags=0)` 这个同步调用本身没有返回。**

---

## 3. 结论（严格区分已实测 / 未验证）

### 已实测

1. **NDK/mlx4 允许同 HCA 自环的 RDMA-CM 握手。**
   从 .3 发起、.2 上的内核 listener 接收：connect event 到达（events=1）、
   `NdkAccept` 完成 0x0、`NdkCompleteConnect` 完成 0x0。
   即 REQ/REP/RTU 三步全部走通，**没有** `0xC0000236`。
   这直接推翻了"同卡回环被 CM 拒绝"的旧结论。
2. **第一个数据路径 SEND 在 provider 内部同步挂死。**
   `NdkSend` 不返回、无 CQ 完成、无错误；同一驱动里 listener 的
   `NdkReceive`（X2）已经成功提交。
3. **这种挂死不可恢复。** 驱动 `NOT_STOPPABLE`（`sc stop` → 1052），
   `DriverEntry` 线程永远停在 `START_PENDING`；本机已累积 3 个（4420/4425/4426 三次运行）。
4. **`0xC0000023` 是 `peerData` 缓冲太小**（需要 148 字节），与连接建立无关；
   把缓冲改成 1 KiB 后 `NdkCompleteConnect` 立即完成。
5. 副作用：三次挂死之后 **WMI 查询开始 20 秒超时**（`Get-CimInstance Win32_OperatingSystem`
   已超时两次），普通 shell、SCM 仍正常。

### 未验证（需要重启后、在干净状态上做）

- 在 `NdkCompleteConnect` 完成与第一次 `NdkSend` 之间加 1–2 秒延迟，是否能避开挂死
  （怀疑 provider 的连接收尾 work item 与 send 路径抢同一把锁）。
- 先做 **RDMA Read**（listener 读 initiator 的 1024 字节 Connect data）而不是 SEND，
  看数据面是否只在 SEND 路径上坏。
- 两个 QP 放在**同一个 adapter** 上（listener 和 connector 都用 .2）是否能自连且能发数据。
- 跨机数据路径（需要第二台机器；本机现在没有）。

---

## 4. 机器现状与恢复建议

- 3 个测试驱动卡在 `START_PENDING`：
  `nvmeofkloop010118`、`nvmeofkloopd3b`、`nvmeofkloopd4`。
- 它们都不能 `sc stop`（1052），文件被映射无法覆盖，**只能重启清理**。
- 重启前不要再用 `nvmeofk*` 做新的内核实验：每个挂死都会再留一个不可回收的内核线程，
  WMI 已经受影响。
- 重启后 Z: 的恢复路径不受影响（`nvmeofnd2` + 两个开机计划任务不依赖这些测试驱动），
  但仍应按交接文档的自检步骤确认一次。
