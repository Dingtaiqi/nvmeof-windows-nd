#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  peer_ref_up.sh - finish bringing up the LIO reference target and start the
#  logging proxy in front of it.  Run as root.
#
#  Two things learned the hard way just before this: LIO's network-portal
#  directory must be named "<ip>:<port>" (dmesg: "Unable to locate ':port' in
#  IPv4 iSCSI network portal address"), and the peer has no tcpdump - hence the
#  Python proxy, which shows the bytes both ways anyway.
# ===========================================================================
IQN="iqn.2024-01.local.rdma:linuxtarget"
CFG=/sys/kernel/config/target
NP="192.168.1.9:3260"

echo "== network portal $NP"
echo 0 > "$CFG/iscsi/$IQN/tpgt_1/enable" 2>/dev/null
for d in "$CFG/iscsi/$IQN/tpgt_1/np"/*; do
    [ -d "$d" ] && rmdir "$d" 2>/dev/null
done
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/np/$NP" && echo "   np created" || echo "   np FAILED"
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"
sleep 2

echo "== listeners on 3260"
ss -ltn 2>/dev/null | grep 3260 || netstat -ltn 2>/dev/null | grep 3260 || echo "   none"

echo "== restart the logging proxy (3261 -> 3260)"
pkill -f lio_proxy.py 2>/dev/null
sleep 1
rm -f /tmp/lio_proxy.log /tmp/lio_proxy.out
nohup python3 /tmp/lio_proxy.py 3261 3260 /tmp/lio_proxy.log >/tmp/lio_proxy.out 2>&1 &
sleep 2
chmod 644 /tmp/lio_proxy.log 2>/dev/null
cat /tmp/lio_proxy.out
ss -ltn 2>/dev/null | grep 3261 || echo "   proxy NOT listening"
