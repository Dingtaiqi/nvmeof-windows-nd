#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  probe_mtu.sh - what MTU and read-atom resources does the rxe port have?
#
#  Written because two RDMA Reads between this peer and a ConnectX-3 failed with
#  "remote access error" and the answer has to come from facts about the port,
#  not from another guess.  Read-only: it changes nothing.
# ===========================================================================
IFACE="${1:-enp55s0}"

echo "=== link $IFACE ==="
ip -o link show "$IFACE" | sed 's/\\/ /'

echo "=== rdma link -d ==="
sudo -n /usr/bin/rdma link show -d 2>&1 | head -20

echo "=== ibv_devinfo -v (rxe0) ==="
ibv_devinfo -v -d rxe0 2>/dev/null | grep -iE 'fw_ver|node_guid|sys_image_guid|max_mr_size|page_size_cap|max_qp|max_sge|max_cqe|max_mr|max_pd|max_qp_rd_atom|max_ee_rd_atom|max_res_rd_atom|max_qp_init_rd_atom|max_srq|atomic_cap|max_ee_init_rd_atom|active_width|active_speed|phys_state|state|max_msg_sz' | head -40

echo "=== rxe port attributes (sysfs) ==="
for f in state rate lid sm_lid lmc link_layer; do
    printf '  %-12s = %s\n' "$f" "$(cat /sys/class/infiniband/rxe0/ports/1/$f 2>/dev/null)"
done

echo "=== netdev MTU ==="
printf '  mtu = %s\n' "$(cat /sys/class/net/$IFACE/mtu)"
