# 给笔记本 AI 的提示词（修正版）：用仓库里已有的脚本启动 Linux 目标

把下面**整段**粘贴给笔记本上的 AI。**这台笔记本是 EndeavourOS，用千兆网卡上的软件 RoCE（`rdma_rxe`）**，不是硬件 RDMA 卡 —— 下面全部按这个事实来。

---

## 你的任务

笔记本要为另一台机器（Windows，192.168.100.2）上的**内核态 NVMe-oF initiator** 提供 **NVMe-oF over RDMA 目标**。这个项目早就做过这件事 ✓ —— **你不需要自己摸索，仓库里已经有一整套验证过的脚本** ✓。

## 已知事实（都来自本项目自己的记录）

| 项 | 值 |
|---|---|
| 笔记本角色 | **EndeavourOS + 普通千兆网卡 + 软件 RoCE（`rdma_rxe`）** |
| 笔记本的 RoCE 地址 | **192.168.100.5**（rxe 设备上；本项目一直用它） |
| Windows 侧 | **192.168.100.2**，Mellanox CX3，40G 口 |
| 通路 | Windows 的 40G 口 → 自环线 → 另一块 40G 口（桥成员）→ 网桥 → 千兆 LAN → 你 |
| 端口 | 4420 |
| 子系统 NQN | **`nqn.2024-01.local.rdma:linux-nvmet`** |
| 你的仓库位置 | 应该在 **`~/nvmeof/`**（脚本里引用的是 `$HOME/nvmeof/linux/...`）。若不在，请问用户仓库在哪 |

## 请按顺序做

### 1. 确认仓库与脚本在
```bash
ls -l ~/nvmeof/linux/f5_linux_up.sh ~/nvmeof/linux/nvmet_setup.sh
```
若不存在，请让用户把仓库拷过来（或从 GitHub 拉：本项目仓库名含 `nvmeof-windows-nd`）。

### 2. 确认 rxe 与内核模块可用
```bash
uname -r
modinfo rdma_rxe | head -3
modinfo nvmet-rdma | head -3
ip -br addr                      # 找到千兆网卡名（下称 $ETH；不是 100.x，100.x 是 rxe 上的逻辑地址）
ip -br link | grep -i rdma       # 可选：看有没有 rxe0
```
若 `rdma_rxe`/`nvmet-rdma` 缺模块：告诉用户需要装对应的内核模块包，并回报 `modinfo` 的输出。

### 3. 用仓库脚本把目标拉起来（这一步会做 rxe + 地址 + nvmet 三件事）
```bash
cd ~/nvmeof/linux
sudo ./f5_linux_up.sh
```
如果它需要参数，用法是（脚本头部注释里写着）：
```bash
# f5_linux_up.sh [什么网卡] [roce-ip]
sudo ./f5_linux_up.sh <千兆网卡名> 192.168.100.5
```
它内部会依次：加载 `nvmet nvmet-rdma rdma_rxe brd` → 在网卡上起 `rxe` → 给 rxe 设备配上 `192.168.100.5` → 用 `nvmet_setup.sh` 建 configfs 子系统与端口 ✓。

### 4. 验证目标确实在监听（本项目历史上的验证方式）
```bash
# 地址与 rxe 设备
rdma link show                      # 应看到 rxe0 之类，state ACTIVE
ip -br addr | grep 192.168.100
# nvmet 配置
ls /sys/kernel/config/nvmet/ports/1/
cat /sys/kernel/config/nvmet/ports/1/addr_traddr
cat /sys/kernel/config/nvmet/ports/1/addr_trsvcid
ls -l /sys/kernel/config/nvmet/ports/1/subsystems/
# 自检：用本机的 nvme-cli 连自己（连上就说明目标没问题）
sudo nvme discover -t rdma -a 192.168.100.5 -s 4420
sudo nvme connect  -t rdma -a 192.168.100.5 -s 4420 -n nqn.2024-01.local.rdma:linux-nvmet
nvme list                            # 应出现一个新的 nvme 设备
sudo nvme disconnect -n nqn.2024-01.local.rdma:linux-nvmet    # 自检完断开
```

### 5. ★ 让目标保持运行，并回报

**不要** teardown、**不要**卸载模块 ✓ —— 目标要保持可用，等 Windows 那边的内核驱动来连。

回报这 5 项：
1. `uname -r` 与 `rdma link show` 的输出
2. `ip -br addr` 里 `192.168.100.5` 那一行（确认在哪个设备上）
3. `f5_linux_up.sh` 的完整输出
4. **第 4 步自检是否成功**（`nvme list` 有没有新设备）
5. `ping -c 2 192.168.100.2` 的结果（通不通都要报）

## 硬性约束

1. **不要** `rmmod`、不要 `nvmet_teardown.sh` —— 目标要保持可用 ✓
2. **不要**改笔记本的默认路由或 Wi-Fi；只碰 rxe 与千兆网卡 ✓
3. 若 `192.168.100.5` 已被别的设备占用，换一个 100.x 地址也可以，**但必须在回报里写明最终地址** ✓（Windows 侧要改一个注册表项指向它 ✓）
4. 若项目里还有 `linux/nvmet_teardown.sh` 之类的清理脚本，**先不要运行** ✓

## 背景（不必回报）

Windows 那边的内核驱动经历了 9 次蓝屏/卡死，靠**崩溃转储 + 反汇编 + NDKPI 头文件**把缺陷逐个钉死（等待链表损坏、MR flags 非法枚举值、关闭回调签名与头文件不符、释放路径挂死……），现在：

```
NdkOpenAdapter ok / CreatePd / CreateCq / CreateQp / CreateConnector / CreateMr / RegisterMr 全部 0x00000000 ✓
region registered: rkey / lkey 已到手 ✓
connecting to … → NdkConnect = 0xC0000236 ✗   ← 之前一直在连"自己"，所以被拒
```

**现在地址搞清楚了** ✓：Windows 侧要从 `192.168.100.2` 连到 **`192.168.100.5`（你）** ✓ —— 两台机器、两个地址，这才是正常的 NVMe-oF 拓扑 ✓。**你把目标拉起来，那条连接就会成立** ✓，接下来就是 Fabrics Connect → Property Get CAP → NVMe 读 + RDMA Read + 字节比对 ✓✓✓
