#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  f5_nvmet_ref2.sh - part two of the reference table: every log page id and the
#  data-structure features, so "our target deviates here" is a measured statement
#  rather than a guess.  Same shape as f5_nvmet_ref.sh; run after it.
# ===========================================================================
set -e
IP="${1:-192.168.100.5}"; PORT="${2:-4420}"; SUBNQN="${3:-nqn.2024-01.local.rdma:linux-nvmet}"
NVME=/usr/bin/nvme
disconnect() { sudo -n $NVME disconnect -n "$SUBNQN" >/dev/null 2>&1 || true; }
trap 'disconnect' EXIT INT TERM
disconnect
sudo -n $NVME connect -t rdma -a "$IP" -s "$PORT" -n "$SUBNQN" >/dev/null 2>&1 || { echo "connect failed" >&2; exit 1; }
DEV=""; i=0
while [ "$i" -lt 30 ]; do DEV=$(sudo -n $NVME list 2>/dev/null | awk '/Linux/{print $1}' | head -1); [ -n "$DEV" ] && break; i=$((i+1)); sleep 1; done
[ -n "$DEV" ] || { echo "no device" >&2; exit 1; }
echo "== reference 2: LIDs and data-structure features on $DEV"

one() { label=$1; shift; if out=$(sudo -n $NVME "$@" 2>&1); then rc=0; else rc=$?; fi
        st=$(printf '%s\n' "$out" | grep -o 'NVMe status:.*' | head -1)
        [ -n "$st" ] || st=$(printf '%s\n' "$out" | grep -v '^WARNING' | head -1)
        printf '  %-22s rc=%-3s %s\n' "$label" "$rc" "$(printf '%s' "$st" | cut -c1-84)"; }

echo ""
echo "-- Get Log Page, every LID a Linux host may ask for (512 bytes each)"
for lid in 0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0a 0x0b 0x0c 0x0d \
           0x0e 0x0f 0x10 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x18 0x19 0x1a 0x1b 0x70; do
    one "lid=$lid" get-log "$DEV" -i "$lid" -l 512
done

echo ""
echo "-- data-structure Get Features, with the length the structure needs"
one "fid=0x03 len=4096" get-feature "$DEV" -f 0x03 -l 4096
one "fid=0x0c len=256"  get-feature "$DEV" -f 0x0c -l 256
one "fid=0x0e len=8"    get-feature "$DEV" -f 0x0e -l 8
one "fid=0x16 len=4"    get-feature "$DEV" -f 0x16 -l 4
one "fid=0x16 len=512"  get-feature "$DEV" -f 0x16 -l 512
one "fid=0x17 len=4"    get-feature "$DEV" -f 0x17 -l 4

echo ""
echo "-- Set Features a host uses, no data"
one "set-fid=0x06 vwc=1"    set-feature "$DEV" -f 0x06 -v 1
one "set-fid=0x08 irqcoal"  set-feature "$DEV" -f 0x08 -v 0
one "set-fid=0x09 irqcfg"   set-feature "$DEV" -f 0x09 -v 0
one "set-fid=0x0b aen"      set-feature "$DEV" -f 0x0b -v 0x1f
one "set-fid=0x0c autopst"  set-feature "$DEV" -f 0x0c -v 0
one "set-fid=0x0e timestamp" set-feature "$DEV" -f 0x0e -v 0
one "set-fid=0x10 hctm"     set-feature "$DEV" -f 0x10 -v 0
one "set-fid=0x11 nopsc"    set-feature "$DEV" -f 0x11 -v 0
one "set-fid=0x12 rrl"      set-feature "$DEV" -f 0x12 -v 0
one "set-fid=0x16 hostbeh"  set-feature "$DEV" -f 0x16 -v 0
one "set-fid=0x04 tempthr"  set-feature "$DEV" -f 0x04 -v 0

echo ""
echo "-- I/O commands beyond read/write/flush"
one "compare"      compare "$DEV" -s 0 -c 7 -b 512 -z 4096 -d /dev/zero
one "verify"       verify "$DEV" -s 0 -c 7 -b 512
one "write-uncor"  write-uncor "$DEV" -s 0 -c 7 -b 512
one "copy"         copy "$DEV" -s 0 -d 100 -n 1 -b 512
one "flush"        flush "$DEV"
one "id-ns-csi"    id-ns "$DEV"

disconnect
echo "   disconnected"
