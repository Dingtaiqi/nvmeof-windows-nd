// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
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
    NvmeofndTrace(L"TraceFindAdapter", 1);
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);
    UNREFERENCED_PARAMETER(Again);

    ConfigInfo->VirtualDevice            = TRUE;
    ConfigInfo->AdapterInterfaceType     = Internal;
    ConfigInfo->MaximumTransferLength    = 1024u * 1024u;
    ConfigInfo->NumberOfPhysicalBreaks   = 0x20;
    ConfigInfo->AlignmentMask            = 0;
    ConfigInfo->MaximumNumberOfTargets   = NVMEOFND_MAX_TARGETS;
    ConfigInfo->MaximumNumberOfLogicalUnits = NVMEOFND_MAX_LUNS;
    ConfigInfo->NumberOfBuses            = 1;
    ConfigInfo->ScatterGather            = TRUE;
    ConfigInfo->Master                   = TRUE;
    ConfigInfo->CachesData               = FALSE;
    ConfigInfo->MapBuffers               = STOR_MAP_ALL_BUFFERS;
    ConfigInfo->NeedPhysicalAddresses    = FALSE;
    ConfigInfo->TaggedQueuing            = TRUE;
    ConfigInfo->AutoRequestSense         = TRUE;
    ConfigInfo->SrbType                  = SRB_TYPE_SCSI_REQUEST_BLOCK;
    ConfigInfo->AddressType              = STORAGE_ADDRESS_TYPE_BTL8;
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
    StorPortNotification(BusChangeDetected, DeviceExtension, 0);
    NvmeofndTrace(L"TraceBusChange", 1);
    return TRUE;
}

// ---------------------------------------------------------------------------
//  HwStartIo - one SCSI command at a time, answered from the RAM LUN.
// ---------------------------------------------------------------------------
static BOOLEAN NvmeofndStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb)
{
    NvmeofndTrace(L"TraceStartIo", 1);
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    PUCHAR  cdb;
    PVOID   buffer = NULL;
    ULONG   status;

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

    status = StorPortGetSystemAddress(DeviceExtension, Srb, &buffer);
    if (status != STOR_STATUS_SUCCESS && (cdb[0] == NVMEOFND_OPC_READ10 || cdb[0] == NVMEOFND_OPC_READ16 || cdb[0] == NVMEOFND_OPC_READ6 || cdb[0] == NVMEOFND_OPC_WRITE10 || cdb[0] == NVMEOFND_OPC_WRITE16 || cdb[0] == NVMEOFND_OPC_WRITE6 ||
                                          cdb[0] == NVMEOFND_OPC_READ10 || cdb[0] == NVMEOFND_OPC_WRITE10 ||
                                          cdb[0] == NVMEOFND_OPC_INQUIRY)) {
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
        return TRUE;
    }

    switch (cdb[0]) {

    case NVMEOFND_OPC_TEST_UNIT_READY:
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;

    case NVMEOFND_OPC_INQUIRY: {
        ULONG want = Srb->DataTransferLength;
        ULONG give = sizeof(g_Inquiry);
        if (cdb[1] & 0x01) {                     // EVPD: no vital product pages here
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x24, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return TRUE;
        }
        if (buffer != NULL) {
            ULONG n = (want < give) ? want : give;
            RtlZeroMemory(buffer, want);
            RtlCopyMemory(buffer, g_Inquiry, n);
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_READ_CAPACITY10: {                 // 10-byte form: last LBA + block size
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

    case NVMEOFND_OPC_READ6:
    case NVMEOFND_OPC_WRITE6:
    case NVMEOFND_OPC_READ10:
    case NVMEOFND_OPC_WRITE10: {
        BOOLEAN isRead = (cdb[0] == NVMEOFND_OPC_READ10 || cdb[0] == NVMEOFND_OPC_READ16 || cdb[0] == NVMEOFND_OPC_READ6 || cdb[0] == NVMEOFND_OPC_READ10);
        ULONG lba, blocks, offset, bytes;

        if (cdb[0] == NVMEOFND_OPC_READ10 || cdb[0] == NVMEOFND_OPC_READ16 || cdb[0] == NVMEOFND_OPC_READ6 || cdb[0] == NVMEOFND_OPC_WRITE10 || cdb[0] == NVMEOFND_OPC_WRITE16 || cdb[0] == NVMEOFND_OPC_WRITE6) {   // 6-byte CDB
            lba    = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
            blocks = (cdb[4] == 0) ? 256u : (ULONG)cdb[4];
        } else {                                                  // 10-byte CDB
            lba    = ((ULONG)cdb[2] << 24) | ((ULONG)cdb[3] << 16) | ((ULONG)cdb[4] << 8) | cdb[5];
            blocks = ((ULONG)cdb[7] << 8) | cdb[8];
        }
        if (blocks == 0) {
            Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
            return TRUE;
        }
        if (lba >= ext->Blocks || blocks > (ext->Blocks - lba)) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x21, 0x00); // LBA out of range
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
        Srb->DataTransferLength = bytes;
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_SYNC_CACHE10:
        // A RAM LUN has nothing to flush; answering "done" is the truth here,
        // and a driver that claims otherwise makes every format take minutes.
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;

    case NVMEOFND_OPC_REQUEST_SENSE: {
        PUCHAR out = (PUCHAR)buffer;
        if (out != NULL && Srb->DataTransferLength >= 18) {
            RtlZeroMemory(out, 18);
            out[0] = 0x70;
            out[7] = 10;
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return TRUE;
    }

    case NVMEOFND_OPC_REPORT_LUNS: {
        PUCHAR out = (PUCHAR)buffer;
        if (out != NULL && Srb->DataTransferLength >= 16) {
            RtlZeroMemory(out, 16);
            out[3] = 8;                        // one LUN, 8 bytes of it
            out[9] = 0;                        // LUN 0
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
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(PathId);
    return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS NvmeofndAdapterControl(PVOID DeviceExtension,
                                                          SCSI_ADAPTER_CONTROL_TYPE ControlType,
                                                          PVOID Parameters)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    UNREFERENCED_PARAMETER(Parameters);

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
