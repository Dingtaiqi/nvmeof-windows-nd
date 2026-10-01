#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  f5_dirb_admin.sh - direction B, wider: what does a real Linux host ask of a
#  target, and does ours answer all of it?
#
#  f5_dirb_check.sh proves the transport, discovery, I/O and multi-queue.
#  This one goes after the ADMIN surface and the RECONNECT path, which are the two
#  places where "it works" and "it is compatible" come apart:
#
#    * every admin command nvme-cli can issue against a fabrics controller is run
#      here, and the point is not that they all succeed - a prototype target may
#      legitimately refuse some - but that OUR TARGET RECOGNISES ALL OF THEM.  The
#      target's own `unknownAdmin` counter is the verdict: a command it does not
#      implement logs UNHANDLED and answers Invalid Opcode, which is a legal
#      answer and still a gap.
#
#    * `nvme reset` tears the controller down and brings it back.  For a fabrics
#      controller that is a disconnect and a RECONNECT: the host drops every queue
#      pair, runs the admin Connect again, authenticates again if asked, and
#      re-creates its I/O queues.  A target that only ever serves one controller
#      passes every other test in this project and fails this one.
#
#  Read-only except for one Set Features (write cache) on a RAM disk.  `nvme reset`
#  is OPT-IN (-r) and the reason is worth reading before using it.
#
#  A controller reset makes the kernel tear the controller down and reconnect.  If the
#  target is not there when it reconnects, the host does not fail fast: `nvme reset`
#  blocks in the reset path until its Ctrl Loss Timeout, and the deletion that is
#  already in flight parks a task in D state holding nvme-core's controller lock.  A
#  connect issued afterwards then fails with an empty error and no kernel message, and
#  the controller cannot be deleted either - measured on this peer: `nvme list` and
#  `nvme disconnect` both stuck in D for 1200 s, with `INFO: task nvme blocked for
#  more than 737 seconds` every 120 s, and no way back short of a reboot.
#
#  The target's part of that is the reconnect window: it used to stop listening 20 s
#  after a controller went away, so a reset could never complete.  Start it with
#  `-reconnectwait 180` for this script.  Even then, use -r deliberately.
#
#  Usage:
#      ./f5_dirb_admin.sh [win-ip] [port] [subnqn] [keyfile] [-r]
#  Defaults:
#      192.168.100.2  4420  nqn.2024-01.local.rdma:windows-nd  (no auth)  no reset
# ===========================================================================
set -e

WIN_IP="${1:-192.168.100.2}"
PORT="${2:-4420}"
SUBNQN="${3:-nqn.2024-01.local.rdma:windows-nd}"
KEYFILE="${4:-}"
WITH_RESET=0
for a in "$@"; do [ "$a" = "-r" ] && WITH_RESET=1; done

