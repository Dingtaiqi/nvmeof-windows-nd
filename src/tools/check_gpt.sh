#!/bin/bash
# SPDX-License-Identifier: MIT
# Check whether the first 4096 bytes of /dev/nvme1n1 still hold a valid GPT.
# The initiator test suite issues "WRITE 8 blocks at slba 0", which on a whole-disk namespace is the
# MBR plus the GPT header plus the first partition-entry sectors - so this must be verified on the media,
# not from the kernel's in-memory table.
set -u
D=/dev/nvme1n1
echo "== device =="
blockdev --getsz $D
echo "== LBA 0 (protective MBR): last two bytes must be 55 aa =="
dd if=$D bs=512 count=1 2>/dev/null | od -An -tx1 -j 510 -N 2
echo "== LBA 1 (GPT header): bytes 0-7 must be 'EFI PART' =="
dd if=$D bs=512 skip=1 count=1 2>/dev/null | od -An -c -N 8
echo "== LBA 1: header fields =="
dd if=$D bs=512 skip=1 count=1 2>/dev/null | od -An -tx1 -N 32
echo "== LBA 2 (first partition entries): entry 0 type GUID =="
dd if=$D bs=512 skip=2 count=1 2>/dev/null | od -An -tx1 -N 16
SZ=$(blockdev --getsz $D)
echo "== backup GPT at LBA $((SZ-1)): bytes 0-7 must be 'EFI PART' =="
dd if=$D bs=512 skip=$((SZ-1)) count=1 2>/dev/null | od -An -c -N 8
echo "== kernel view (re-read the media) =="
partprobe $D
sleep 1
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT $D
echo "== first 4 KB in hex (what the test wrote, if anything) =="
dd if=$D bs=512 count=8 2>/dev/null | od -An -tx1 | head -8
echo "== GPT-CHECK-DONE =="
