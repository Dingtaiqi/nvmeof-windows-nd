#!/bin/sh
# ===========================================================================
#  peer_ref_acl.sh - let the Windows initiator actually LOG IN to the LIO
#  reference target, so the NORMAL-session login bytes can be captured.
#
#  LIO denies a login when an ACL exists for the initiator but no LUN is mapped
#  into it: the ACL restricts, and an ACL with nothing mapped is "no LUNs for
#  you" - which reaches the initiator as a failed connect with no detail.
#
#  Run as root.
# ===========================================================================
IQN="iqn.2024-01.local.rdma:linuxtarget"
CFG=/sys/kernel/config/target
INIT="iqn.1991-05.com.microsoft:yinshibai"

echo "== map the LUN into the ACL for $INIT"
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0"
if [ ! -L "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0/refdisk" ]; then
    ln -s "$CFG/core/fileio_1/refdisk" "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0/refdisk"
    echo "   acl lun_0 -> fileio_1/refdisk"
fi
# demo_mode needs the TPG disabled to be written at all; try it, do not depend on it.
echo 0 > "$CFG/iscsi/$IQN/tpgt_1/enable"
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/attrib/demo_mode" 2>/dev/null && echo "   demo_mode=1" || echo "   demo_mode not writable (ACL is the mechanism)"
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"
sleep 1

echo "== state"
echo "   demo_mode : $(cat "$CFG/iscsi/$IQN/tpgt_1/attrib/demo_mode" 2>/dev/null)"
echo "   acls      : $(ls "$CFG/iscsi/$IQN/tpgt_1/acls/" 2>/dev/null | tr '\n' ' ')"
echo "   acl luns  : $(ls "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/" 2>/dev/null | tr '\n' ' ')"
ss -ltn 2>/dev/null | grep 3260 | sed 's/^/   /'
