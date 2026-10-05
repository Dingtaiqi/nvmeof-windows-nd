#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  peer_ref_acl2.sh - give LIO the ACL it wants, with authentication OFF.
#
#  dmesg named the first half of this: "iSCSI Initiator Node: ... is not
#  authorized to access iSCSI target portal group: 1".  The second half is that an
#  ACL in LIO defaults its own `attrib/authentication` to on, and the Windows
#  initiator offers AuthMethod=None - so the login dies in negotiation with nothing
#  on the wire to show for it.  Both settings are explicit here so the next capture
#  is about the PROTOCOL and not about this target's policy.
#
#  Run as root.
# ===========================================================================
IQN="iqn.2024-01.local.rdma:linuxtarget"
CFG=/sys/kernel/config/target
INIT="iqn.1991-05.com.microsoft:yinshibai"

echo 0 > "$CFG/iscsi/$IQN/tpgt_1/enable" 2>/dev/null
sleep 1
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0"
ln -sfn "$CFG/core/fileio_1/refdisk" "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0/refdisk"
echo 0 > "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/attrib/authentication" 2>/dev/null
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"
sleep 1

echo "== state"
echo "   auth required : $(cat "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/attrib/authentication" 2>/dev/null)"
echo "   acl luns      : $(ls "$CFG/iscsi/$IQN/tpgt_1/acls/$INIT/lun_0/" 2>/dev/null | tr '\n' ' ')"
echo "   tpg luns      : $(ls "$CFG/iscsi/$IQN/tpgt_1/lun/lun_0/" 2>/dev/null | tr '\n' ' ')"
ss -ltn 2>/dev/null | grep -E '3260|3261' | sed 's/^/   /'
