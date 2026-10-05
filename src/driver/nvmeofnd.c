// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
//
// ===========================================================================
//  nvmeofnd.sys - D1.1: a StorPort VIRTUAL MINIPORT, step one of native
//  Windows NVMe-oF.
//
//  Why this shape: this Windows (11 25H2, build 26200) ships no nvmeof.sys and
//  no nvmf.sys, so there is no Microsoft NVMe-oF transport to register with -
//  the native initiator exists only on Windows Server 2025.  A StorPort virtual
//  miniport is therefore the native form available here: Windows' own storage
//  stack mounts what we present, partitions it, formats it and puts a drive
//  letter on it, and later the same driver speaks NVMe-oF over the kernel NDK
//  path that D0 proved.
//
//  D1.1 deliberately does NOT touch the network.  It presents a RAM-backed LUN
//  so the storage-stack side is finished and provable on its own.  StorPort's
//  failure modes (timeouts, resets, queue depth, SRB status codes) have nothing
//  in common with RDMA's, and debugging both at once is how days disappear.
//
//  Interfaces below were read out of storport.h / scsi.h (WDK 10.0.26100.0),
//  not recalled: HW_INITIALIZATION_DATA's field list, the SRB layout,
//  StorPortInitialize / StorPortNotification / StorPortGetSystemAddress, and
//  PORT_CONFIGURATION_INFORMATION.VirtualDevice.
// ===========================================================================

#include <ntddk.h>
#include <storport.h>

// SCSI opcodes and status codes are defined here rather than pulled from scsi.h, because including
// both storport.h and scsi.h makes srb.h define its structures twice (measured: a wall of C2011
// "struct type redefinition").  The values are the standard ones.
#define NVMEOFND_OPC_TEST_UNIT_READY  0x00
#define NVMEOFND_OPC_REQUEST_SENSE    0x03
#define NVMEOFND_OPC_INQUIRY          0x12
#define NVMEOFND_OPC_READ_CAPACITY10  0x25
#define NVMEOFND_OPC_READ10           0x28
#define NVMEOFND_OPC_WRITE10          0x2A
#define NVMEOFND_OPC_SYNC_CACHE10     0x35
#define NVMEOFND_OPC_READ16           0x88
#define NVMEOFND_OPC_WRITE16          0x8A
#define NVMEOFND_OPC_READ6            0x08
#define NVMEOFND_OPC_WRITE6           0x0A
#define NVMEOFND_OPC_REPORT_LUNS      0xA0
#define NVMEOFND_OPC_MODE_SENSE6     0x1A
#define NVMEOFND_OPC_START_STOP_UNIT 0x1B
#define NVMEOFND_OPC_VERIFY10        0x2F
#define NVMEOFND_OPC_MODE_SENSE10    0x5A
#define NVMEOFND_OPC_SYNC_CACHE16    0x91
#define NVMEOFND_OPC_SERVICE_IN16    0x9E
#define NVMEOFND_STAT_GOOD            0x00
#define NVMEOFND_STAT_CHECK_COND      0x02
#define NVMEOFND_SENSE_ILLEGAL_REQ    0x05

#define NVMEOFND_TAG          'dnfo'
#define NVMEOFND_LUN_BYTES    (48u * 1024u * 1024u)   // 48 MiB, the same size as the interop ns48.img
#define NVMEOFND_BLOCK_BYTES  512u
#define NVMEOFND_BLOCKS       (NVMEOFND_LUN_BYTES / NVMEOFND_BLOCK_BYTES)

#define NVMEOFND_MAX_TARGETS  1
#define NVMEOFND_MAX_LUNS     1

// SCSI opcodes come from scsi.h where it names them; the two 10-byte block
// opcodes are written as literals because this header spells them differently
// across WDK versions and a wrong name would simply fail to compile.
#define SCSIOP_READ10_          0x28
#define SCSIOP_WRITE10_         0x2A