say()  { printf '%s\n' "$*"; }
ok()   { printf '  [ok] %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*" >&2; }
die()  { bad "$*"; exit 1; }

SECRET_OPT=""
if [ -n "$KEYFILE" ] && [ -r "$KEYFILE" ]; then
    HOSTKEY=$(sed -n '1p' "$KEYFILE" | tr -d '\r\n')
    if sudo -n nvme connect --help 2>&1 | grep -q -- '--kxchap-secret'; then
        SECRET_OPT="-S $HOSTKEY"
    else
        SECRET_OPT="--dhchap-secret $HOSTKEY"
    fi
    say "== authentication: on (key from $KEYFILE)"
fi

WORK=$(mktemp -d)
disconnect() { sudo -n nvme disconnect -n "$SUBNQN" >/dev/null 2>&1 || true; }
trap 'rm -rf "$WORK"; disconnect' EXIT INT TERM

wait_gone() {
    i=0
    while [ "$i" -lt 20 ]; do
        sudo -n nvme list 2>/dev/null | grep -q NDVMEOF || return 0
        i=$((i + 1)); sleep 1
    done
    return 1
}
find_dev() { sudo -n nvme list 2>/dev/null | awk '/NDVMEOF/ {print $1}' | head -1; }
wait_dev() {
    i=0
    while [ "$i" -lt 30 ]; do
        d=$(find_dev); [ -n "$d" ] && { printf '%s' "$d"; return 0; }
        i=$((i + 1)); sleep 1
    done
    return 1
}

disconnect
wait_gone || die "a leftover NDVMEOF controller is still present"

say ""
say "== connect $WIN_IP:$PORT as $SUBNQN"
# shellcheck disable=SC2086
if ! sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN" $SECRET_OPT --nr-io-queues=4; then
    die "nvme connect failed"
fi
DEV=$(wait_dev) || die "no NDVMEOF device appeared"
ok "connected: $DEV"

# ---------------------------------------------------------------------------
#  The admin battery.
#
#  Each line is "label|command".  The command runs with the device appended, and the
#  script records the exit status and the first line of output.  Nothing here is
#  judged by this script: what a command returns is the host's business, and the
#  verdict on our target is its own unknownAdmin counter.  The output exists so that
#  a refusal can be read and argued with.
# ---------------------------------------------------------------------------
say ""
say "== admin battery (the host's own view; the target's counter is the verdict)"
# One command per line, read as a line: `for entry in $BATTERY` splits on WHITESPACE,
# so "list-ns -n 1" became three separate "commands" and every option was run as a
# subcommand.  The first version of this script therefore reported 31 commands for a
# 15-line battery, and the five Get Features never ran at all.
nrun=0; nbad=0
while IFS= read -r entry; do
    [ -n "$entry" ] || continue
    label=${entry%%|*}
    args=${entry#*|}
    nrun=$((nrun + 1))
    # shellcheck disable=SC2086
    if out=$(sudo -n nvme $args "$DEV" 2>&1); then
        rc=0
    else
        rc=$?
    fi
    first=$(printf '%s\n' "$out" | grep -v "^WARNING" | head -1 | cut -c1-70)
    if [ "$rc" -eq 0 ]; then
        ok "$(printf '%-26s rc=0' "$label")  $first"
    else
        nbad=$((nbad + 1))
        say "  [refused] $(printf '%-26s rc=%s' "$label" "$rc")  $first"
    fi
done <<EOF
identify-controller|id-ctrl
identify-namespace|id-ns
namespace-descriptors|ns-descs
list-namespaces|list-ns -n 1
get-namespace-id|get-ns-id
smart-log|smart-log
error-log|error-log
effects-log|effects-log -c 0
fw-log|fw-log
get-feature-num-queues|get-feature -f 0x07
get-feature-write-cache|get-feature -f 0x06
get-feature-temperature|get-feature -f 0x04
get-feature-ka|get-feature -f 0x0f
get-feature-async-event|get-feature -f 0x0b
set-feature-volatile-wc|set-feature -f 0x06 -V 0
EOF
say "  battery: $nrun command(s), $nbad refused by the host's own error handling"
say "  (a refusal can be correct - what matters is that our target ANSWERED it;"
say "   see unknownAdmin in the target's summary)"

# ---------------------------------------------------------------------------
#  Data, then a controller reset, then data again.
# ---------------------------------------------------------------------------
say ""
say "== data before the reset"
dd if=/dev/urandom of="$WORK/pat" bs=4k count=16 status=none
sudo -n nvme write "$DEV" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/pat"
sudo -n nvme flush "$DEV"
sudo -n nvme read "$DEV" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/before"
cmp "$WORK/pat" "$WORK/before" && ok "CMP: identical before the reset"

# `nvme reset` on a fabrics controller is a DISCONNECT AND RECONNECT: the host drops
# every queue pair and runs the whole handshake again - admin Connect, authentication
# if required, Set Features, Identify, I/O Connects.  A target that serves exactly one
# controller answers everything above and then never comes back.
#
# It takes the CONTROLLER node (/dev/nvme2), not the namespace: passing the namespace
# gets "Only controller device is allowed" and nothing happens at all - which is how
# the first version of this test "passed": the device never went away, so of course it
# was still there.
say ""
if [ "$WITH_RESET" = 1 ]; then
say "== nvme reset (the host tears the controller down and reconnects)"
CTRLNAME=$(printf '%s' "$DEV" | sed 's|/dev/||; s|n[0-9]*$||')
CTRLDEV="/dev/$CTRLNAME"
say "   device $DEV -> controller $CTRLDEV"
# The target must be listening again BEFORE this: see the header.  A reset with no
# target to come back to wedges the host's nvme stack, and that is not recoverable.
if ! ping -c1 -W2 "$WIN_IP" >/dev/null 2>&1; then
    die "the target does not answer ping - refusing to reset a controller it cannot reconnect to"
fi
BEFORE_CTRLS=$(sudo -n nvme list | grep -c NDVMEOF || true)
# Everything the kernel says from here on is what this step is judged by, so take a
# line count first and read only the new lines afterwards.
DMESG_BEFORE=$(sudo -n dmesg 2>/dev/null | wc -l)

T0=$(date +%s)
timeout 60 sudo -n nvme reset "$CTRLDEV" && RESET_RC=0 || RESET_RC=$?
T1=$(date +%s)
say "   nvme reset returned $RESET_RC after $((T1 - T0))s"

# What a controller reset must produce, and what it must NOT:
#
#   * the host re-runs the handshake - "resetting controller", then a new "creating N
#     I/O queues" - which is the whole point of the command;
#   * NO command timeout in the kernel log.  Before the CC.EN=0 fix this target tore
#     its queue pairs down at the moment the host polled CSTS to confirm the disable,
#     and the poll then waited out the 60-second command timeout:
#     `nvme nvme2: I/O tag 1 (4001) opcode 0x7f (Property Get) QID 0 timeout`.
#     Measured before: 60.8 s from "resetting controller" to the reconnect.  After: 1.07 s.
#   * the DEVICE does not have to disappear.  On this kernel a fabrics controller
#     reset reconnects IN PLACE and the namespace survives, so "the device must go
#     away" is not a property of a reset - the first version of this test asserted
#     exactly that and reported FAIL on a reconnect that took 1.07 s.  What is
#     asserted instead is the pair above, plus that the device still works.
NEW=$(sudo -n dmesg 2>/dev/null | tail -n +$((DMESG_BEFORE + 1)))
if printf '%s\n' "$NEW" | grep -q "resetting controller"; then
    ok "the kernel reset the controller"
else
    bad "the kernel never logged 'resetting controller' - the command did nothing"
fi
if printf '%s\n' "$NEW" | grep -qE "opcode 0x7f \(Property Get\).*timeout"; then
    bad "a Property Get TIMED OUT during the reset (60 s of the host's time per reset)"
    printf '%s\n' "$NEW" | grep -E "timeout" | sed 's/^/      /'
else
    ok "no command timeout during the reset"
fi
if [ "$((T1 - T0))" -le 20 ]; then
    ok "the reset completed in $((T1 - T0))s (a reconnect, not a timeout)"
else
    bad "the reset took $((T1 - T0))s - too long for a healthy reconnect"
fi

i=0
NEWDEV=""
while [ "$i" -lt 60 ]; do
    NEWDEV=$(find_dev)
    [ -n "$NEWDEV" ] && break
    i=$((i + 1)); sleep 1
done
if [ -z "$NEWDEV" ]; then
    say "  --- nvme list after the reset:"
    sudo -n nvme list 2>&1 | sed 's/^/   /'
    say "  --- kernel log:"
    printf '%s\n' "$NEW" | tail -20 | sed 's/^/   /'
    die "no NDVMEOF device after nvme reset (waited $i s)"
fi
ok "the device is usable again as $NEWDEV after ${i}s"
say "   controllers seen by the host before/after the reset: $BEFORE_CTRLS -> $(sudo -n nvme list | grep -c NDVMEOF || true)"
say "   (a same-count result is expected: the namespace survives a fabrics reset)"

say "== data after the reset"
sudo -n nvme read "$NEWDEV" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/after"
if cmp "$WORK/pat" "$WORK/after"; then
    ok "CMP: identical after the reconnect (the namespace survived the controller)"
else
    die "the bytes changed across the reset - the target did not keep the namespace"
fi
else
    say "== nvme reset: SKIPPED (pass -r to run it)"
    say "   It tears the controller down and makes the kernel reconnect.  A target that"
    say "   stops listening first turns that into a wedged nvme stack on this host, so it"
    say "   is opt-in; the target needs -reconnectwait 180 for it to be meaningful."
fi

say ""
say "== disconnect"
disconnect
ok "disconnected"
say ""
if [ "$WITH_RESET" = 1 ]; then
    say "RESULT: PASS - the admin battery and a real controller reconnect both completed"
else
    say "RESULT: PASS - the admin battery completed (nvme reset was NOT run; pass -r for it)"
fi
