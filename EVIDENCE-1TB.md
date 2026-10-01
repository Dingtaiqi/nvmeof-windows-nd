# 1 TB Linux 盘挂到 Windows 上：证据记录

日期：2026-10-01。本文件只放**实测输出**，每条都能重跑。

## 0. 拓扑（这是关键，之前判断错了一次）

Windows 的两个 40G CX3 口是**互插的一对**；Linux 笔记本只在千兆 LAN 上。
`src/interop_link.ps1` 把 LAN 网卡与 CX3 口 A 桥接，让 LAN 段和 CX3 直连段合成一个 L2，
CX3 口 B 留作 RoCE 端点 —— 于是对端的软件 RoCE 就能被 ARP 到：

```
ping 192.168.100.5 = True
Get-NetNeighbor 192.168.100.5 -> BE-74-DA-40-ED-E3  Reachable  以太网 24
以太网 24 = 192.168.100.2（RoCE 端点）   网桥 = 192.168.1.2（LAN 保持可用）
MTU: 以太网 24 -> 1500，RoCE Max Frame -> 1024
```

## 1. Linux 侧：导出的是那块真盘

```
$ sudo -n nvme id-ns /dev/nvme1n1
nsze    : 0x74706db0          (= 1953525168 个 512 B 扇区 = 931.51 GiB)
ncap    : 0x74706db0
nuse    : 0x74706db0
lbaf  0 : ms:0   lbads:9  rp:0x1 (in use)      <- 512 B，正在用
lbaf  1 : ms:0   lbads:12 rp:0

$ sudo -n nvme smart-log /dev/nvme1n1          <- 基线（挂载前）
Data Units Read     : 144240893 (73.85 TB)
Data Units Written  : 32884752 (16.84 TB)
```

nvmet 导出（未挂载、无 holders，安全检查通过）：
`/sys/kernel/config/nvmet/subsystems/nqn.2024-01.local.rdma:linux-nvmet/namespaces/1/device_path = /dev/nvme1n1`，
端口 1 = `rdma 192.168.100.5:4420 ipv4`，`enable=1`，`allow_any_host=1`。

## 2. 桥：iSCSI 进、NVMe-oF/RDMA 出，只读

```
> f5_interop.exe -initiator 192.168.100.5 4420 192.168.100.2 ^
                 -subnqn nqn.2024-01.local.rdma:linux-nvmet -iscsi 3260

[PASS] admin RDMA-CM connection to the target
[PASS] fabrics Connect qid 0 (the target's NQN was found) - cntlid=1
[PASS] I/O queue pair connected
[PASS] fabrics Connect qid 1 on its own queue pair
-- iSCSI BRIDGE
    backend : NVMe-oF namespace nsid=1, 1953525168 blocks x 512 B = 931.51 GiB
    identity: serial '4888a61af919cf3f63b5', model 'Linux'
    access  : READ-ONLY (default)
  [iscsi] listening on 127.0.0.1:3260, IQN iqn.2024-01.com.nvmeof:bridge0, 65536-byte chunks
```

## 3. Windows 侧：盘出现了、在线、可读

```
> Get-Disk | ? BusType -eq iSCSI
Number FriendlyName        SerialNumber         PartitionStyle OperationalStatus SizeGB IsReadOnly
     3 NVMEOF iSCSI-NVMeoF 4888a61af919cf3f63b5 GPT            Online            931.51       True

> Get-Disk 3 | Get-Partition
PartitionNumber DriveLetter SizeMB   Type      GptType
              1               300     System    {c12a7328-f81f-11d2-ba4b-00a0c93ec93b}   ESP
              2                16     Reserved  {e3c9e316-0b5c-4db8-817d-f92df00215ae}   MSR
              3           E    953551.7 Basic    {ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}   NTFS

> Get-Disk 3 | Get-Partition | Get-Volume
DriveLetter FileSystemLabel FileSystem SizeGB HealthStatus
          E                 NTFS        931.2 Healthy
```

