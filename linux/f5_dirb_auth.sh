#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  f5_dirb_auth.sh - direction B with DH-HMAC-CHAP: a real Linux host logs in
#  to OUR Windows target using in-band authentication.
#
#  f5_dirb_check.sh proves the transport and the command set.  This one proves
#  the thing next to it: that our target advertises ATR, that a Linux host's
#  Auth Send / Auth Receive capsules are understood, and that the HMACs the two
#  sides compute over the DH shared secret actually agree - on the wire, through
#  the RDMA transport this project wrote.
#
#  nvme-cli 3.x calls this KX-HMAC-CHAP and the options are -S / -C.  Older
#  nvme-cli spells the same two options --dhchap-secret / --dhchap-ctrl-secret;
#  both are accepted below, because the kernel side is unchanged and only the
#  option name moved.
#
#  Usage:
#      ./f5_dirb_auth.sh [win-ip] [port] [subnqn] [keyfile]
#  Defaults:
#      192.168.100.2  4420  nqn.2024-01.local.rdma:windows-nd  ~/f5_authkey.txt
#
#  keyfile: one key per line
#      1  the host key both ends share            (required)
#      2  the controller key, for bidirectional   (optional)
#      3  a DIFFERENT, well-formed host key       (optional; enables case 2)
#
#  Exit status: 0 only if every case behaved as the reference says.
# ===========================================================================
set -e

WIN_IP="${1:-192.168.100.2}"
PORT="${2:-4420}"
SUBNQN="${3:-nqn.2024-01.local.rdma:windows-nd}"
KEYFILE="${4:-$HOME/f5_authkey.txt}"

say()  { printf '%s\n' "$*"; }
ok()   { printf '  [ok] %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*" >&2; }
die()  { bad "$*"; exit 1; }

[ -r "$KEYFILE" ] || die "cannot read the key file $KEYFILE"
HOSTKEY=$(sed -n '1p' "$KEYFILE" | tr -d '\r\n')
CTRLKEY=$(sed -n '2p' "$KEYFILE" | tr -d '\r\n')
WRONGKEY=$(sed -n '3p' "$KEYFILE" | tr -d '\r\n')
[ -n "$HOSTKEY" ] || die "$KEYFILE line 1 is empty (the host key)"

case "$HOSTKEY" in
    DHHC-1:*) ;;
    *) die "line 1 of $KEYFILE is not a DHHC-1 key" ;;
esac

# -S / -C on nvme-cli 3.x, --dhchap-secret / --dhchap-ctrl-secret before the
# rename.  Pick by asking the binary, not by the version string: distributions
# backport the rename.
if sudo -n nvme connect --help 2>&1 | grep -q -- '--kxchap-secret'; then
    SECRET_OPT="-S"; CTRL_OPT="-C"
else
    SECRET_OPT="--dhchap-secret"; CTRL_OPT="--dhchap-ctrl-secret"
fi

WORK=$(mktemp -d)
disconnect() { sudo -n nvme disconnect -n "$SUBNQN" >/dev/null 2>&1 || true; }
trap 'rm -rf "$WORK"; disconnect' EXIT INT TERM

# A controller left over from an interrupted run keeps retrying in the kernel and
# makes the next connect fail with "could not add new controller" while the OLD
# one does the handshake behind our back - so wait for the device to be really
# gone rather than assuming.
wait_gone() {
    i=0
    while [ "$i" -lt 20 ]; do
        sudo -n nvme list 2>/dev/null | grep -q NDVMEOF || return 0
        i=$((i + 1)); sleep 1
    done
    return 1
}
disconnect
wait_gone || die "a leftover NDVMEOF controller is still present after 20s"

# Finds our device BY SERIAL and refuses to touch anything else: /dev/nvme0n1 on
# a laptop is a real SSD with a real filesystem.
find_dev() {
    sudo -n nvme list 2>/dev/null | awk '/NDVMEOF/ {print $1}' | head -1
}

# Wait for the namespace to appear, rather than sleeping and hoping.
#
# `nvme connect` returns as soon as the CONTROLLER exists; the namespace scan runs
# afterwards on a workqueue, so /dev/nvmeXn1 can be seconds behind the connect.
# The first version of this script slept one second and then required the device -
# it failed on a run where the controller had authenticated perfectly and the
# kernel's own udev had already probed the namespace.  "The device is not there
# yet" and "the device will never be there" are different things, and only the
# second one is a failure.
wait_dev() {
    i=0
    while [ "$i" -lt 30 ]; do
        d=$(find_dev)
        if [ -n "$d" ]; then printf '%s' "$d"; return 0; fi
        i=$((i + 1)); sleep 1
    done
    return 1
}

