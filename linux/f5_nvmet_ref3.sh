#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  f5_nvmet_ref3.sh - part three: Set Features, the data-buffer Get Features
#  hypothesis, and the I/O commands.  Every command is wrapped in `timeout`:
#  part two died on `set-feature -f 0x0c`, which hung NVME-CLI (the peer's kernel
#  stack was fine and the teardown trap cleaned up), and one hanging row must not
#  cost the rest of the table again.
#
#  Hypothesis under test: nvmet answers "Data SGL Length Invalid" to ANY Get
#  Features command that carries a data buffer, not just to the data-structure
#  fids - it validates the transfer length against 0.  fid 0x06 is implemented and
#  returns its answer in the completion, so if 0x06 with -l 512 also comes back
#  SGL-invalid, the rule is "no data buffer on Get Features".
# ===========================================================================
set -e
IP="${1:-192.168.100.5}"; PORT="${2:-4420}"; SUBNQN="${3:-nqn.2024-01.local.rdma:linux-nvmet}"
NVME=/usr/bin/nvme
disconnect() { timeout 20 sudo -n $NVME disconnect -n "$SUBNQN" >/dev/null 2>&1 || true; }
trap 'disconnect' EXIT INT TERM
disconnect
sudo -n $NVME connect -t rdma -a "$IP" -s "$PORT" -n "$SUBNQN" >/dev/null 2>&1 || { echo "connect failed" >&2; exit 1; }
DEV=""; i=0
while [ "$i" -lt 30 ]; do DEV=$(sudo -n $NVME list 2>/dev/null | awk '/Linux/{print $1}' | head -1); [ -n "$DEV" ] && break; i=$((i+1)); sleep 1; done
[ -n "$DEV" ] || { echo "no device" >&2; exit 1; }
echo "== reference 3 on $DEV  (every row has a 15 s timeout)"

one() { label=$1; shift
        # `timeout` goes OUTSIDE sudo on purpose: the sudoers entry names
        # /usr/bin/nvme exactly, so `sudo -n timeout 15 nvme ...` is a different
        # command and gets "a password is required".  This cost one whole run.
        if out=$(timeout 15 sudo -n $NVME "$@" 2>&1); then rc=0; else rc=$?; fi
        st=$(printf '%s\n' "$out" | grep -o 'NVMe status:.*' | head -1)
        [ -n "$st" ] || st=$(printf '%s\n' "$out" | grep -v '^WARNING' | head -1)
        [ "$rc" = "124" ] && st="TIMED OUT (the host gave up after 15 s)"
        printf '  %-24s rc=%-3s %s\n' "$label" "$rc" "$(printf '%s' "$st" | cut -c1-84)"; }

echo ""
echo "-- hypothesis: does an implemented fid also reject a data buffer?"
one "get fid=0x06 (no data)"   get-feature "$DEV" -f 0x06
one "get fid=0x06 len=512"     get-feature "$DEV" -f 0x06 -l 512
one "get fid=0x07 (no data)"   get-feature "$DEV" -f 0x07
one "get fid=0x07 len=512"     get-feature "$DEV" -f 0x07 -l 512
one "get fid=0x01 len=512"     get-feature "$DEV" -f 0x01 -l 512

echo ""
echo "-- Set Features, one at a time"
one "set 0x06 vwc=1"      set-feature "$DEV" -f 0x06 -v 1
one "set 0x08 irqcoal"    set-feature "$DEV" -f 0x08 -v 0
one "set 0x09 irqcfg"     set-feature "$DEV" -f 0x09 -v 0
one "set 0x0b aen=0"      set-feature "$DEV" -f 0x0b -v 0
one "set 0x0e timestamp"  set-feature "$DEV" -f 0x0e -v 0
one "set 0x10 hctm"       set-feature "$DEV" -f 0x10 -v 0
one "set 0x11 nopsc"      set-feature "$DEV" -f 0x11 -v 0
one "set 0x12 rrl"        set-feature "$DEV" -f 0x12 -v 0
one "set 0x16 hostbeh"    set-feature "$DEV" -f 0x16 -v 0
one "set 0x04 tempthr"    set-feature "$DEV" -f 0x04 -v 0
one "set 0x05 errrec"     set-feature "$DEV" -f 0x05 -v 0
echo "-- (0x0c auto-pst is skipped: it hung nvme-cli in part two)"

echo ""
echo "-- I/O commands beyond read/write/flush"
one "compare"      compare "$DEV" -s 0 -c 7 -b 512 -z 4096 -d /dev/zero
one "verify"       verify "$DEV" -s 0 -c 7 -b 512
one "write-uncor"  write-uncor "$DEV" -s 2000 -c 7 -b 512
one "copy"         copy "$DEV" -s 0 -d 100 -n 1 -b 512
one "flush"        flush "$DEV"
one "write"        write "$DEV" -s 8 -c 7 -b 512 -z 4096 -d /dev/zero
one "read"         read  "$DEV" -s 8 -c 7 -b 512 -z 4096 -d /dev/null

echo ""
if timeout 15 sudo -n $NVME id-ctrl "$DEV" >/dev/null 2>&1; then echo "   still alive: yes"; else echo "   still alive: NO"; fi
disconnect
echo "   disconnected"
