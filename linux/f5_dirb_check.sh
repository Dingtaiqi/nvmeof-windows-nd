#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  f5_dirb_check.sh - direction B: drive OUR Windows target from a real host.
#
#  This runs on the Linux peer.  It lives in a file rather than in a one-liner
#  sent over ssh because a command string that travels
#  PowerShell -> ssh.exe -> bash loses its double quotes on the way (Windows
#  argv quoting), and the first parenthesis in it then becomes
#  "bash: -c: line 12: syntax error near unexpected token `('".  A file has no
#  quoting left to lose.
#
#  Usage:
#      ./f5_dirb_check.sh [win-ip] [port] [subnqn]
#  Defaults: 192.168.100.2  4420  nqn.2024-01.local.rdma:windows-nd
#
#  Needs nvme-cli.  The nvme calls need root; they are the only ones that do, so
#  this script can be started as an ordinary user (and in fact must be, unless a
#  sudoers rule names this exact path).
#
#  Exit status: 0 only if the 65536 bytes written came back identical.
# ===========================================================================
set -e

WIN_IP="${1:-192.168.100.2}"
PORT="${2:-4420}"
SUBNQN="${3:-nqn.2024-01.local.rdma:windows-nd}"

say() { printf '%s\n' "$*"; }
die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

#  Work in a private directory of our own.  The data files must NOT live directly in
#  /tmp: `sudo nvme read -d /tmp/f5back` creates that file as root, and the next run
#  then dies at "rm: cannot remove '/tmp/f5back': Operation not permitted" - and with
#  `set -e` that abort happens in the middle of the sequence.  Inside a directory this
#  user owns, rm works on root-owned files (the directory's permission is what counts).
WORK=$(mktemp -d)
#  However this script ends, the host must not be left with a live controller: a
#  leftover controller keeps sending Keep Alives and the next run's connect then has
#  to sort out two of them.
trap 'rm -rf "$WORK"; sudo -n nvme disconnect -n "$SUBNQN" >/dev/null 2>&1 || true' EXIT INT TERM

#  A controller left over from an interrupted run does not just sit there: the kernel
#  keeps trying to reconnect it, and a `nvme connect` for the same NQN then fails with
#  "could not add new controller: failed to write to nvme-fabrics device" - while the
#  OLD controller's reconnect quietly does the handshake instead.  So disconnect and
#  then wait until the device is really gone, rather than assuming it is.
sudo -n nvme disconnect -n "$SUBNQN" >/dev/null 2>&1 || true
i=0
while [ "$i" -lt 20 ]; do
    sudo -n nvme list 2>/dev/null | grep -q NDVMEOF || break
    i=$((i + 1))
    sleep 1
done
if sudo -n nvme list 2>/dev/null | grep -q NDVMEOF; then
    die "a leftover NDVMEOF controller is still present after ${i}s - clear it first"
fi

say "== connect $WIN_IP:$PORT as $SUBNQN"

#  ---- discovery first: what does the target say it has? --------------------------
#  A real host asks this before it connects, and it is a SEPARATE controller with its
#  own admin queue pair: discovery connects, reads the log page, and goes away.  A
#  target that serves one connection and exits cannot answer both, which is why our
#  target is started with -serve 2 for this script.
say "== nvme discover (Get Log Page LID 0x70 on the discovery subsystem)"
if sudo -n nvme discover -t rdma -a "$WIN_IP" -s "$PORT" > "$WORK/discover.txt" 2>&1; then
    sed 's/^/   /' "$WORK/discover.txt"
    grep -q "subtype" "$WORK/discover.txt" || die "the discovery log has no entries"
    grep -q "$SUBNQN" "$WORK/discover.txt" || die "the discovery log does not name $SUBNQN"
    say "DISC: the discovery log names $SUBNQN"
else
    sed 's/^/   /' "$WORK/discover.txt"
    die "nvme discover failed against our target"
fi

i=0
#  Ask for eight I/O queues explicitly.  The kernel host creates as many queue pairs
#  as the target grants, so this is where a target that promises more than it owns
#  fails: it would accept the first and drop the rest.
#  --nr-io-queues is used when this nvme-cli knows it, and skipped when it does not,
#  so an older nvme-cli still runs this script instead of dying on an option.
CONNECT_OK=0
if sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN" --nr-io-queues=8; then
    CONNECT_OK=1
    say "   (asked the host for 8 I/O queues)"
