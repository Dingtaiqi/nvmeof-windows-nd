#!/usr/bin/env python3
# Rebuild the PRIMARY GPT of /dev/nvme1n1 from the intact BACKUP GPT at the end of the disk.
#
# Why this is needed: the initiator's test suite issues "WRITE 8 blocks at slba 0", and this namespace
# exports the whole disk, so LBA 0 (protective MBR) plus LBA 1 (GPT header) plus LBA 2..7 (the first
# partition entries) were overwritten with a test pattern.  No partition data was touched - only the
# table - and the backup GPT at the last LBA is intact, which is exactly what it exists for.
#
# The reconstruction is the standard one: copy the backup header, point MyLBA at 1 and AlternateLBA at
# the last sector, point the entry array at LBA 2, recompute the header CRC32 and the entry-array CRC32,
# write it, and synthesise a protective MBR.
import os, struct, zlib, sys, shutil, subprocess

DEV = "/dev/nvme1n1"
SECT = 512
KEEP = "/root/nvme1n1-first34-sectors-before-gpt-restore.bin"

def read_at(f, off, n):
    f.seek(off); return f.read(n)

def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF

f = open(DEV, "r+b")
f.seek(0, os.SEEK_END)
size_bytes = f.tell()
total = size_bytes // SECT
print(f"device: {DEV}  {size_bytes} bytes  {total} sectors")

# ---- 0. preserve what is on the media right now -----------------------------------------------
with open(DEV, "rb") as r, open(KEEP, "wb") as k:
    k.write(r.read(34 * SECT))
print(f"kept the current first 34 sectors in {KEEP}")

# ---- 1. read the backup -----------------------------------------------------------------------
backup_hdr = read_at(f, (total - 1) * SECT, SECT)
if backup_hdr[:8] != b"EFI PART":
    sys.exit("backup header is not 'EFI PART' - refusing to guess")
hdr_size   = struct.unpack_from("<I", backup_hdr, 12)[0]
my_lba     = struct.unpack_from("<Q", backup_hdr, 24)[0]
alt_lba    = struct.unpack_from("<Q", backup_hdr, 32)[0]
first_use  = struct.unpack_from("<Q", backup_hdr, 40)[0]
last_use   = struct.unpack_from("<Q", backup_hdr, 48)[0]
disk_guid  = backup_hdr[56:72]
ent_lba    = struct.unpack_from("<Q", backup_hdr, 72)[0]
num_ent    = struct.unpack_from("<I", backup_hdr, 80)[0]
ent_size   = struct.unpack_from("<I", backup_hdr, 84)[0]
print(f"backup: MyLBA={my_lba} AltLBA={alt_lba} FirstUsable={first_use} LastUsable={last_use}")
print(f"        entryArray={ent_lba} numEntries={num_ent} entrySize={ent_size}")
if my_lba != total - 1:
    sys.exit("backup MyLBA does not point at the last sector - refusing to proceed")

ent_bytes = num_ent * ent_size
ent_sectors = (ent_bytes + SECT - 1) // SECT
entries = read_at(f, ent_lba * SECT, ent_sectors * SECT)
print(f"read {ent_sectors} entry sectors from the backup")

# ---- 2. build the primary header ---------------------------------------------------------------
hdr = bytearray(backup_hdr)
struct.pack_into("<Q", hdr, 24, 1)                 # MyLBA
struct.pack_into("<Q", hdr, 32, total - 1)         # AlternateLBA
struct.pack_into("<Q", hdr, 72, 2)                 # PartitionEntryLBA
struct.pack_into("<I", hdr, 16, 0)                 # HeaderCRC32 = 0 while computing
struct.pack_into("<I", hdr, 88, crc32(entries[:ent_bytes]))   # PartitionEntryArrayCRC32
struct.pack_into("<I", hdr, 16, crc32(bytes(hdr[:hdr_size])))
print(f"primary header CRC32 = 0x{struct.unpack_from('<I', hdr, 16)[0]:08X}")
print(f"entry array CRC32    = 0x{struct.unpack_from('<I', hdr, 88)[0]:08X}")

# ---- 3. protective MBR -------------------------------------------------------------------------
mbr = bytearray(SECT)
mbr[0x1BE] = 0x00                                  # not bootable
mbr[0x1BF:0x1C2] = bytes([0x00, 0x02, 0x00])       # CHS start (ignored)
mbr[0x1C2] = 0xEE                                  # type: GPT protective
mbr[0x1C3:0x1C6] = bytes([0xFF, 0xFF, 0xFF])       # CHS end (ignored)
struct.pack_into("<I", mbr, 0x1C6, 1)              # starting LBA
struct.pack_into("<I", mbr, 0x1CA, min(total - 1, 0xFFFFFFFF))
mbr[0x1FE:0x200] = b"\x55\xAA"

# ---- 4. write -----------------------------------------------------------------------------------
f.seek(0);            f.write(bytes(mbr))
f.seek(1 * SECT);     f.write(bytes(hdr[:SECT]))
f.seek(2 * SECT);     f.write(entries[:ent_sectors * SECT])
f.flush(); os.fsync(f.fileno())
print("wrote MBR at LBA 0, header at LBA 1, entries at LBA 2")

# ---- 5. verify by reading it back ---------------------------------------------------------------
f.seek(0); mbr2 = f.read(SECT)
f.seek(SECT); hdr2 = f.read(SECT)
print(f"read back: MBR sig={mbr2[0x1FE:0x200].hex()}  header={hdr2[:8]!r}  "
      f"hdrCRC=0x{struct.unpack_from('<I', hdr2, 16)[0]:08X}  MyLBA={struct.unpack_from('<Q', hdr2, 24)[0]}")
hdr2_ok = (hdr2[:8] == b"EFI PART"
           and struct.unpack_from("<I", hdr2, 16)[0] == crc32(bytes(hdr2[:hdr_size]))
           and struct.unpack_from("<Q", hdr2, 24)[0] == 1)
print("header self-check:", "PASS" if hdr2_ok else "FAIL")
f.close()
print("GPT-RESTORE-DONE")
