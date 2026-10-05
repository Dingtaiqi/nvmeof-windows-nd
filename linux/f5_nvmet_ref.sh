#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  f5_nvmet_ref.sh - the REFERENCE answers.
#
#  Runs the same command list as f5_dirb_demo.sh's sweep against Linux's own nvmet
#  (loopback on the peer, no bridge needed) and prints the raw status of each one.
#  Every "our target answered X" claim needs this row next to it, otherwise a
#  deviation is invisible: the whole point of having a second implementation is to
#  find the places where both of ours agree and nvmet does not.
#
#  Usage: ./f5_nvmet_ref.sh [ip] [port] [subnqn]
#  Defaults: 192.168.100.5  4420  nqn.2024-01.local.rdma:linux-nvmet
# ===========================================================================
set -e

IP="${1:-192.168.100.5}"
PORT="${2:-4420}"
SUBNQN="${3:-nqn.2024-01.local.rdma:linux-nvmet}"
NVME=/usr/bin/nvme

disconnect() { sudo -n $NVME disconnect -n "$SUBNQN" >/dev/null 2>&1 || true; }
trap 'disconnect' EXIT INT TERM

disconnect
sudo -n $NVME connect -t rdma -a "$IP" -s "$PORT" -n "$SUBNQN" >/dev/null 2>&1 || {
    echo "connect to $IP:$PORT failed - is nvmet up?" >&2; exit 1; }
DEV=""
i=0
while [ "$i" -lt 30 ]; do
    DEV=$(sudo -n $NVME list 2>/dev/null | awk '/Linux/{print $1}' | head -1)
    [ -n "$DEV" ] && break
    i=$((i+1)); sleep 1
done
[ -n "$DEV" ] || { echo "no Linux nvmet device appeared" >&2; exit 1; }
echo "== reference: Linux nvmet at $IP:$PORT, namespace $DEV"
echo "   nsze $(sudo -n $NVME id-ns "$DEV" 2>/dev/null | awk '/^nsze/{print $3}')"
echo ""

run() {
    label=$1; shift
    if out=$(sudo -n $NVME "$@" "$DEV" 2>&1); then rc=0; else rc=$?; fi
    st=$(printf '%s\n' "$out" | grep -o 'NVMe status:.*' | head -1)
    [ -n "$st" ] || st=$(printf '%s\n' "$out" | grep -v '^WARNING' | head -1)
    printf '  %-26s rc=%-3s %s\n' "$label" "$rc" "$(printf '%s' "$st" | cut -c1-96)"
}

echo "-- Get Features (fids our target refuses with Invalid Field)"
run get-feature-arbitration  get-feature -f 0x01
run get-feature-power-mgmt   get-feature -f 0x02
run get-feature-lba-range    get-feature -f 0x03
run get-feature-write-atomic get-feature -f 0x0a
run get-feature-auto-pst     get-feature -f 0x0c
run get-feature-timestamp    get-feature -f 0x0e
run get-feature-host-behavior get-feature -f 0x16
run get-feature-sanitize     get-feature -f 0x17

echo ""
echo "-- log pages"
run error-log                error-log
run smart-log                smart-log
run get-log-lid-13-512       get-log -i 0x0d -l 512

echo ""
echo "-- the rest"
run effects-log-csi1         effects-log -c 1
run write-zeroes             write-zeroes -s 2000 -c 7 -b 512
run dsm-deallocate           dsm -s 2000 -b 8 -d
run resv-report              resv-report
run resv-register            resv-register -c 1 -k 0x12345678
run sanitize                 sanitize
run format                   format -f

alive() {
    if sudo -n $NVME id-ctrl "$DEV" >/dev/null 2>&1; then echo "   still alive: yes"
    else echo "   still alive: NO"; fi
}
echo ""
echo "== liveness before the one command that killed OUR target:"
alive
# `nvme persistent-event-log` is what sent Get Log Page lid=13 with numd=0xFFFFFFFF
# and a 0-byte SGL.  Against our target that hung the admin queue until the host's
# Keep Alive timer expired.  Last on the list, with its own liveness check, so it
# cannot mask anything above it.
echo "-- the command that hung our target"
run persistent-event-log     persistent-event-log
echo ""
echo "== liveness after it:"
alive
disconnect
echo "   disconnected"
