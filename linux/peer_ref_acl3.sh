#!/bin/sh
# ===========================================================================
#  peer_ref_acl3.sh - finish the LIO reference: a LUN the ACL can actually use.
#
#  Progress: Windows now completes the normal-session login against LIO, so
#  "Authorization Failure"/"Login negotiation failed" were this target's policy
#  (ACL with authentication on), not a protocol problem.  What is left is the LUN:
#  LIO refuses to export the same backstore at the TPG level AND inside an ACL, and
#  the symlink into the ACL failed with "Bad address" while refdisk was still mapped
#  at tpgt_1/lun/lun_0.  With an ACL present the TPG-level mapping is ignored anyway,
#  so it goes away.
#
#  Run as root.
# ===========================================================================
IQN="iqn.2024-01.local.rdma:linuxtarget"
CFG=/sys/kernel/config/target
INIT="iqn.1991-05.com.microsoft:yinshibai"

echo 0 > "$CFG/iscsi/$IQN/tpgt_1/enable" 2>/dev/null
sleep 1

echo "== drop the TPG-level LUN mapping (the ACL is what counts now)"
rm -f "$CFG/iscsi/$IQN/tpgt_1/lun/lun_0/refdisk"
echo "   tpg luns now: [$(ls "$CFG/iscsi/$IQN/tpgt_1/lun/lun_0/" 2>/dev/null | tr '\n' ' ')]"

echo "== map the backstore inside the ACL"
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0"
ln -sfn "$CFG/core/fileio_1/refdisk" "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0/refdisk" \
    && echo "   acl lun linked" || echo "   acl lun link FAILED"
echo 0 > "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/attrib/authentication" 2>/dev/null

echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"
sleep 1
echo "== state"
echo "   auth required : $(cat "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/attrib/authentication" 2>/dev/null)"
echo "   acl luns      : [$(ls "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0/" 2>/dev/null | tr '\n' ' ')]"
ss -ltn 2>/dev/null | grep -E '3260|3261' | sed 's/^/   /'