typedef struct _NVMEOFND_EXTENSION {
    PVOID   Lun;                       // the RAM backing store
    ULONG   LunBytes;
    ULONG   Blocks;
    ULONG   BlockBytes;
    BOOLEAN Ready;
    ULONGLONG Reads;
    ULONGLONG Writes;
    ULONGLONG BytesIn;
    ULONGLONG BytesOut;
    // SCSI I/O runs at DISPATCH_LEVEL, where ZwSetValueKey (PASSIVE only) fails silently.
    // That is exactly why the first version of this driver looked like it received no
    // commands at all while Windows was in fact reading INQUIRY and READ CAPACITY from it.
    // Counters therefore live here and are published from the WMI SRB path, which is the
    // one place StorPort calls the miniport at PASSIVE level.
    ULONG   OpCount[256];
    ULONG   TotalSrbs;
    ULONGLONG IoBytes;
} NVMEOFND_EXTENSION, *PNVMEOFND_EXTENSION;

// ---------------------------------------------------------------------------
//  Progress tracing.  A StorPort miniport that loads and presents nothing gives
//  no clue which of FindAdapter / Initialize / BusChangeDetected ran, and there
//  is no printf in this context - so each stage records a DWORD under the same
//  key the NDK probes already use, and user mode reads them back.
// ---------------------------------------------------------------------------
static VOID NvmeofndTrace(PCWSTR name, ULONG value)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, NULL))) {
        return;
    }
    RtlInitUnicodeString(&valueName, name);
    ZwSetValueKey(key, &valueName, 0, REG_DWORD, &value, sizeof(value));
    ZwClose(key);
}

// ---------------------------------------------------------------------------
//  Inquiry data: what a disk driver must answer before anything else works.
// ---------------------------------------------------------------------------
static const UCHAR g_Inquiry[36] = {
    0x00,           // peripheral qualifier 0, device type 0 = direct access block
    0x00,           // not removable
    0x05,           // SPC-3
    0x02,           // response data format 2
    0x1F,           // additional length = 31 (36 - 5)
    0x00, 0x00,     // reserved
    0x00,           // no linked commands
    'N','V','M','e','o','F','N','D',        // vendor  (8)
    'R','A','M',' ','L','U','N',' ',        // product (16)
    '0','0','0','1',                        // revision (4)
};

static VOID NvmeofndComplete(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb, UCHAR SrbStatus)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    Srb->SrbStatus = SrbStatus;
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
}

static VOID NvmeofndSetSense(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb,
                             UCHAR Key, UCHAR Asc, UCHAR Ascq)
{
    if (Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength >= 18) {
        PUCHAR s = (PUCHAR)Srb->SenseInfoBuffer;
        RtlZeroMemory(s, 18);
        s[0] = 0x70;            // current error, valid
        s[2] = Key;             // sense key
        s[7] = 10;              // additional sense length
        s[12] = Asc;
        s[13] = Ascq;
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }
    Srb->ScsiStatus = NVMEOFND_STAT_CHECK_COND;
}

// ---------------------------------------------------------------------------
//  HwFindAdapter - for a virtual device this is where the shape is declared.
//  ConfigInfo->VirtualDevice = TRUE is what tells StorPort not to go looking
//  for hardware, and it is the difference between a miniport that loads and one
//  that is never called at all.
// ---------------------------------------------------------------------------
static ULONG NvmeofndFindAdapter(PVOID DeviceExtension, PVOID HwContext, PVOID BusInformation,
                                 PCHAR ArgumentString, PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                                 PBOOLEAN Again)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);
    UNREFERENCED_PARAMETER(Again);

    NvmeofndTrace(L"TraceFindAdapter", 1);

    // MINIMAL ConfigInfo, deliberately.  The previous version set a dozen fields from
    // memory of the sample, and the adapter start failed between HwFindAdapter and
    // HwInitialize with no indication which field was unacceptable.  StorPort fills in
    // what it wants for a virtual device; a miniport should state only what it must.
    ConfigInfo->VirtualDevice                  = TRUE;
    ConfigInfo->MaximumTransferLength          = 1024u * 1024u;
    ConfigInfo->MaximumNumberOfTargets         = NVMEOFND_MAX_TARGETS;
    ConfigInfo->MaximumNumberOfLogicalUnits    = NVMEOFND_MAX_LUNS;
    ConfigInfo->SrbType                        = SRB_TYPE_SCSI_REQUEST_BLOCK;
    ConfigInfo->AddressType                    = STORAGE_ADDRESS_TYPE_BTL8;
    // NumberOfBuses is what tells StorPort how many buses to scan for devices.  Leaving
    // it out (as the first minimal version did) gives ZERO buses, so the adapter runs and
    // is never asked for a single SCSI command - measured exactly that way.
    ConfigInfo->NumberOfBuses                = 1;

    NvmeofndTrace(L"TraceCfgVirtual",    ConfigInfo->VirtualDevice ? 1 : 0);
    NvmeofndTrace(L"TraceCfgMaxXfer",    ConfigInfo->MaximumTransferLength);
    NvmeofndTrace(L"TraceCfgMaxLuns",    ConfigInfo->MaximumNumberOfLogicalUnits);
    NvmeofndTrace(L"TraceCfgMaxTargets", ConfigInfo->MaximumNumberOfTargets);
    NvmeofndTrace(L"TraceFindAdapterDone", 1);
    return SP_RETURN_FOUND;
}

