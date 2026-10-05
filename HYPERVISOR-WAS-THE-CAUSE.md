# 13 次蓝屏的根因：hypervisor（不是驱动）

## 决定性实验
| hypervisor | NdkOpenAdapter | 走到连接路径时 |
|---|---|---|
| **运行中** | failed 0xC0010011 | **0x139 KERNEL_SECURITY_CHECK_FAILURE (P1=0xa incorrect stack)** ✗ |
| **停止后**（bcdedit /set hypervisorlaunchtype off） | **ok** ✓ | **优雅失败，机器完全稳定** ✓✓✓ |

同一份驱动二进制、同一条连接代码 —— 唯一变量是 hypervisor。

## 机理
hypervisor 接管 DMA 重映射/IOMMU → Mellanox (WinOF 5.50, CX3) 的 NDK 提供程序无法建立 →
返回 0xC0010011（也解释了 SriovSupport = NoIoMmuSupport）→ 半初始化的 CM 状态在后续操作里
触发 FAST_FAIL_INCORRECT_STACK。驱动只是恰好在那个时刻调用了它。

## 顺带确证的硬件事实
- 两个 40G 口是自环互插的一对（收发计数完全互补）
- 以太网 23 是网桥成员，是通往局域网那条路，不可动
- 对端是 EndeavourOS 笔记本 + 千兆网卡上的软件 RoCE (rda_rxe)，192.168.100.5
- **同卡回环被 RDMA-CM 拒绝** → 必须两台机器

## 驱动侧真缺陷（都修了，都有实证）
1. StepStart 每次 KeInitializeEvent → 破坏等待链表（转储实证）
2. NdkRegisterMr flags 非法枚举 0xF（ndkpi.h：ALLOW_REMOTE_WRITE 是 0x5 不是位）
3. closeDone 签名 2 参数 ≠ NDK_FN_CLOSE_COMPLETION 的 1 参数（ndkpi.h）
4. WskReleaseProviderNPI 在 NDK 对象仍打开时挂死（Start Pending，无转储）
5. NdkGetConnectionData 的 pPrivateDataLength 是 IN/OUT，传了未初始化栈变量（ndkpi.h）
6. teardown 连续 5 次复用同一上下文

## 当前状态
- 内核 NDK 通路**全绿且稳定** ✓（hypervisor 关闭时）
- 只差一个**活的监听目标**（笔记本的 nvmet 需重新拉起），即可完成
  NdkConnect → Fabrics Connect → Property Get CAP → NVMe 读 + RDMA Read + 字节比对

## 机器设置改动（可还原）
`bcdedit /set hypervisorlaunchtype off` —— 会让虚拟机/WSL2/沙盒不可用；
还原：`bcdedit /set hypervisorlaunchtype auto`