else
    say "   --nr-io-queues was refused; retrying and letting the host choose"
    until sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN"; do
        i=$((i + 1))
        if [ "$i" -ge 3 ]; then
            die "nvme connect failed $i times - is the target running on $WIN_IP:$PORT?"
        fi
        say "connect attempt $i failed; waiting 2s"
        sleep 2
    done
fi
# The queues the kernel actually created: this is what our target must have accepted.
sleep 1

sudo -n nvme list

#  Find the device BY ITS SERIAL, never by index.  /dev/nvme0n1 on a normal
#  laptop is a real SSD with a real filesystem: a script that writes to "the
#  first nvme device" would destroy it.  Our target's serial starts with
#  NDVMEOF, so anything else is refused - and if we cannot find it we stop,
#  rather than guessing which device was probably meant.
#
#  Retry rather than assume: `nvme connect` returns as soon as the CONTROLLER
#  exists, and the namespace scan runs afterwards on a workqueue, so /dev/nvmeXn1
#  can be seconds behind.  A single look after one second is a race, and it lost
#  one (the controller had connected and authenticated; the namespace simply was
#  not there yet).
DEV=""
i=0
while [ "$i" -lt 30 ]; do
    DEV=$(sudo -n nvme list | awk '/NDVMEOF/ {print $1}' | head -1)
    [ -n "$DEV" ] && break
    i=$((i + 1)); sleep 1
done
if [ -z "$DEV" ]; then
    say "   --- nvme list:"
    sudo -n nvme list 2>&1 | sed 's/^/   /' || true
    say "   --- /dev/nvme*:"
    ls -l /dev/nvme* 2>&1 | sed 's/^/   /' || true
    sudo -n nvme disconnect -n "$SUBNQN" >/dev/null 2>&1 || true
    die "no NDVMEOF device appeared within 30 s - refusing to touch any other block device"
fi
say "using $DEV - serial NDVMEOF..., our target, not a real disk (after ${i}s)"

#  How many queue pairs the KERNEL created for this controller.  Our target's own log
#  is the authority on what it accepted; this is the host's side of the same number.
CTRL=$(basename "$DEV")
say "   host queue_count for $CTRL: $(cat "/sys/class/nvme/$CTRL/queue_count" 2>/dev/null || echo 'not exposed by this kernel')"

sudo -n nvme id-ctrl "$DEV" 2>&1 | head -20 || true

#  -s 0 -c 127 -b 512 -z 65536 = blocks 0..127 at 512 B = exactly 65536 bytes.
#  The units are easy to get wrong and the mistake looks like a data-path bug:
#  -c is 0-based and -b is the block size, so "write -c 15 -b 4096" on a 512-byte
#  device moves only 8192 bytes, and the following cmp then reports a difference
#  that came from the test rather than from the transfer.
dd if=/dev/urandom of="$WORK/pat" bs=4k count=16 status=none
sudo -n nvme write "$DEV" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/pat"
sudo -n nvme flush "$DEV"
sudo -n nvme read "$DEV" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/back"
cmp "$WORK/pat" "$WORK/back" && say "CMP: identical (65536 bytes written, flushed, read back)"

#  ---- multi-queue: four concurrent reads of the same 65536 bytes -----------------
#  A real host spreads I/O across its queues by CPU (blk-mq), so concurrent jobs land
#  on different queue pairs.  That is the host-side evidence that this controller is
#  really multi-queue: the target's log shows which queue served which command, and
#  every one of these reads must still return the same bytes.
i=0
while [ "$i" -lt 4 ]; do
    sudo -n nvme read "$DEV" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/par$i" &
    i=$((i + 1))
done
wait
i=0
while [ "$i" -lt 4 ]; do
    cmp "$WORK/pat" "$WORK/par$i" || die "concurrent read $i returned different bytes"
    i=$((i + 1))
done
say "CMP: identical for 4 concurrent reads (multi-queue)"

# Give the keep-alive path a moment's work before we leave: the target's own log
# must show it answering Keep Alives, which is where a KATO of 0 shows up.
sleep 2

sudo -n nvme disconnect -n "$SUBNQN"
say "== disconnected"