// ---------------------------------------------------------------------------
//  HwInitialize
// ---------------------------------------------------------------------------
static BOOLEAN NvmeofndInitialize(PVOID DeviceExtension)
{
    NvmeofndTrace(L"TraceInitialize", 1);
    ULONG status;
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;

    ext->LunBytes   = NVMEOFND_LUN_BYTES;
    ext->BlockBytes = NVMEOFND_BLOCK_BYTES;
    ext->Blocks     = NVMEOFND_BLOCKS;
    status = StorPortAllocatePool(DeviceExtension, ext->LunBytes, NVMEOFND_TAG, &ext->Lun);
    if (ext->Lun == NULL) {
        return FALSE;
    }
    // A recognisable pattern, so "the disk is really ours and really readable"
    // is checkable with a hex dump rather than taken on trust.
    {
        PUCHAR p = (PUCHAR)ext->Lun;
        for (ULONG i = 0; i < ext->LunBytes; i++) {
            p[i] = (UCHAR)(i & 0xFF);
        }
        RtlCopyMemory(p + 512, "NVMEOFND-D1.1-RAM-LUN", 21);
    }
    ext->Ready = TRUE;

    StorPortSetDeviceQueueDepth(DeviceExtension, 0, 0, 0, 32);
    // Announce the LUN: until this fires, the storage stack does not know the
    // device exists and nothing else in this file is ever called.
    // StorPort takes PathId, TargetId AND Lun here.  With only one argument the
    // other two are read off the stack, so the device at (0,0,0) is never announced and
    // StorPort never scans it - measured as a running adapter that receives no SCSI
    // command at all, only one WMI SRB.
    StorPortNotification(BusChangeDetected, DeviceExtension, 0, 0, 0);
    NvmeofndTrace(L"TraceBusChange", 1);
    return TRUE;
}

// ---------------------------------------------------------------------------
//  HwStartIo - one SCSI command at a time, answered from the RAM LUN.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Counters are published from here: the WMI path is the one entry point
//  StorPort uses at PASSIVE_LEVEL, so registry writes are legal in it and
//  illegal (and silently lost) in the I/O path below.
// ---------------------------------------------------------------------------
static VOID NvmeofndPublishStats(PVOID DeviceExtension)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    static const UCHAR interesting[] = {
        0x00, 0x03, 0x12, 0x1A, 0x1B, 0x25, 0x28, 0x2A, 0x2F, 0x35,
        0x5A, 0x88, 0x8A, 0x91, 0x9E, 0xA0
    };
    WCHAR name[32];
    ULONG i;

    NvmeofndTrace(L"StatTotal", ext->TotalSrbs);
    NvmeofndTrace(L"StatReads", (ULONG)ext->Reads);
    NvmeofndTrace(L"StatWrites", (ULONG)ext->Writes);
    for (i = 0; i < sizeof(interesting) / sizeof(interesting[0]); i++) {
        UCHAR op = interesting[i];
        if (ext->OpCount[op] == 0) {
            continue;
        }
        {   // build "StatOp_XX" by hand: no ntstrsafe dependency for nine characters
            static const WCHAR hexd[] = L"0123456789ABCDEF";
            name[0] = L'S'; name[1] = L't'; name[2] = L'a'; name[3] = L't';
            name[4] = L'O'; name[5] = L'p'; name[6] = L'_';
            name[7] = hexd[(op >> 4) & 0x0F];
            name[8] = hexd[op & 0x0F];
            name[9] = 0;
            NvmeofndTrace(name, ext->OpCount[op]);
        }
    }
}