`E:\` 根目录（真实内容、真实时间戳；共 60 项。**目录名属于私人内容，这里只给形状**）：

```
d--hs- 2026/2/3   $RECYCLE.BIN            （其余 59 项 = 具名目录与文件，此处隐去）
d--hs- 2026/9/28  System Volume Information
-a---- 2026/8/1   3566.03 MB  <发行版>-x86_64.iso
-a-hs- 2026/9/30  2944 MB     pagefile.sys
-a---- 2026/8/2   0.02 MB     telemetry.db        <- 第 4 节的字节级对照用的就是它
```

（顶层共 60 项，名字按发布需要隐去；证据本身不依赖这些名字——下面的两次 sha256 对照才是。）

## 4. 内容是真的：两次字节级对照（Windows 读到的东西 == Linux 盘上的东西）

**(a) 经文件系统读一个文件，再在 Linux 侧裸读它所在的物理块**

```
> fsutil file queryextents E:\telemetry.db
VCN: 0x0   Clusters: 0x5   LCN: 0xce2e

分区 3 偏移 = 333447168 字节；每簇 4096 字节（fsutil: Bytes Per Cluster 4096, 512 B/sector）
绝对偏移 = 333447168 + 0xce2e*4096 = 549642240 -> 绝对 LBA 1073520

Windows（经 E:）          前 16 字节: 53 51 4C 69 74 65 20 66 6F 72 6D 61 74 20 33 00
Linux（裸读 LBA 1073520） 前 16 字节: 53 51 4c 69 74 65 20 66 6f 72 6d 61 74 20 33 00
Windows sha256(512) : DEC615AE97B8B81CD5E395FD4F9B1FD379332C3516A16AB122C1C1A57A5DD397
Linux   sha256(512) : dec615ae97b8b81cd5e395fd4f9b1fd379332c3516a16ab122c1c1a57a5dd397
```

第一次算错了 LBA（忘了分区偏移），Linux 侧读出全 0；补上偏移后逐字节相同 —— 记在这里，
因为"看起来不一致"的那一步是**我的算术**，不是桥。

**(b) 裸读 ESP 引导扇区（另一个区域，另一条路径）**

```
Windows 读 \\.\PhysicalDrive3 偏移 20480（LBA 40）: sha256 802b1462817444dcb2111f2cb62a54f45c2da13ac8229b82dcb1b711a2452293
Linux   sudo nvme read /dev/nvme1n1 -s 40 -c 0 -z 512: sha256 802b1462817444dcb2111f2cb62a54f45c2da13ac8229b82dcb1b711a2452293
OemName 两侧都是 "MSDOS5.0"
```

**(c) 卷标：这块盘本身没有卷标，不是读不出来**

```
ESP 卷标字段(0x47, 11 字节)  Windows: '           '   Linux: 全部空格
NTFS:  > fsutil fsinfo volumeinfo E:
       Volume Name :                     <- 空
       > Get-Volume -DriveLetter E | Select FileSystemLabel   <- 空
```

两个卷（ESP/FAT32 与 NTFS）的卷标字段都是空，两侧读到的字节完全一致；路径是通的，
盘上确实没写过卷标。

## 5. 全程零写入

```
SMART 基线（挂载前） Data Units Written : 32884752
SMART 之后           Data Units Written : 32884752      <- 一点没变
                     Data Units Read    : 144241018     <- 从 144240893 涨上来，读是真的发生了

> New-Item E:\__rdma_write_probe.tmp
写入被拒绝: The media is write protected.

桥日志： [iscsi] WRITE lba=651264 blocks=16 REFUSED (read-only bridge)
         [iscsi] -> itt 0x0000003D status 0x02 residual 0 sense 18      <- CHECK CONDITION + 18 字节 sense
```

LBA 651264 正好是 NTFS 卷的第一个扇区（333447168/512 = 651264），也就是 Windows 挂载时
想写的那个"脏位"——被桥按只读拒绝了。命令直方图（747 条 CDB）：

```
584 x 28  READ(10)        48 x 25  READ CAPACITY(10)    9 x 12  INQUIRY
 98 x 1A  MODE SENSE(6)    3 x 00  TEST UNIT READY      2 x A0  REPORT LUNS
  1 x 2A  WRITE(10) <- 唯一一次写尝试，被拒
  1 x 35  SYNCHRONIZE CACHE     1 x 9E  READ CAPACITY(16)
