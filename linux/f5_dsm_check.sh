#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  f5_dsm_check.sh - which DSM form actually deallocates?
#
#  `nvme dsm -d` sets the AD bit in COMMAND DWORD 11 only; `-a 4` sets it in each
#  range's context attributes.  The kernel's own discard path (nvme_setup_discard)
#  sets BOTH.  This target honoured only the per-range bit, so `nvme dsm -s X -b 8 -d`
#  came back rc=0 and changed nothing - and "rc=0 and nothing happened" is the one
#  outcome no test would have caught without reading the medium back.
#
#  Runs the same probe against whatever is at <ip>:<port>, so the reference and our
#  target can be compared row by row instead of guessed at.
#
#  Usage: ./f5_dsm_check.sh [ip] [port] [subnqn]
# ===========================================================================
set -e
IP="${1:-192.168.100.5}"; PORT="${2:-4420}"; SUBNQN="${3:-nqn.2024-01.local.rdma:linux-nvmet}"
MODEL="${4:-Linux|NDVMEOF}"
NVME=/usr/bin/nvme
W=$(mktemp -d)
disconnect() { sudo -n $NVME disconnect -n "$SUBNQN" >/dev/null 2>&1 || true; }
trap 'rm -rf "$W"; disconnect' EXIT INT TERM
disconnect
sudo -n $NVME connect -t rdma -a "$IP" -s "$PORT" -n "$SUBNQN" >/dev/null 2>&1 || { echo "connect failed" >&2; exit 1; }
# Match a MODEL on a line that starts with a device path - the first version of this
# used an awk pattern with an empty variable, which matched the header line and made
# `DEV` the literal string "Node".
DEV=""; i=0
while [ "$i" -lt 30 ]; do
    DEV=$(sudo -n $NVME list 2>/dev/null | awk -v m="$MODEL" '$1 ~ /^\/dev\// && $0 ~ m {print $1; exit}')
    [ -n "$DEV" ] && break
    i=$((i+1)); sleep 1
done
[ -n "$DEV" ] || { echo "no device matching '$MODEL' appeared" >&2; exit 1; }
echo "== $IP:$PORT  namespace $DEV"

SLBA=${SLBA:-4000}
dd if=/dev/urandom of="$W/pat" bs=512 count=8 status=none

zeros() { [ "$(tr -d '\0' < "$1" | wc -c)" -eq 0 ] && echo yes || echo no; }

round() {
    label=$1; shift
    sudo -n $NVME write "$DEV" -s "$SLBA" -c 7 -b 512 -z 4096 -d "$W/pat"
    sudo -n $NVME read  "$DEV" -s "$SLBA" -c 7 -b 512 -z 4096 -d "$W/pre"
    cmp -s "$W/pat" "$W/pre" && landed=yes || landed=no
    if out=$(sudo -n $NVME "$@" "$DEV" 2>&1); then rc=0; else rc=$?; fi
    sudo -n $NVME read  "$DEV" -s "$SLBA" -c 7 -b 512 -z 4096 -d "$W/post"
    printf '  %-34s pattern landed=%-3s dsm rc=%-3s reads back zero: %s\n' \
           "$label" "$landed" "$rc" "$(zeros "$W/post")"
}

round "dsm -b 8 -d   (CDW11 AD only)"  dsm -s "$SLBA" -b 8 -d
round "dsm -b 8 -a 4 (per-range AD)"  dsm -s "$SLBA" -b 8 -a 4
round "dsm -b 8 -a 4 -d (both)"       dsm -s "$SLBA" -b 8 -a 4 -d
round "dsm -b 8    (neither)"         dsm -s "$SLBA" -b 8

disconnect
