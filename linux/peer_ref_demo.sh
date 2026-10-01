#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  peer_ref_demo.sh - take LIO's ACL out of the picture.
#
#  LIO answered every NORMAL-session login with "iSCSI Login negotiation failed"
#  (dmesg) while the DISCOVERY session went through - which is what an ACL that
#  does not match the initiator looks like.  demo_mode is LIO's "accept any
#  initiator" switch; to write it the TPG must be disabled AND have no ACLs, so the
#  ACL created earlier is removed here rather than left to confuse the test.
#
#  Run as root.
# ===========================================================================
IQN="iqn.2024-01.local.rdma:linuxtarget"
CFG=/sys/kernel/config/target
INIT="iqn.1991-05.com.microsoft:yinshibai"

echo "== disable TPG, drop the ACL, enable demo_mode"
echo 0 > "$CFG/iscsi/$IQN/tpgt_1/enable" 2>/dev/null
sleep 1
if [ -d "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT" ]; then
    rmdir "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0" 2>/dev/null
    rmdir "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT" 2>/dev/null
fi
for d in "$CFG/iscsi/$IQN/tpgt_1/acls"/*; do
    [ -d "$d" ] && { rmdir "$d"/lun_0 2>/dev/null; rmdir "$d" 2>/dev/null; }
done
if echo 1 > "$CFG/iscsi/$IQN/tpgt_1/attrib/demo_mode" 2>/dev/null; then
    echo "   demo_mode=1 (the ACL was the gate)"
else
    echo "   demo_mode STILL not writable - it is not the ACL"
fi
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"
sleep 1
echo "   demo_mode : $(cat "$CFG/iscsi/$IQN/tpgt_1/attrib/demo_mode" 2>/dev/null)"
echo "   acls      : [$(ls "$CFG/iscsi/$IQN/tpgt_1/acls/" 2>/dev/null | tr '\n' ' ')]"
echo "   luns      : [$(ls "$CFG/iscsi/$IQN/tpgt_1/lun/lun_0/" 2>/dev/null | tr '\n' ' ')]"
ss -ltn 2>/dev/null | grep -E '3260|3261' | sed 's/^/   /'
