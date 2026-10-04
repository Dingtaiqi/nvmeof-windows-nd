# 搭建 KDNET 内核调试环境 —— 定位内核 NVMe-oF 连接路径崩溃

> 结论先行：**这个 bug 靠"崩一次猜一次"找不到** ✗。它的 bugcheck 是 `0x139 P1=0xa`
> （**incorrect stack**，栈被破坏），转储里的调用栈**完全无法展开** ✗ —— 拿不到出错指令 ✗。
> 唯一能直接看到出错指令的办法是**活的内核调试会话**：目标机崩溃的瞬间，调试器停在
> 出错指令上，寄存器、栈、内存**全部可读** ✓✓。

---

## 一、拓扑

```
[目标机 = 这台 Windows]                        [调试主机 = 另一台 Windows]
  待查的 nvmeofk*.sys 在这里加载                  kd.exe 在这里运行
  bcdedit /debug on                             kd -k net:port=50000,key=<KEY>
  bcdedit /dbgsettings net hostip=<调试主机IP>   加载 nvmeofk*.pdb → 有函数名与行号
                          ←── KDNET（二层 UDP，默认 50000）──→
```

**调试主机可以是**（任一即可）：
1. **笔记本上装一个 Windows 虚拟机**，网络设为**桥接（Bridged）** ✓✓ ← **最省事，不用重装系统** ✓
2. 那台笔记本**临时双系统/换 Windows** ✗
3. **任何一台旧 Windows 机器/另一台 PC** ✓

**为什么 Linux 不行** ✗：Linux 上没有 `kd`（WinDbg 是 Windows 程序）✗。

---

## 二、目标机侧（这台机器，需要**一次重启**）

### 1. 选定调试网卡

| 网卡 | 能否用于 KDNET | 说明 |
|---|---|---|
| **以太网 24**（Mellanox CX3，192.168.100.2） | ★ **首选** | 独立、不在网桥里 ✓。驱动是 WinOF 5.50(2020) ✗，**是否支持 KDNET 未知** ✗ → 只能实测 |
| 以太网 23（同卡第二口） | ✗ | 是网桥成员，被桥独占 |
| 以太网 20（Intel I211，1 Gbps） | ✗ | **也是网桥成员**（你的局域网靠它）✗，腾出来会断网 |

**先试 Mellanox** ✓ —— 失败无副作用（最多是调试连不上 ✓，系统照常运行 ✓）。

### 2. 配置（需要调试主机先给出 IP / 端口 / KEY）

```powershell
# 需要管理员权限的 PowerShell
bcdedit /debug on
bcdedit /dbgsettings net hostip:<调试主机IP> port:50000 key:<KEY>

# 如果上面的命令报"网卡不支持"，试指定 PCI 位置（Mellanox 是 Bus4 Dev0 Fn0）：
bcdedit /dbgsettings net hostip:<调试主机IP> port:50000 key:<KEY> busparams=4.0.0

# 查看当前调试设置
bcdedit /dbgsettings
bcdedit /enum {current} | Select-String debug
```

**`KEY` 的格式**：`a.b.c.d`（如 `1.2.3.4`）或 8 位十六进制串 ✓，两端必须**完全一致** ✓。

**然后把网卡从网桥释放是不需要的** ✓ —— 用 `以太网 24` 即可 ✓（它本来就不在网桥里 ✓）。

### 3. 重启目标机

```powershell
Restart-Computer -Force
```
重启后调试器**一开机就能接上** ✓（在 Windows 启动早期就建立连接 ✓）。

### 4. 确认调试已启用

```powershell
bcdedit /dbgsettings           # 应显示 net / hostip / port / key
# 若想临时关掉： bcdedit /debug off
```

---

## 三、调试主机侧（笔记本上的 Windows 虚拟机）

### 1. 装调试器
- 首选：**Windows SDK → 只勾 "Debugging Tools for Windows"** ✓（约 80 MB）
- 或 `winget install --id Microsoft.WinDbg` ✓
- 确认可执行文件存在：`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe`

### 2. 放行入站 UDP 50000
```powershell
New-NetFirewallRule -DisplayName 'KDNET-In' -Direction Inbound -Protocol UDP -LocalPort 50000 -Action Allow
```

### 3. **虚拟机网络必须是"桥接"**（NAT 收不到 KDNET 的广播 ✗）
- VirtualBox：网络 → 连接方式 → **桥接网卡** ✓
- VMware：网络适配器 → **桥接** ✓
- Hyper-V（若笔记本换 Windows）：**外部交换机** ✓
- 并确认虚拟机里 `Get-NetIPAddress` 能拿到**与目标机同网段**的地址 ✓（例如 192.168.1.x 或 192.168.100.x）

