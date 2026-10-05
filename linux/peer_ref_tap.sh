#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  peer_ref_tap.sh - final arrangement for capturing a NORMAL iSCSI session.
#
#      Windows -> 192.168.1.9:3260 (proxy) -> 192.168.1.9:3261 (LIO, reference)
#
#  LIO binds 3261 and therefore advertises "TargetAddress=192.168.1.9:3261,1" in
#  its SendTargets reply.  The proxy rewrites that to :3260 - a ONE-BYTE-FOR-ONE-BYTE
#  substitution (same host, 3261 -> 3260), so no length field has to be recomputed -
#  and the initiator therefore opens the normal session back into the proxy instead
#  of straight to the target.  Without that, half the handshake is invisible, which
#  is what happened on the first three attempts at this capture.
#
#  Run as root.
# ===========================================================================
IQN="iqn.2024-01.local.rdma:linuxtarget"
CFG=/sys/kernel/config/target

echo "== LIO moves to 192.168.1.9:3261"
echo 0 > "$CFG/iscsi/$IQN/tpgt_1/enable" 2>/dev/null
for d in "$CFG/iscsi/$IQN/tpgt_1/np"/*; do
    [ -d "$d" ] && rmdir "$d" 2>/dev/null
done
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/np/192.168.1.9:3261" && echo "   np ok" || echo "   np FAILED"
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"
sleep 2
ss -ltn 2>/dev/null | grep -E '3260|3261' | sed 's/^/   /'

echo "== firewall for 3260 and 3261"
firewall-cmd --add-port=3260/tcp >/dev/null 2>&1 || true
firewall-cmd --add-port=3261/tcp >/dev/null 2>&1 || true

echo "== proxy on 192.168.1.9:3260 with the address patch"
pkill -f lio_proxy.py 2>/dev/null
sleep 1
rm -f /tmp/lio_tap.log
setsid nohup python3 /tmp/lio_proxy.py 192.168.1.9:3260 192.168.1.9:3261 \
      /tmp/lio_tap.log "192.168.1.9:3261" "192.168.1.9:3260" \
      </dev/null >/tmp/lio_tap.out 2>&1 &
sleep 2
cat /tmp/lio_tap.out
ss -ltn 2>/dev/null | grep -E '3260|3261' | sed 's/^/   /'
