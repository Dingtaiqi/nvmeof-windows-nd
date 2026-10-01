#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  peer_lio_up.sh - bring up Linux's own iSCSI target (LIO, in-kernel) as a
#  REFERENCE implementation to copy the login bytes from.
#
#  Why: this project's rule is "read the reference implementation, do not write
#  from memory" (DESIGN 8.29/8.40/8.44 paid for it five times).  The Windows
#  iSCSI initiator is a closed-source black box, but LIO is open AND is known to
#  work with it - so the fastest way to learn what a target must send at the
#  normal-session security stage is to watch one that Windows accepts.
#
#  Run as root.  Idempotent enough to re-run.
# ===========================================================================
set -e
IQN="iqn.2024-01.local.rdma:linuxtarget"
IMG=/var/tmp/lio-ref-disk.img
SIZE=16777216          # 16 MiB: the reference only has to answer, not be big

echo "== modules"
modprobe iscsi_target_mod
modprobe target_core_mod
modprobe target_core_file
lsmod | grep -E 'iscsi_target|target_core' | sed 's/^/   /'

echo "== configfs target tree"
CFG=/sys/kernel/config/target
[ -d "$CFG" ] || { mount -t configfs none /sys/kernel/config; }
mkdir -p "$CFG/core"
mkdir -p "$CFG/iscsi"

# ---- backstore: a plain file ----
if [ ! -d "$CFG/core/fileio_1" ]; then
    [ -f "$IMG" ] || dd if=/dev/zero of="$IMG" bs=1M count=$((SIZE / 1048576)) status=none
    mkdir -p "$CFG/core/fileio_1/refdisk"
    echo "fd_dev_name=$IMG,fd_dev_size=$SIZE" > "$CFG/core/fileio_1/refdisk/control"
    echo 1 > "$CFG/core/fileio_1/refdisk/enable"
    echo "   fileio_1/refdisk backed by $IMG"
else
    echo "   fileio_1/refdisk already present"
fi

# ---- the iSCSI target itself ----
mkdir -p "$CFG/iscsi/$IQN"
mkdir -p "$CFG/iscsi/$IQN/tpgt_1"
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/lun/lun_0"
if [ ! -L "$CFG/iscsi/$IQN/tpgt_1/lun/lun_0/refdisk" ]; then
    ln -s "$CFG/core/fileio_1/refdisk" "$CFG/iscsi/$IQN/tpgt_1/lun/lun_0/refdisk"
    echo "   lun_0 -> fileio_1/refdisk"
fi
# No CHAP: the point is to capture the LOGIN, and authentication would add noise.
echo 0 > "$CFG/iscsi/discovery_auth/enforce_discovery_auth" 2>/dev/null || true
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"

echo "== firewall"
if command -v firewall-cmd >/dev/null 2>&1; then
    firewall-cmd --add-port=3260/tcp >/dev/null 2>&1 || true
    echo "   3260/tcp opened (or firewalld absent)"
fi

echo "== listening?"
(ss -ltnp 2>/dev/null || netstat -ltnp 2>/dev/null) | grep -E ':3260' | sed 's/^/   /' || echo "   NOT listening (check dmesg)"

echo ""
echo "REFERENCE TARGET READY"
echo "   iqn      : $IQN"
echo "   portal   : 192.168.1.9:3260 (and every other address on this host)"
echo "   backstore: $IMG ($SIZE bytes)"