```

## 6. 已知的小问题（不影响结论，但要记下来）

1. 桥与 nvmet 之间的 keep-alive 在 584 次连续读期间**超时过 2 次**（`keep-alive to the NVMe
   target failed`）。会话没有掉、数据没有错，但 KATO 120 s + 大读流下我的 `tick()` 等待
   可能需要放宽或重试——属于健壮性项。
2. 这一轮之前我曾在汇报里说"1 TB 盘卡在物理链路、需要买网卡"——**那是错的**，
   本仓库的 `interop_link.ps1` 早就解决了这件事（桥接 LAN 网卡与一个 CX3 口）。
   教训与 §8.55(2) 同一条：先读自己的仓库，再下结论。

## 7. 复现

```powershell
.\src\interop_link.ps1 -Action Up -PeerIp 192.168.100.5 ; .\src\interop_link.ps1 -Action LowMtu
Start-Process .\src\f5_interop.exe -ArgumentList '-initiator','192.168.100.5','4420','192.168.100.2',
    '-subnqn','nqn.2024-01.local.rdma:linux-nvmet','-iscsi','3260'
iscsicli AddTargetPortal 127.0.0.1 3260
Connect-IscsiTarget -NodeAddress iqn.2024-01.com.nvmeof:bridge0 -IsPersistent $false
Get-Disk | Where-Object BusType -eq 'iSCSI'
Get-ChildItem E:\

# 收工（把 LAN/CX3 的绑定还给系统）
.\src\interop_link.ps1 -Action Down ; .\src\interop_link.ps1 -Action RestoreMtu
```

## 8. 拆除记录（2026-10-01，用户确认后执行）

顺序很重要，第一次按"直接登出"走是被拒的，记下来：

```
1) 摘盘符      Remove-PartitionAccessPath -DiskNumber 3 -PartitionNumber 3 -AccessPath 'E:\'   -> ok
2) 盘离线      Set-Disk -DiskNumber 3 -IsOffline $true                                        -> ok
3) 停桥        Stop-Process -Name f5_interop      （释放到 nvmet 的 RDMA 连接）
4) 拆网桥      .\interop_link.ps1 -Action Down     （同时把 MTU 还原：以太网 23/24 = 4074,
                                                    Jumbo 4088, RoCE Max Frame 2048）
5) 登出会话    iscsicli LogoutTarget <session-id>  -> 成功
6) 清门户      iscsicli RemoveTargetPortal 192.168.1.9 3260 / 3262
7) 清缓存      Update-IscsiTarget                  -> 目标列表变空
```

踩到的三个坑：

1. **`Disconnect-IscsiTarget` 在非交互环境下必然失败**：它内部会弹确认
   （`Windows PowerShell is in NonInteractive mode. Read and Prompt functionality is not available.`），
   `-Confirm:$false` 也绕不过。用 `iscsicli LogoutTarget <SessionId>`。
2. **只要卷还挂着，登出一定是 `The session cannot be logged out since a device on that
   session is currently being used.`**，而且**盘离线也没用**；必须先摘盘符、把盘设为 Offline。
   另外它拒绝时 `Restart-Service msiscsi -Force` 也**不会**清掉会话（会话在 iScsiPrt 里活着），
   别指望重启服务。
3. **`Get-Volume -DiskNumber` 在这台机器的 PowerShell 上不存在**（得走
   `Get-Partition -DiskNumber N | Get-Volume`）；另外我一度用
   `@(Get-Disk | ... | Measure-Object).Count` 统计——`Measure-Object` 被包进了 `@()`，
   结果永远是 1。这两个都是**我自己的检查写错了**，不是系统状态。

最终状态（全部实测）：

```
iSCSI 盘        0        iSCSI 会话   0        iSCSI 目标列表  空
iSCSI 门户      只剩用户自己的 NAS：192.168.1.114:3260
网桥适配器      0        f5_interop 进程 0     E: 卷  0
地址           以太网 24 = 192.168.100.2   以太网 23 = 192.168.100.3   以太网 20 = 192.168.1.2
MTU            以太网 23/24 = 4074（与实验前一致）    以太网 20 = 1500
Get-Disk       只剩本机自己的盘：ST1000DM003(SATA) / GIGABYTE 256G(NVMe) / YCY_256GB(NVMe)
```

**对端（Linux）**：用户已关机，因此没有做对端拆除。nvmet 的配置在 configfs 里，**不跨重启保留**，
所以重启后那块 1 TB 盘不再对外导出；`192.168.100.5/24` 是脚本用 `ip addr add` 临时加的，
重启后大概也没了。下次要用，按第 7 节从头跑一遍即可（对端两条 + Windows 三条）。
关机前的 SMART 读数（写入量 32884752，与挂载前完全一致）已经落在第 5 节，本次拆除没有再写入任何东西。

