// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
//
// D0 step 0: can this machine BUILD and LOAD a kernel driver at all?
//
// Nothing about RDMA is in here on purpose.  Before asking whether NDKPI works, the cheaper
// question is whether the toolchain produces a .sys and whether the kernel accepts it - and
// those two failures look completely different from "the provider said no", so they must be
// ruled out first.  Step 1 (opening an NDK adapter through NDKPI) is the next file, not this one.
#include <ntddk.h>

static void NdProbeUnload(PDRIVER_OBJECT driver) {
    UNREFERENCED_PARAMETER(driver);
    DbgPrint("[ndprobe] unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);
    driver->DriverUnload = NdProbeUnload;
    DbgPrint("[ndprobe] loaded: D0 step 0 - the toolchain and the loader both work\n");
    return STATUS_SUCCESS;
}