// ---------------------------------------------------------------------------
//  HwStartIo - the SCSI command set Windows actually uses against a disk.
//  Every command the host may legally send has a case here; anything else is
//  refused WITH sense data rather than silently, because a host that gets a
//  bare failure retries forever.
// ---------------------------------------------------------------------------
static BOOLEAN NvmeofndStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    PUCHAR cdb;
    PVOID  buffer = NULL;
    UCHAR  op;

    ext->TotalSrbs++;

    if (Srb->Function == SRB_FUNCTION_WMI) {
        NvmeofndPublishStats(DeviceExtension);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }
    if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI) {
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_INVALID_REQUEST);
        return TRUE;
    }
    if (Srb->CdbLength < 6) {
        NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
        return TRUE;
    }
    cdb = Srb->Cdb;
    op = cdb[0];
    ext->OpCount[op]++;

    if (Srb->DataTransferLength > 0 &&
        StorPortGetSystemAddress(DeviceExtension, Srb, &buffer) != STOR_STATUS_SUCCESS) {
        buffer = NULL;
    }

    switch (op) {

    case NVMEOFND_OPC_TEST_UNIT_READY:
    case NVMEOFND_OPC_START_STOP_UNIT:          // a RAM LUN is always spinning
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;

    case NVMEOFND_OPC_VERIFY10:
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;   // nothing to verify: successful no-op
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;

    case NVMEOFND_OPC_INQUIRY: {
        BOOLEAN evpd = (cdb[1] & 0x01) != 0;
        UCHAR   page = cdb[2];
        ULONG   want = Srb->DataTransferLength;
        ULONG   give = 0;

        if (buffer == NULL) {
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return TRUE;
        }
        RtlZeroMemory(buffer, want);
        if (!evpd) {
            give = (want < sizeof(g_Inquiry)) ? want : sizeof(g_Inquiry);
            RtlCopyMemory(buffer, g_Inquiry, give);
            Srb->DataTransferLength = give;
        } else if (page == 0x00) {
            static const UCHAR pages[3] = { 0x00, 0x80, 0x83 };
            give = (want < 7) ? want : 7;
            ((PUCHAR)buffer)[1] = 0x00;
            ((PUCHAR)buffer)[3] = 3;
            RtlCopyMemory((PUCHAR)buffer + 4, pages, 3);
            Srb->DataTransferLength = give;
        } else if (page == 0x80) {
            static const char serial[] = "NVMEOFND0000000000001";
            give = (want < (ULONG)(5 + sizeof(serial) - 1)) ? want : (ULONG)(5 + sizeof(serial) - 1);
            ((PUCHAR)buffer)[1] = 0x80;
            ((PUCHAR)buffer)[3] = (UCHAR)(sizeof(serial) - 1);
            RtlCopyMemory((PUCHAR)buffer + 4, serial, sizeof(serial) - 1);
            Srb->DataTransferLength = give;
        } else if (page == 0x83) {
            // One identification descriptor: T10 vendor id "NVMEOFND" + serial.
            static const char vpd83[] = "NVMEOFND:0000000000000000001";
            ULONG dlen = (ULONG)(sizeof(vpd83) - 1);
            if (want >= 8 + dlen) {
                PUCHAR d = (PUCHAR)buffer + 4;
                d[0] = 0x02;                    // code set: ASCII
                d[1] = 0x01;                    // identifier type: T10 vendor ID
                d[3] = (UCHAR)dlen;
                RtlCopyMemory(d + 4, vpd83, dlen);
                ((PUCHAR)buffer)[1] = 0x83;
                ((PUCHAR)buffer)[3] = (UCHAR)(4 + dlen);
                Srb->DataTransferLength = 8 + dlen;
            } else {
                Srb->DataTransferLength = 4;
            }
        } else {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x24, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return TRUE;
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_READ_CAPACITY10: {
        PUCHAR out = (PUCHAR)buffer;
        ULONG  lastLba = ext->Blocks - 1;
        if (out == NULL || Srb->DataTransferLength < 8) {
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return TRUE;
        }
        out[0] = (UCHAR)(lastLba >> 24); out[1] = (UCHAR)(lastLba >> 16);
        out[2] = (UCHAR)(lastLba >> 8);  out[3] = (UCHAR)(lastLba);
        out[4] = (UCHAR)(ext->BlockBytes >> 24); out[5] = (UCHAR)(ext->BlockBytes >> 16);
        out[6] = (UCHAR)(ext->BlockBytes >> 8);  out[7] = (UCHAR)(ext->BlockBytes);
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_SERVICE_IN16: {
        // SERVICE ACTION IN(16), service action 0x10 = READ CAPACITY(16).
        PUCHAR out = (PUCHAR)buffer;
        ULONGLONG last = (ULONGLONG)(ext->Blocks - 1);
        if ((cdb[1] & 0x1F) != 0x10 || out == NULL || Srb->DataTransferLength < 32) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return TRUE;
        }
        RtlZeroMemory(out, 32);
        out[0] = (UCHAR)(last >> 56); out[1] = (UCHAR)(last >> 48);
        out[2] = (UCHAR)(last >> 40); out[3] = (UCHAR)(last >> 32);
        out[4] = (UCHAR)(last >> 24); out[5] = (UCHAR)(last >> 16);
        out[6] = (UCHAR)(last >> 8);  out[7] = (UCHAR)(last);
        out[8] = (UCHAR)(ext->BlockBytes >> 24); out[9] = (UCHAR)(ext->BlockBytes >> 16);
        out[10] = (UCHAR)(ext->BlockBytes >> 8); out[11] = (UCHAR)(ext->BlockBytes);
        Srb->DataTransferLength = 32;
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_MODE_SENSE6:
    case NVMEOFND_OPC_MODE_SENSE10: {
        BOOLEAN ten = (op == NVMEOFND_OPC_MODE_SENSE10);
        ULONG hdr = ten ? 8 : 4;
        PUCHAR out = (PUCHAR)buffer;
        if (out == NULL || Srb->DataTransferLength < hdr) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return TRUE;
        }
        RtlZeroMemory(out, hdr);
        if (ten) {
            out[1] = (UCHAR)(hdr - 2);          // mode data length = everything after this field
        } else {
            out[0] = (UCHAR)(hdr - 1);
        }
        Srb->DataTransferLength = hdr;          // no block descriptor, no pages
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_READ6:
    case NVMEOFND_OPC_WRITE6:
    case NVMEOFND_OPC_READ10:
    case NVMEOFND_OPC_WRITE10:
    case NVMEOFND_OPC_READ16:
    case NVMEOFND_OPC_WRITE16: {
        BOOLEAN isRead = (op == NVMEOFND_OPC_READ6 || op == NVMEOFND_OPC_READ10 ||
                          op == NVMEOFND_OPC_READ16);
        ULONG lba = 0, blocks = 0, offset, bytes;
        ULONGLONG lba64 = 0;

        if (op == NVMEOFND_OPC_READ6 || op == NVMEOFND_OPC_WRITE6) {
            lba    = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
            blocks = (cdb[4] == 0) ? 256u : (ULONG)cdb[4];
        } else if (op == NVMEOFND_OPC_READ10 || op == NVMEOFND_OPC_WRITE10) {
            lba    = ((ULONG)cdb[2] << 24) | ((ULONG)cdb[3] << 16) | ((ULONG)cdb[4] << 8) | cdb[5];
            blocks = ((ULONG)cdb[7] << 8) | cdb[8];
        } else {
            lba64  = ((ULONGLONG)cdb[2] << 56) | ((ULONGLONG)cdb[3] << 48) |
                     ((ULONGLONG)cdb[4] << 40) | ((ULONGLONG)cdb[5] << 32) |
                     ((ULONGLONG)cdb[6] << 24) | ((ULONGLONG)cdb[7] << 16) |
                     ((ULONGLONG)cdb[8] << 8)  | (ULONGLONG)cdb[9];
            blocks = ((ULONG)cdb[10] << 24) | ((ULONG)cdb[11] << 16) |
                     ((ULONG)cdb[12] << 8)  | (ULONG)cdb[13];
            lba    = (lba64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (ULONG)lba64;
        }

        if (blocks == 0) {
            Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
            return TRUE;
        }
        if (lba64 > 0xFFFFFFFFull || lba >= ext->Blocks || blocks > (ext->Blocks - lba)) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x21, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return TRUE;
        }
        offset = lba * ext->BlockBytes;
        bytes  = blocks * ext->BlockBytes;
        if (bytes > Srb->DataTransferLength) {
            bytes = Srb->DataTransferLength;
        }
        if (buffer != NULL) {
            if (isRead) {
                RtlCopyMemory(buffer, (PUCHAR)ext->Lun + offset, bytes);
                ext->Reads++;
                ext->BytesOut += bytes;
            } else {
                RtlCopyMemory((PUCHAR)ext->Lun + offset, buffer, bytes);
                ext->Writes++;
                ext->BytesIn += bytes;
            }
        }
        ext->IoBytes += bytes;
        Srb->DataTransferLength = bytes;
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_SYNC_CACHE10:
    case NVMEOFND_OPC_SYNC_CACHE16:
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;

    case NVMEOFND_OPC_REQUEST_SENSE: {
        PUCHAR out = (PUCHAR)buffer;
        if (out != NULL && Srb->DataTransferLength >= 18) {
            RtlZeroMemory(out, 18);
            out[0] = 0x70;
            out[7] = 10;
            Srb->DataTransferLength = 18;
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_REPORT_LUNS: {
        PUCHAR out = (PUCHAR)buffer;
        if (out != NULL && Srb->DataTransferLength >= 16) {
            RtlZeroMemory(out, 16);
            out[3] = 8;
            Srb->DataTransferLength = 16;
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    default:
        NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
        return TRUE;
    }
}

static BOOLEAN NvmeofndResetBus(PVOID DeviceExtension, ULONG PathId)
{
    NvmeofndTrace(L"TraceResetBus", PathId);
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(PathId);
    return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS NvmeofndAdapterControl(PVOID DeviceExtension,
                                                          SCSI_ADAPTER_CONTROL_TYPE ControlType,
                                                          PVOID Parameters)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    NvmeofndTrace(L"TraceAdapterControl", (ULONG)ControlType);

    // ScsiQuerySupportedControlTypes is a REQUIRED handshake, not a courtesy call.
    // StorPort asks which control types the miniport handles and the miniport must
    // ANSWER by writing into the caller's list.  Returning success while leaving
    // that list untouched makes the adapter fail to start - measured here as problem
    // 10 with HwFindAdapter completing and HwInitialize never being reached.
    if (ControlType == ScsiQuerySupportedControlTypes) {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        ULONG i;
        if (list == NULL) {
            return ScsiAdapterControlUnsuccessful;
        }
        NvmeofndTrace(L"TraceCtlListMax", list->MaxControlType);
        for (i = 0; i < list->MaxControlType; i++) {
            list->SupportedTypeList[i] = FALSE;
        }
        if (list->MaxControlType > (ULONG)ScsiStopAdapter) {
            list->SupportedTypeList[ScsiStopAdapter] = TRUE;   // we free the RAM LUN on stop
        }
        return ScsiAdapterControlSuccess;
    }

    if (ControlType == ScsiStopAdapter && ext->Lun != NULL) {
        StorPortFreePool(DeviceExtension, ext->Lun);
        ext->Lun = NULL;
        ext->Ready = FALSE;
    }
    return ScsiAdapterControlSuccess;
}

// ---------------------------------------------------------------------------
//  DriverEntry
// ---------------------------------------------------------------------------
NTSTATUS DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    NvmeofndTrace(L"TraceDriverEntry", 1);
    HW_INITIALIZATION_DATA hw = {};

    hw.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
    hw.AdapterInterfaceType     = Internal;          // virtual: no bus, no hardware
    hw.HwInitialize             = NvmeofndInitialize;
    hw.HwStartIo                = NvmeofndStartIo;
    hw.HwFindAdapter            = NvmeofndFindAdapter;
    hw.HwResetBus               = NvmeofndResetBus;
    hw.HwAdapterControl         = NvmeofndAdapterControl;
    hw.DeviceExtensionSize      = sizeof(NVMEOFND_EXTENSION);
    hw.SpecificLuExtensionSize  = 0;
    hw.SrbExtensionSize         = 0;
    hw.NumberOfAccessRanges     = 0;
    hw.MapBuffers               = STOR_MAP_ALL_BUFFERS;
    hw.NeedPhysicalAddresses    = FALSE;
    hw.TaggedQueuing            = TRUE;
    hw.AutoRequestSense         = TRUE;
    hw.MultipleRequestPerLu     = TRUE;
    hw.ReceiveEvent             = FALSE;
    hw.PortVersionFlags         = 0;

    { NTSTATUS rc = StorPortInitialize(DriverObject, RegistryPath, &hw, NULL);
      NvmeofndTrace(L"TraceStorPortInitRc", (ULONG)rc);
      NvmeofndTrace(L"TraceHwStructSize", (ULONG)hw.HwInitializationDataSize);
      return rc; }
}
