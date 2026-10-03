#!/bin/bash
# SPDX-License-Identifier: MIT
# Point the Linux nvmet namespace at the real 1 TB disk instead of the RAM disk, with safety checks.
# Run as root from the laptop.  Written to a file because PowerShell -> ssh -> bash loses quotes.
set -u
NQN=nqn.2024-01.local.rdma:linux-nvmet
NS=/sys/kernel/config/nvmet/subsystems/$NQN/namespaces/1
DISK=/dev/nvme1n1

echo "== 0. current namespace =="
cat "$NS/device_path" 2>/dev/null || echo "  (none)"
cat "$NS/enable" 2>/dev/null

echo "== 1. safety checks on $DISK =="
if mount | grep -q "^$DISK"; then echo "  [REFUSE] $DISK is mounted"; exit 1; fi
if [ -n "$(lsblk -n -o MOUNTPOINT $DISK 2>/dev/null | tr -d ' \n')" ]; then
  echo "  [REFUSE] a partition of $DISK is mounted"; lsblk $DISK; exit 1
fi
holders=$(ls /sys/block/$(basename $DISK)/holders/ 2>/dev/null | tr -d '\n')
if [ -n "$holders" ]; then echo "  [REFUSE] $DISK has holders: $holders"; exit 1; fi
echo "  ok: not mounted, no holders"
echo "  size: $(blockdev --getsize64 $DISK 2>/dev/null) bytes"

echo "== 2. switch the namespace to the real disk =="
echo 0 > "$NS/enable"
# nvmet wants the size reset when the backing device changes; a block device reports its own.
echo -n "$DISK" > "$NS/device_path"
echo 1 > "$NS/enable"
echo "  device_path -> $(cat $NS/device_path)"
echo "  enable      -> $(cat $NS/enable)"

echo "== 3. what a host will now see =="
echo "  nsze bytes: $(blockdev --getsize64 $DISK 2>/dev/null)"
echo "  sectors   : $(( $(blockdev --getsize64 $DISK 2>/dev/null) / 512 ))"
echo "== NS-SWITCH-DONE =="
