#!/bin/bash
# Read-only inspection of the backup GPT on /dev/nvme1n1 so the primary can be rebuilt from it.
set -u
D=/dev/nvme1n1
SZ=$(blockdev --getsz $D)
echo "== tools =="
for t in python3 sgdisk gdisk parted sfdisk; do
  p=$(command -v $t 2>/dev/null)
  if [ -n "$p" ]; then echo "  $t -> $p"; else echo "  $t -> (not installed)"; fi
done
echo "== device sectors: $SZ =="
echo "== backup GPT header (last sector) key fields =="
dd if=$D bs=512 skip=$((SZ-1)) count=1 2>/dev/null > /tmp/bk.bin
echo -n "  signature      : "; head -c 8 /tmp/bk.bin; echo
echo -n "  revision       : "; od -An -tx1 -j 8 -N 4 /tmp/bk.bin
echo -n "  header size    : "; od -An -tu4 -j 12 -N 4 /tmp/bk.bin
echo -n "  MyLBA (should be $((SZ-1))) : "; od -An -tu8 -j 24 -N 8 /tmp/bk.bin
echo -n "  AlternateLBA   : "; od -An -tu8 -j 32 -N 8 /tmp/bk.bin
echo -n "  FirstUsableLBA : "; od -An -tu8 -j 40 -N 8 /tmp/bk.bin
echo -n "  LastUsableLBA  : "; od -An -tu8 -j 48 -N 8 /tmp/bk.bin
echo -n "  DiskGUID       : "; od -An -tx1 -j 56 -N 16 /tmp/bk.bin
echo -n "  EntryArrayLBA  : "; od -An -tu8 -j 72 -N 8 /tmp/bk.bin
echo -n "  NumEntries     : "; od -An -tu4 -j 80 -N 4 /tmp/bk.bin
echo -n "  EntrySize      : "; od -An -tu4 -j 84 -N 4 /tmp/bk.bin
echo "== first partition entry of the backup array (type GUID) =="
EA=$(od -An -tu8 -j 72 -N 8 /tmp/bk.bin | tr -d ' ')
dd if=$D bs=512 skip=$EA count=1 2>/dev/null | od -An -tx1 -N 16
echo "== GPT-DUMP-DONE =="
