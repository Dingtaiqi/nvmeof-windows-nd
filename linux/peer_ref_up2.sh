#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  peer_ref_up2.sh - LIO on loopback + the logging proxy on the LAN address.
#
#  Layout, and why:
#      Windows --3260--> 192.168.1.9 (proxy) --3260--> 127.0.0.1 (LIO, reference)
#  Windows gets the standard iSCSI port (no portal-port oddities), and every byte
#  LIO sends passes through the proxy and lands in /tmp/lio_proxy.log.
#
#  Also: demo_mode + an ACL for the Windows initiator, because LIO denies unknown
#  initiators by default - which is what a "connection succeeded, then nothing"
#  looks like from the outside.
#
#  Run as root.
# ===========================================================================
IQN="iqn.2024-01.local.rdma:linuxtarget"
CFG=/sys/kernel/config/target
INIT="iqn.1991-05.com.microsoft:yinshibai"

echo "== network portal -> 127.0.0.1:3260"
echo 0 > "$CFG/iscsi/$IQN/tpgt_1/enable" 2>/dev/null
for d in "$CFG/iscsi/$IQN/tpgt_1/np"/*; do
    [ -d "$d" ] && rmdir "$d" 2>/dev/null
done
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/np/127.0.0.1:3260" && echo "   np ok" || echo "   np FAILED"

echo "== access control: demo_mode + ACL for $INIT"
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/attrib/demo_mode"
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT"
echo "   demo_mode=$(cat "$CFG/iscsi/$IQN/tpgt_1/attrib/demo_mode") acls=[$(ls "$CFG/iscsi/$IQN/tpgt_1/acls/" | tr '\n' ' ')]"

echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"
sleep 2
echo "== listener on loopback"
ss -ltn 2>/dev/null | grep 3260 | sed 's/^/   /' || echo "   none"

echo "== proxy on 192.168.1.9:3260 -> 127.0.0.1:3260"
pkill -f lio_proxy.py 2>/dev/null
sleep 1
rm -f /tmp/lio_proxy.log /tmp/lio_proxy.out
nohup python3 /tmp/lio_proxy.py 192.168.1.9:3260 127.0.0.1:3260 /tmp/lio_proxy.log >/tmp/lio_proxy.out 2>&1 &
sleep 2
chmod 644 /tmp/lio_proxy.log 2>/dev/null
cat /tmp/lio_proxy.out
ss -ltn 2>/dev/null | grep 3260 | sed 's/^/   /'