dump_nvme_state() {
    say "   --- nvme list:"
    sudo -n nvme list 2>&1 | sed 's/^/   /' || true
    say "   --- /dev/nvme*:"
    ls -l /dev/nvme* 2>&1 | sed 's/^/   /' || true
    say "   --- kernel log (last 20 lines):"
    sudo -n dmesg 2>/dev/null | tail -20 | sed 's/^/   /' || true
}

say "== DH-HMAC-CHAP against $WIN_IP:$PORT ($SUBNQN)"
say "   host key      : ${HOSTKEY%????}..."   # keep the secret out of the log
say "   controller key: $([ -n "$CTRLKEY" ] && echo yes || echo no)"
say "   wrong key     : $([ -n "$WRONGKEY" ] && echo yes || echo no)"
say "   option names  : $SECRET_OPT / $CTRL_OPT"

# ---------------------------------------------------------------------------
# case 1: the host key is right, the target must require it and accept it
# ---------------------------------------------------------------------------
say ""
say "== case 1: connect with the shared host key (the target requires auth)"
if sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN" \
        "$SECRET_OPT" "$HOSTKEY" --nr-io-queues=8; then
    ok "nvme connect succeeded with DH-HMAC-CHAP"
else
    die "nvme connect with the correct key FAILED - the exchange did not complete"
fi
sleep 1
DEV=$(wait_dev) || {
    dump_nvme_state
    die "no NDVMEOF device appeared within 30 s of an authenticated connect"
}
ok "device $DEV (serial NDVMEOF...) - our target, not a real disk"

# What the HOST says about the authentication it just did.  This is the peer's own
# account, which is the whole point of running it here rather than trusting our log.
if sudo -n dmesg 2>/dev/null | tail -60 | grep -qiE 'authenticated with hash|dhchap|kxchap'; then
    sudo -n dmesg 2>/dev/null | tail -60 | grep -iE 'authenticated with hash|dhchap|kxchap' | sed 's/^/   /'
    ok "the kernel host logged the authentication (see above)"
else
    say "   (the kernel did not log the auth line; not fatal, the connect succeeded)"
fi

# ---------------------------------------------------------------------------
# case 2: data actually moves over an authenticated controller
# ---------------------------------------------------------------------------
say ""
say "== case 2: write / flush / read back over the authenticated controller"
dd if=/dev/urandom of="$WORK/pat" bs=4k count=16 status=none
sudo -n nvme write "$DEV" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/pat"
sudo -n nvme flush "$DEV"
sudo -n nvme read  "$DEV" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/back"
if cmp "$WORK/pat" "$WORK/back"; then
    ok "CMP: identical (65536 bytes written, flushed and read back)"
else
    die "the bytes that came back differ from the bytes written"
fi

# ---------------------------------------------------------------------------
# case 3: a well-formed key that is not the target's must be REFUSED
#
# This costs a whole controller of its own: the Connect SUCCEEDS (a target cannot
# know the key is wrong until the Reply arrives), so our target serves a controller
# that then fails authentication fatally.  That is why the target's controller
# budget has to count this connection.
# ---------------------------------------------------------------------------
if [ -n "$WRONGKEY" ]; then
    say ""
    say "== case 3: connect with a DIFFERENT host key - must be refused"
    disconnect
    wait_gone || die "the controller did not go away before case 3"
    if sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN" \
            "$SECRET_OPT" "$WRONGKEY" 2>"$WORK/wrong.txt"; then
        disconnect
        die "nvme connect SUCCEEDED with the wrong key - the target is not verifying the response"
    fi
    sed 's/^/   /' "$WORK/wrong.txt" || true
    ok "the wrong key was refused by the target"
fi

# ---------------------------------------------------------------------------
# case 4: bidirectional - the TARGET has to prove it holds the controller key
# ---------------------------------------------------------------------------
if [ -n "$CTRLKEY" ]; then
    say ""
    say "== case 4: bidirectional (host key + controller key)"
    disconnect
    wait_gone || die "the controller did not go away before case 4"
    if sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN" \
            "$SECRET_OPT" "$HOSTKEY" "$CTRL_OPT" "$CTRLKEY"; then
        sleep 1
        DEV2=$(wait_dev) || { dump_nvme_state; die "no device 30 s after a bidirectional connect"; }
        ok "bidirectional connect succeeded - the host verified the target's response"
        sudo -n nvme read "$DEV2" -s 0 -c 127 -b 512 -z 65536 -d "$WORK/bi" >/dev/null
        cmp "$WORK/pat" "$WORK/bi" && ok "CMP: identical over the bidirectional controller"
    else
        die "bidirectional nvme connect FAILED"
    fi
fi

disconnect
ok "disconnected"
say ""
say "RESULT: PASS - a Linux host authenticated to our target and moved data"
