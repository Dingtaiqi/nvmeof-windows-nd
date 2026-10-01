#!/bin/sh
# ===========================================================================
#  f5_dirb_demo.sh - direction B, the one that produces something you can look at.
#
#  Two parts:
#
#   1. a WIDE admin sweep - the commands a real Linux host or admin may issue that
#      f5_dirb_admin.sh does not cover (format, write-zeroes, dsm, reservations,
#      telemetry, ns-rescan, more Get Features).  The verdict is our target's own
#      `unknownAdmin` / `unknownIo` counters, not the host's exit codes: nvme-cli
#      refuses plenty of things client-side before a capsule is ever sent.
#
#   2. the DEMO: Linux writes a marker to the namespace through NVMe-oF/RDMA, flushes,
#      disconnects, reconnects, and reads it back.  If the target was started with
#      `-nsfile F:\...img`, those bytes are now inside a FILE on the Windows side -
#      which is the whole point, and it is checked from Windows afterwards.
#
#  Usage:
#      ./f5_dirb_demo.sh [win-ip] [port] [subnqn]
#  Defaults: 192.168.100.2  4420  nqn.2024-01.local.rdma:windows-nd
#
#  Nothing here touches a block device it did not identify BY SERIAL, and the marker
#  is written at LBA 8 - well past any partition table a demo image might carry.
# ===========================================================================
set -e

WIN_IP="${1:-192.168.100.2}"
PORT="${2:-4420}"
SUBNQN="${3:-nqn.2024-01.local.rdma:windows-nd}"

