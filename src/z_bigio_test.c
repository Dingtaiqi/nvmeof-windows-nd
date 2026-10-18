/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * z_bigio_test.c - measure and verify large I/O through Z: with uncached I/O.
 *
 * Run this after the driver rebuild that understands nvmeofnd_chunkkb and a
 * device restart with nvmeofnd_chunkkb = 64.  A normal file copy would be served
 * from the NTFS cache and never reach the NVMe/TCP transport, so both directions
 * use FILE_FLAG_NO_BUFFERING (and the write also uses FILE_FLAG_WRITE_THROUGH):
 * every byte has to cross the miniport, the transport and the target.
 *
 * It writes 2 MiB, reads it back, compares byte for byte and prints MB/s.  The
 * free space on this Z: is about 4.7 MiB, so 2 MiB is the largest comfortable
 * size; the file is deleted at the end.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#define TEST_BYTES  (2u * 1024u * 1024u)
#define TEST_FILE   "Z:\\bigio-test.bin"

static double NowSeconds(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static int ReadChunkKb(void)
{
    HKEY k;
    DWORD v = 0, n = sizeof(v), type = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\NVMeoFProbe", 0, KEY_READ, &k) != ERROR_SUCCESS) {
        return -1;
    }
    if (RegQueryValueExA(k, "nvmeofnd_chunkkb", NULL, &type, (LPBYTE)&v, &n) != ERROR_SUCCESS
        || type != REG_DWORD) {
        v = (DWORD)-1;
    }
    RegCloseKey(k);
    return (int)v;
}

int main(void)
{
    uint8_t* buf;
    HANDLE h;
    DWORD done;
    double t0, t1;
    int i;
    int chunkKb = ReadChunkKb();

    printf("=== Z: large I/O through the NVMe/TCP miniport ===\n");
    printf("    nvmeofnd_chunkkb = %d%s\n", chunkKb,
           chunkKb == 64 ? " (one 64 KiB NVMe command per transfer)" : " (4 KiB default)");

    buf = (uint8_t*)VirtualAlloc(NULL, TEST_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buf == NULL) { printf("VirtualAlloc failed %lu\n", GetLastError()); return 2; }
    for (i = 0; i < (int)TEST_BYTES; i++) {
        buf[i] = (uint8_t)((i * 31 + 7) ^ (i >> 9));
    }

    DeleteFileA(TEST_FILE);
    h = CreateFileA(TEST_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("[FAIL] create %s (error %lu)\n", TEST_FILE, GetLastError());
        return 1;
    }
    t0 = NowSeconds();
    if (!WriteFile(h, buf, TEST_BYTES, &done, NULL) || done != TEST_BYTES) {
        printf("[FAIL] write %u bytes (error %lu)\n", TEST_BYTES, GetLastError());
        CloseHandle(h);
        return 1;
    }
    t1 = NowSeconds();
    CloseHandle(h);
    printf("[PASS] wrote %u KiB with FILE_FLAG_NO_BUFFERING|WRITE_THROUGH in %.3f s = %.2f MB/s\n",
           TEST_BYTES / 1024u, t1 - t0, (TEST_BYTES / 1048576.0) / (t1 - t0));

    memset(buf, 0, TEST_BYTES);
    h = CreateFileA(TEST_FILE, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                    FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("[FAIL] reopen for read (error %lu)\n", GetLastError());
        return 1;
    }
    t0 = NowSeconds();
    if (!ReadFile(h, buf, TEST_BYTES, &done, NULL) || done != TEST_BYTES) {
        printf("[FAIL] read back (error %lu)\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    t1 = NowSeconds();
    CloseHandle(h);
    printf("[PASS] read back %u KiB uncached in %.3f s = %.2f MB/s\n",
           TEST_BYTES / 1024u, t1 - t0, (TEST_BYTES / 1048576.0) / (t1 - t0));

    {
        int bad = 0;
        for (i = 0; i < (int)TEST_BYTES; i++) {
            if (buf[i] != (uint8_t)((i * 31 + 7) ^ (i >> 9))) { bad++; if (bad < 4) printf("       first mismatch at %d\n", i); }
        }
        if (bad == 0) { printf("[PASS] all %u bytes match the pattern\n", TEST_BYTES); }
        else { printf("[FAIL] %d bytes differ\n", bad); }
    }

    DeleteFileA(TEST_FILE);
    VirtualFree(buf, 0, MEM_RELEASE);
    return 0;
}
