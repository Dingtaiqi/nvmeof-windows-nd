#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  f5_ns_verify.sh - look INSIDE the namespace, from the Linux side.
#
#  The Windows demo ends with "the volume's bytes are back in the target's
#  namespace".  That claim is only worth something if somebody else reads the
#  medium and finds them, so this reads every block of the namespace with nvme-cli
#  and looks for what Windows wrote:
#
#    * the NTFS signature at offset 3 of LBA 0   (the volume really is in there)
#    * a text string that only Windows could have produced
#
#  Usage: ./f5_ns_verify.sh <ip> <subnqn> [needle]
# ===========================================================================
set -e
IP="${1:-192.168.100.5}"
SUBNQN="${2:-nqn.2024-01.local.rdma:linux-nvmet}"
NEEDLE="${3:-NVMe-oF volume, written from Windows}"
NVME=/usr/bin/nvme
W=$(mktemp -d)
disconnect() { sudo -n $NVME disconnect -n "$SUBNQN" >/dev/null 2>&1 || true; }
trap 'rm -rf "$W"; disconnect' EXIT INT TERM

disconnect
sudo -n $NVME connect -t rdma -a "$IP" -s 4420 -n "$SUBNQN" >/dev/null 2>&1 || {
    echo "connect to $IP failed" >&2; exit 1; }
DEV=""; i=0
while [ "$i" -lt 30 ]; do
    DEV=$(sudo -n $NVME list 2>/dev/null | awk '$1 ~ /^\/dev\// && /Linux|NDVMEOF/ {print $1; exit}')
    [ -n "$DEV" ] && break
    i=$((i+1)); sleep 1
done
[ -n "$DEV" ] || { echo "no namespace device appeared" >&2; exit 1; }

NSZE=$(sudo -n $NVME id-ns "$DEV" 2>/dev/null | awk '/^nsze/{print $3}')
NSZE=$((NSZE))
echo "== $DEV  nsze=$NSZE blocks  ($((NSZE * 512)) bytes)"

# Read the whole namespace in chunks: one 64 MiB command over rxe is a lot of work
# for the peer in a single capsule, and a chunking bug is easier to see than a
# timeout.  The chunk size starts at 2 MiB and HALVES on the first refused read,
# because how big a read the transport accepts is the target's business
# (max_rdma_size / mdts) and this script is not the place to assert a number.
CHUNK=4096
: > "$W/ns.img"
off=0
adapted=no
while [ "$off" -lt "$NSZE" ]; do
    n=$((NSZE - off)); [ "$n" -gt "$CHUNK" ] && n=$CHUNK
    # Remove the staging file before EVERY attempt.  nvme-cli does not necessarily
    # truncate a `-d` file, and a refused attempt can leave a partial one behind: the
    # first version of this script kept it, the retry wrote 1 MiB over the front of a
    # 2 MiB leftover, and `cat` then appended 2 MiB for a 1 MiB chunk.  The dump came
    # back EXACTLY 64 MiB long and looked plausible, but every byte after that point
    # was shifted by 1049003 - which is how a "the data is not there" conclusion gets
    # built on a reading bug.  The tell was the MBR appearing at a non-512-aligned
    # offset: nothing in this protocol can write off a block boundary.
    rm -f "$W/part"
    if ! sudo -n $NVME read "$DEV" -s "$off" -c $((n - 1)) -b 512 -z $((n * 512)) \
             -d "$W/part" >/dev/null 2>"$W/err"; then
        if [ "$CHUNK" -gt 8 ]; then
            CHUNK=$((CHUNK / 2))
            echo "   (a $((n * 512))-byte read was refused: retrying at $((CHUNK * 512)) bytes)"
            adapted=yes
            continue
        fi
        cat "$W/err" >&2
        echo "a $((n * 512))-byte read at block $off failed" >&2
        exit 1
    fi
    got=$(wc -c < "$W/part")
    if [ "$got" != "$((n * 512))" ]; then
        echo "   read at block $off returned $got bytes, expected $((n * 512))" >&2
        exit 1
    fi
    cat "$W/part" >> "$W/ns.img"
    off=$((off + n))
done
[ "$adapted" = yes ] && echo "   (chunk size settled at $((CHUNK * 512)) bytes)"
echo "   read $(( $(wc -c < "$W/ns.img") )) bytes back"

echo ""
echo "-- LBA 0 (a boot sector, if Windows formatted it):"
sig=$(dd if="$W/ns.img" bs=1 skip=3 count=8 status=none)
[ "$sig" = "NTFS    " ] && echo "   [ok] signature 'NTFS    ' at offset 3" \
                        || echo "   [--] signature is '$sig', not NTFS"
od -A d -t x1 -N 32 "$W/ns.img" | head -2 | sed 's/^/   /'
echo "   MBR bootstrap '33 c0 8e d0' is at byte offset: $(grep -abo -m1 "$(printf '\x33\xc0\x8e\xd0')" "$W/ns.img" | cut -d: -f1)"

echo ""
echo "-- whole-namespace checksum (compare with the local image on Windows):"
echo "   sha256: $(sha256sum "$W/ns.img" | cut -d' ' -f1)"
echo "   bytes : $(wc -c < "$W/ns.img")"

echo ""
echo "-- searching all $(( $(wc -c < "$W/ns.img") )) bytes for the string Windows wrote:"
if grep -a -q -F "$NEEDLE" "$W/ns.img"; then
    echo "   [ok] found: $NEEDLE"
    grep -a -o -F -m1 "$NEEDLE" "$W/ns.img" | sed 's/^/   /'
    echo "   -- and the text around it:"
    grep -a -o -m1 "stamp : 2026.*" "$W/ns.img" | sed 's/^/      /' || true
else
    echo "   [FAIL] not found"
    exit 1
fi
disconnect