say()  { printf '%s\n' "$*"; }
ok()   { printf '  [ok] %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*" >&2; }
die()  { bad "$*"; exit 1; }

WORK=$(mktemp -d)
disconnect() { sudo -n nvme disconnect -n "$SUBNQN" >/dev/null 2>&1 || true; }
trap 'rm -rf "$WORK"; disconnect' EXIT INT TERM

wait_gone() { i=0; while [ "$i" -lt 20 ]; do sudo -n nvme list 2>/dev/null | grep -q NDVMEOF || return 0; i=$((i+1)); sleep 1; done; return 1; }
find_dev()  { sudo -n nvme list 2>/dev/null | awk '/NDVMEOF/ {print $1}' | head -1; }
wait_dev()  { i=0; while [ "$i" -lt 30 ]; do d=$(find_dev); [ -n "$d" ] && { printf '%s' "$d"; return 0; }; i=$((i+1)); sleep 1; done; return 1; }

disconnect
wait_gone || die "a leftover NDVMEOF controller is still present"

say "== connect $WIN_IP:$PORT"
sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN" >/dev/null 2>&1 || die "nvme connect failed"
DEV=$(wait_dev) || die "no NDVMEOF device appeared"
CTRLNAME=$(printf '%s' "$DEV" | sed 's|/dev/||; s|n[0-9]*$||')
CTRLDEV="/dev/$CTRLNAME"
ok "connected: $DEV (controller $CTRLDEV)"
say "   $(sudo -n nvme id-ns "$DEV" 2>/dev/null | grep -E 'nsze|ncap' | tr '\n' ' ')"

# ---------------------------------------------------------------------------
#  1. the wide sweep
# ---------------------------------------------------------------------------
say ""
say "== wide admin sweep (the target's counter is the verdict; a host-side rc=1 may"
say "   just mean nvme-cli refused it locally, so both are printed)"
nrun=0; nbad=0
while IFS= read -r entry; do
    [ -n "$entry" ] || continue
    label=${entry%%|*}; args=${entry#*|}
    nrun=$((nrun+1))
    if out=$(sudo -n nvme $args "$DEV" 2>&1); then rc=0; else rc=$?; fi
    first=$(printf '%s\n' "$out" | grep -v "^WARNING" | head -1 | cut -c1-66)
    if [ "$rc" -eq 0 ]; then ok "$(printf '%-24s rc=0' "$label")  $first"
    else nbad=$((nbad+1)); say "  [refused] $(printf '%-24s rc=%s' "$label" "$rc")  $first"; fi
done <<EOF
get-feature-arbitration|get-feature -f 0x01
get-feature-power-mgmt|get-feature -f 0x02
get-feature-lba-range|get-feature -f 0x03
get-feature-write-atomic|get-feature -f 0x0a
get-feature-auto-pst|get-feature -f 0x0c
get-feature-timestamp|get-feature -f 0x0e
get-feature-host-behavior|get-feature -f 0x16
get-feature-sanitize|get-feature -f 0x17
telemetry-log|telemetry-log
persistent-event-log|persistent-event-log
error-log-lid1|get-log -i 0x01 -l 512
smart-log-lid2|get-log -i 0x02 -l 512
ana-log-lidc|get-log -i 0x0c -l 512
feature-effects-lid12|get-log -i 0x12 -l 512
ns-rescan|ns-rescan
write-zeroes|write-zeroes -s 2000 -c 7 -b 512
dsm-deallocate|dsm -s 2000 -b 8 -d
resv-report|resv-report
resv-register|resv-register -c 1 -k 0x12345678
effects-log-csi1|effects-log -c 1
EOF
say "  sweep: $nrun command(s), $nbad refused host-side"

# ---------------------------------------------------------------------------
#  2. the demo: bytes that leave Linux and end up in a file on Windows
# ---------------------------------------------------------------------------
say ""
say "== demo: write a marker through NVMe-oF, flush, disconnect, reconnect, read back"
MARK="$WORK/marker.bin"
# 8 blocks (4 KiB) at LBA 8.  The string has to be one a human can find in the file on
# the Windows side, so it says where it came from and when.
{
    printf 'NVMEOF-DEMO from Linux via our own NVMe-oF/RDMA stack\n'
    printf 'host    : %s\n' "$(uname -n)"
    printf 'kernel  : %s\n' "$(uname -r)"
    printf 'written : %s\n' "$(date -Is)"
    printf 'target  : %s:%s %s\n' "$WIN_IP" "$PORT" "$SUBNQN"
    printf 'lba     : 8..15 (4 KiB at offset 4096)\n'
} > "$MARK"
# pad to exactly 4096
dd if=/dev/zero bs=4096 count=1 status=none >> "$MARK" 2>/dev/null || true
head -c 4096 "$MARK" > "$WORK/marker.pad" && mv "$WORK/marker.pad" "$MARK"
say "   marker:"
sed 's/^/     /' "$MARK" | head -6

sudo -n nvme write "$DEV" -s 8 -c 7 -b 512 -z 4096 -d "$MARK"
ok "nvme write at LBA 8 (4 KiB)"
sudo -n nvme flush "$DEV"
ok "nvme flush (this is where our target persists the file backend)"
sleep 1

sudo -n nvme read "$DEV" -s 8 -c 7 -b 512 -z 4096 -d "$WORK/back1"
cmp "$MARK" "$WORK/back1" && ok "CMP: identical immediately after the write"

say "== disconnect and reconnect (a different controller, a different queue pair set)"
disconnect
wait_gone || die "the controller did not go away"
sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN" >/dev/null 2>&1 || die "the second connect failed"
DEV2=$(wait_dev) || die "no device after the reconnect"
ok "reconnected as $DEV2 (a new controller)"
sudo -n nvme read "$DEV2" -s 8 -c 7 -b 512 -z 4096 -d "$WORK/back2"
if cmp "$MARK" "$WORK/back2"; then
    ok "CMP: identical after a DISCONNECT + RECONNECT (the bytes outlived the controller)"
else
    die "the marker did not survive the reconnect"
fi

disconnect
ok "disconnected"

# ---------------------------------------------------------------------------
#  3. write-zeroes and dsm, made to PROVE something
# ---------------------------------------------------------------------------
# The sweep above only says these two are accepted (nvmet serves both; this target
# used to answer Invalid Command Opcode to both).  rc=0 on a namespace that is already
# zero proves nothing, so: write a pattern, zero it, read it back, and require zeros.
say ""
say "== zeroing commands: a pattern first, so 'reads back as zeros' cannot be luck"
# Connect FIRST: wait_dev() waits for a device, it does not make one appear, and the
# first version of this section waited 30 s for a controller it had not connected to.
sudo -n nvme connect -t rdma -a "$WIN_IP" -s "$PORT" -n "$SUBNQN" >/dev/null 2>&1 \
    || die "the connect for the zeroing test failed"
ZDEV=$(wait_dev) || die "no device for the zeroing test"

dd if=/dev/urandom of="$WORK/pat" bs=4096 count=1 status=none
sudo -n nvme write "$ZDEV" -s 3000 -c 7 -b 512 -z 4096 -d "$WORK/pat"
sudo -n nvme read  "$ZDEV" -s 3000 -c 7 -b 512 -z 4096 -d "$WORK/pat2"
cmp "$WORK/pat" "$WORK/pat2" || die "the pattern did not land in the first place"
ok "a 4 KiB random pattern is at LBA 3000"

sudo -n nvme write-zeroes "$ZDEV" -s 3000 -c 7 -b 512
sudo -n nvme read       "$ZDEV" -s 3000 -c 7 -b 512 -z 4096 -d "$WORK/z1"
if [ "$(tr -d '\0' < "$WORK/z1" | wc -c)" -eq 0 ]; then
    ok "WRITE ZEROES: all 4096 bytes read back as zero"
else
    die "write-zeroes left data behind"
fi

sudo -n nvme write "$ZDEV" -s 3100 -c 7 -b 512 -z 4096 -d "$WORK/pat"
# `-d` is the Attribute Deallocate BIT.  Without it nvme-cli sends a range whose
# context attributes are 0, and a range with no attribute is a no-op - the first
# version of this check used `-c 7 -b 512`, which sets CDW11 and the block count, and
# then reported a failure that was really the host not asking for anything.
sudo -n nvme dsm   "$ZDEV" -s 3100 -b 8 -d
sudo -n nvme read  "$ZDEV" -s 3100 -c 7 -b 512 -z 4096 -d "$WORK/z2"
if [ "$(tr -d '\0' < "$WORK/z2" | wc -c)" -eq 0 ]; then
    ok "DATASET MANAGEMENT (deallocate): all 4096 bytes read back as zero"
else
    die "dsm deallocate left data behind"
fi
sudo -n nvme flush "$ZDEV"
ok "flushed (so the zeroing is in the backing file too, not just in memory)"
disconnect
ok "disconnected"

say ""
say "RESULT: PASS - wide sweep done; a marker written by Linux survived a reconnect;"
say "        write-zeroes and dsm really zero the namespace."
say "        On Windows: if the target was started with -nsfile, the marker is inside"
say "        that file at offset 4096 and the zeroed areas are at 3000*512 and 3100*512."