### 4. 把符号文件拷到调试主机
从目标机取这两个文件（U 盘/共享/远程复制均可）：
```
D:\rdma\nvmeof\src\driver\sym\nvmeofk21.pdb     (452 KB)   ← 内核驱动的符号
D:\rdma\nvmeof\src\driver\sym\nvmeofk21.sys     (26 KB)    ← 与它配套的二进制
D:\rdma\nvmeof\src\driver\sym\nvmeofk21.map     (34 KB)    ← 备用：地址映射表
```
放在调试主机的 `C:\sym\` ✓。

### 5. 接上调试会话
```powershell
mkdir C:\kdnet -Force
& "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe" `
    -k net:port=50000,key=<KEY> `
    -y "C:\sym;srv*C:\symbols*https://msdl.microsoft.com/download/symbols" `
    -logo C:\kdnet\session.log
```
- 第一次会下载微软符号（视网络 ✗ 可能较慢 ✓）；**即使下不来，`C:\sym` 里的 `nvmeofk21.pdb` 也足以给出我们驱动里的函数名与行号** ✓✓
- 连上后会停在 `kd>` 提示符 ✓

### 6. ★ 崩溃发生的瞬间，在 `kd` 提示符下依次执行 ★

```
.logopen C:\kdnet\crash-report.txt
.bugcheck
!analyze -v
r
kv
kb
u @rip L20
lm vm nvmeofk21
lm vm mlx4eth63
!thread
.cxr 0
dq nvmeofk21!g_async L8
dq nvmeofk21!g_connectCtx L8
dq nvmeofk21!g_connector L2
dq nvmeofk21!g_qp L2
dq nvmeofk21!g_cq L2
dq nvmeofk21!g_pd L2
dq nvmeofk21!g_mr L2
dq nvmeofk21!g_region L2
q
```

**重点看三样** ✓：
1. **`u @rip`** ＝ **出错的确切指令** ✓✓（这是我们一直拿不到的东西 ✗）
2. **`kv` / `kb`** ＝ **完整调用栈**（有 PDB 就有函数名+行号 ✓）
3. **`dq nvmeofk21!g_*`** ＝ **崩溃瞬间我的全局变量值** ✓（哪个 NDK 对象指针是野的 ✓）

### 7. 把这份报告发回来
`C:\kdnet\crash-report.txt` 的完整内容 ✓ —— 有了出错指令和栈，**根因基本就是一行代码的事** ✓。

---

## 四、如果 Mellanox 不支持 KDNET（`bcdedit` 报错或连不上）

按可行性排序的退路：

| 方案 | 代价 | 说明 |
|---|---|---|
| **USB 3.0 调试（USB-KD）** | 买一根 **USB 3.0 A-A 调试线**（几十元 ✓） | 两台机器 USB 直连，**不占网卡、不受网桥影响** ✓✓ 很稳 |
| **串口调试** | 需要串口/串口卡 ✗ | 你这台没有串口 ✗ |
| **临时把 Intel I211 从网桥腾出来** | 会短暂断网 ✗ | 只有 以太网 20 与 以太网 23 都是成员时才能腾一个 ✓；而 I211 正是局域网那根 ✗ |
| **换一块支持 KDNET 的独立网卡** | 需要一条 PCIe 槽 + 网卡 | 你这台是台式机，有槽 ✓ |

**命令示例（USB-KD，插好线后）**：
```powershell
bcdedit /debug on
bcdedit /dbgsettings usb targetname:TESTPC1
# 调试主机侧：
kd -k usb:targetname=TESTPC1
```

---

## 五、这一夜已经确认的事实（不必重查）

- 内核驱动的 **NDK bring-up 全绿** ✓：`NdkOpenAdapter` / `CreatePd` / `CreateCq` / `CreateQp` / `CreateConnector` / `CreateMr` / `RegisterMr` **全部 status 0**，rkey/lkey 到手 ✓（网络关闭时反复验证 ✓）
- **一旦打开 `doconnect=1` 就会在第一次 NDK 创建附近 `0x139`** ✗（`P1=0xa` incorrect stack ✓）
- 已修并确认的**头文件级真 bug** ✓：`NdkGetConnectionData` 的最后一个参数是 **IN/OUT**，输入时必须给出缓冲区大小 ✓（原代码传了未初始化栈变量 ✗ → 提供程序按垃圾长度写栈 ✓）—— **这个修复保留在 `nvmeofk21` 里** ✓
- 已修的其他真缺陷 ✓：等待链表被重复 `KeInitializeEvent` 破坏（转储实证）✓、MR flags 非法枚举值（`0x5` 不是位）✓、关闭回调签名与 `ndkpi.h` 不符 ✓、释放路径 `WskReleaseProviderNPI` 挂死（`Start Pending`）✓
- **每次测试前必须重启** ✓（驱动故意不释放对象 ✓，十来次运行后提供程序状态会污染到早期就崩 ✓）

**驱动二进制与符号**：`D:\rdma\nvmeof\src\driver\sym\nvmeofk21.sys` + `.pdb` + `.map` ✓（已签名 ✓，含 9 个增量日志落盘点 ✓ —— 以后即使崩溃，注册表 `HKLM\Software\NVMeoFProbe\nvmeofk_log` 里也会留下它走到哪一步 ✓）
