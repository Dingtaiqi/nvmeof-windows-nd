/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * nvmeof_wsktest.c - one load of the WSK lifecycle, to exercise the IRP
 * completion pattern in nvmeof_tcpk.c and nothing else.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * nvmeof_tcpk.c was written against one assumption that had never executed:
 * that a WSK downcall (WskSocket / WskBind / WskConnect / WskCloseSocket) takes
 * only an IRP, that WSK completes that IRP through the IRP's own completion
 * routine (IO_COMPLETION_ROUTINE, attached with IoSetCompletionRoutine), that
 * the routine must return STATUS_MORE_PROCESSING_REQUIRED, and that the caller
 * then owns the IRP and must free it with IoFreeIrp.  If any part of that is
 * wrong the failure modes are a double free (0x139 KERNEL_SECURITY_CHECK_FAILURE
 * or 0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED), or a WSK request that is still
 * outstanding when the client deregisters - and WskDeregister then blocks
 * forever with no bugcheck and no dump at all.
 *
 * So this driver does the smallest thing that runs that pattern end to end:
 *
 *   1. WskRegister                      (client NPI, version 1.0)
 *   2. WskCaptureProviderNPI            (bounded wait, 5 s)
 *   3. NvtcpOpen(127.0.0.1, port 9)     - a port nothing listens on, EXPECTED to
 *                                         fail: socket create, bind, connect to
 *                                         a refusing target, completion, error
 *                                         path, close, free
 *   4. NvtcpOpen(127.0.0.1, port 9)     again - two iterations catch a bug that
 *                                         only bites on first use, and state left
 *                                         behind by the first failed attempt
 *   5. NvtcpClose(NULL)                 - must be safe, must not crash
 *   6. WskReleaseProviderNPI
 *   7. WskDeregister                    - genuinely last.  This is the trap: if
 *                                         any WSK request is still outstanding it
 *                                         never returns, and the only evidence is
 *                                         the log line written just before it.
 *   8. elapsed time, and wsktest_done = 1
 *
 * Everything is logged to the registry AS IT HAPPENS, one ZwSetValueKey per
 * step, so that a bugcheck still leaves the evidence on disk.  That is the same
 * logf/writeLog shape as src/driver/nvmeofk.c (which is the proven pattern on
 * this machine), with one deliberate difference: the buffer is converted to
 * UTF-16 before it is written, so the value is a well-formed UTF-16 REG_SZ that
 * reg.exe, PowerShell and regedit all render correctly.  The log text itself is
 * plain ASCII.
 *
 * WHAT IT DELIBERATELY DOES NOT DO: no StorPort, no device object, no I/O
 * dispatch table, no adapter enumeration, no route/DNS/interface change, no file
 * I/O.  The only network action is the loopback connect above, which goes
 * nowhere.  It never touches nvmeof_tcpk.c's internals; it only calls the four
 * public functions in nvmeof_tcpk.h.
 *
 * THE ENTRY POINT IS NOT CALLED DriverEntry, and that is not a mistake:
 * nvmeof_tcpk.c already defines a stub DriverEntry of its own (it exists only so
 * that file links into a complete driver), so linking both objects with
 * /ENTRY:DriverEntry is a duplicate-symbol error.  This file therefore defines
 * WsktestDriverEntry and the image is linked with /ENTRY:WsktestDriverEntry.
 * The kernel calls whatever address the PE header points at; the name of the
 * symbol is not part of the contract.  Nothing in nvmeof_tcpk.c is changed, and
 * its unused stub is never called.
 *
 * READING THE RESULT
 * ------------------
 *   HKLM\Software\NVMeoFProbe\wsktest_log          (REG_SZ)   the whole story
 *   HKLM\Software\NVMeoFProbe\wsktest_stage        (REG_DWORD) last step entered
 *   HKLM\Software\NVMeoFProbe\wsktest_open1        (REG_DWORD) NTSTATUS of open #1
 *   HKLM\Software\NVMeoFProbe\wsktest_open2        (REG_DWORD) NTSTATUS of open #2
 *   HKLM\Software\NVMeoFProbe\wsktest_close_null   (REG_DWORD) NvtcpClose(NULL)
 *   HKLM\Software\NVMeoFProbe\wsktest_elapsed_ms   (REG_DWORD) DriverEntry total
 *   HKLM\Software\NVMeoFProbe\wsktest_slow         (REG_DWORD) 1 if over budget
 *   HKLM\Software\NVMeoFProbe\wsktest_abort_stage  (REG_DWORD) set only on abort
 *   HKLM\Software\NVMeoFProbe\wsktest_done         (REG_DWORD) 1 only at the end
 *
 *   wsktest_done = 1 present  -> the completion pattern works and WskDeregister
 *                                returned.
 *   wsktest_stage = 7 and no wsktest_done -> WskDeregister is blocking, i.e. a
 *                                WSK request was still outstanding: a hang, not
 *                                a crash, and exactly the failure this test is
 *                                looking for.
 */

#include "nvmeof_tcpk.h"

/* va_list/va_start/va_arg for the log formatter below; the compiler's own
 * header, no CRT.  nvmeof_tcpk.c needs the same include for the same reason. */
#include <stdarg.h>

/* ------------------------------------------------------------------------- */
/*  Constants                                                                */
/* ------------------------------------------------------------------------- */

/* The registry key every probe and driver in this project writes to. */
#define WSKTEST_REG_KEY            L"\\Registry\\Machine\\Software\\NVMeoFProbe"

/* 127.0.0.1 in network byte order: the bytes in memory are 7F 00 00 01, so the
 * 32-bit value - read as a number - is 0x0100007F.  That is the same
 * "ipv4NetworkOrder" convention NvtcpOpen documents and nvmeofk.c uses. */
#define WSKTEST_LOOPBACK_NET_ORDER 0x0100007Fu

/* Port 9 (discard).  Nothing listens on it here, so connect is REFUSED, which is
 * the point: the failing path is the one that exercises error handling, close
 * and free. */
#define WSKTEST_DEAD_PORT          9u

/* WskCaptureProviderNPI: bounded wait.  WSK_INFINITE_WAIT (0xFFFFFFFF) is the
 * documented "wait forever" value and is exactly what a test must not use. */
#define WSKTEST_CAPTURE_WAIT_MS    5000u

/* If the whole DriverEntry takes longer than this, the test has failed and the
 * fact is recorded as wsktest_slow.  (A DriverEntry wedged inside WskDeregister
 * never reaches that check at all: that case is caught from outside, by the
 * load harness timing out with wsktest_stage = 7 and no wsktest_done.) */
#define WSKTEST_BUDGET_MS          60000u

/* ------------------------------------------------------------------------- */
/*  Logging: a kernel driver's only output is the registry.                  */
/*                                                                           */
/*  logf/logRaw/logNum are nvmeofk.c's formatter, kept in the same shape and   */
/*  with the same supported specifiers (%s %u %d %x %X %p %c %%), because a    */
/*  hand-rolled formatter is the only kind a driver can link: the RtlStringCbV- */
/*  Printf family and the ANSI printf helpers pull UCRT symbols.               */
/* ------------------------------------------------------------------------- */

#define WSKTEST_LOG_CHARS 8192

static char   g_log[WSKTEST_LOG_CHARS];
static size_t g_logLen = 0;

/* ASCII -> UTF-16 staging buffer.  One WCHAR per logged byte is exactly enough,
 * because every byte the formatter produces is ASCII by construction. */
static WCHAR  g_logW[WSKTEST_LOG_CHARS + 1];

static void LogRaw(const char* s, size_t n)
{
    if (g_logLen + n + 3 >= sizeof(g_log)) {
        return;
    }
    RtlCopyMemory(g_log + g_logLen, s, n);
    g_logLen += n;
}

static void LogNum(unsigned long long v, int base, int width, int upper, int padZero)
{
    char tmp[24];
    int i = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = digits[(int)(v % (unsigned)base)];
        v /= (unsigned)base;
    }
    while (i < width && i < (int)sizeof(tmp)) {
        tmp[i++] = padZero ? '0' : ' ';
    }
    while (i > 0) {
        LogRaw(&tmp[--i], 1);
    }
}

static void LogF(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    for (const char* p = fmt; *p != 0; p++) {
        long width = 0L;
        int padZero = 0;
        int longs = 0;

        if (*p != '%') {
            LogRaw(p, 1);
            continue;
        }
        p++;
        if (*p == '0') { padZero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l') { longs++; p++; }
        switch (*p) {
        case 'u':
            /* LLP64: only %llu reads a 64-bit argument, so this file uses %u for
             * everything 32-bit and never %lu. */
            LogNum((longs >= 2) ? va_arg(ap, unsigned long long)
                                : (unsigned long long)va_arg(ap, unsigned int),
                   (int)10, (int)width, 0, padZero);
            break;
        case 'd': {
            int iv = va_arg(ap, int);
            if (iv < 0) { LogRaw("-", 1); iv = -iv; }
            LogNum((unsigned long long)iv, (int)10, (int)width, 0, padZero);
            break;
        }
        case 'X':
            LogNum((unsigned long long)va_arg(ap, unsigned int), 16, (int)width, 1, padZero);
            break;
        case 'x':
            LogNum((unsigned long long)va_arg(ap, unsigned int), 16, (int)width, 0, padZero);
            break;
        case 'p':
            LogRaw("0x", 2);
            LogNum((unsigned long long)(ULONG_PTR)va_arg(ap, PVOID), 16, 16, 0, 1);
            break;
        case 's': {
            const char* sv = va_arg(ap, const char*);
            if (sv == NULL) { sv = "(null)"; }
            while (*sv != 0) { LogRaw(sv, 1); sv++; }
            break;
        }
        case 'c': {
            char cv = (char)va_arg(ap, int);
            LogRaw(&cv, 1);
            break;
        }
        case '%':
            LogRaw("%", 1);
            break;
        default:
            LogRaw("%", 1);
            LogRaw(p, 1);
            break;
        }
    }
    va_end(ap);
    LogRaw("\r\n", 2);
}

/* Publish the whole buffer, rewritten after every step.  Registry calls are
 * PASSIVE_LEVEL only and DriverEntry is PASSIVE_LEVEL, so no IRQL guard is
 * needed here - unlike nvmeof_tcpk.c, which can be called at DISPATCH_LEVEL. */
static void WriteLog(void)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    ULONG disp = 0;
    size_t i;

    if (g_logLen == 0) {
        return;
    }
    RtlInitUnicodeString(&keyName, WSKTEST_REG_KEY);
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, &disp))) {
        return;
    }
    /* ASCII -> UTF-16, so the value is a real UTF-16 string.  (nvmeofk.c hands
     * the ASCII bytes straight to REG_SZ, which reads back as mojibake through
     * most tools; this is the one place this file departs from it, on purpose.) */
    for (i = 0; i < g_logLen; i++) {
        g_logW[i] = (WCHAR)(UCHAR)g_log[i];
    }
    g_logW[g_logLen] = 0;

    RtlInitUnicodeString(&valueName, L"wsktest_log");
    ZwSetValueKey(key, &valueName, 0, REG_SZ, g_logW,
                  (ULONG)((g_logLen + 1) * sizeof(WCHAR)));
    ZwClose(key);
}

/* One DWORD per interesting fact, so a human does not have to parse the log. */
static void TraceValue(PCWSTR name, ULONG v)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    ULONG disp = 0;

    RtlInitUnicodeString(&keyName, WSKTEST_REG_KEY);
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, &disp))) {
        return;
    }
    RtlInitUnicodeString(&valueName, name);
    ZwSetValueKey(key, &valueName, 0, REG_DWORD, &v, sizeof(v));
    ZwClose(key);
}

/* ------------------------------------------------------------------------- */
/*  Time: every line carries milliseconds since DriverEntry entry, so the log  */
/*  shows which step ate the time even when the total is within budget.        */
/* ------------------------------------------------------------------------- */

static ULONGLONG g_entryTime = 0;

static ULONG ElapsedMs(void)
{
    ULONGLONG now = KeQueryInterruptTime();      /* 100 ns units */
    return (ULONG)((now - g_entryTime) / 10000ULL);
}

/* ------------------------------------------------------------------------- */
/*  WSK registration state (static: these must outlive any call)             */
/* ------------------------------------------------------------------------- */

static WSK_REGISTRATION    g_registration;
static WSK_CLIENT_NPI      g_wskClient;
static WSK_CLIENT_DISPATCH g_wskDispatch;
static WSK_PROVIDER_NPI    g_wskProvider;
static BOOLEAN             g_registered = FALSE;

/* ------------------------------------------------------------------------- */
/*  Unload: nothing to do, and that is the point of the second load test.     */
/*                                                                           */
/*  Every WSK object is released before DriverEntry returns, so unloading this */
/*  driver must not have to tear anything down.  All this does is leave a mark */
/*  that the unload happened and did not crash.                               */
/* ------------------------------------------------------------------------- */

static VOID WsktestUnload(PDRIVER_OBJECT Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    LogF("[%6u ms] DriverUnload: reached; DriverEntry already released every WSK object",
         ElapsedMs());
    /* NOT touched on unload: a registry write from DriverUnload is what wedged
     * this machine on 2026-10-17.  The log stays in the ring only; the evidence
     * from the run that matters is already on disk by the time unload happens. */
    /* WriteLog(); */
    /* NOT touched on unload: a registry write from DriverUnload is what wedged
     * this machine on 2026-10-17.  The log stays in the ring only; the evidence
     * from the run that matters is already on disk by the time unload happens. */
    /* TraceValue(L"wsktest_unload", 1); */
}

/* ------------------------------------------------------------------------- */
/*  Entry: the whole test, in order, once.                                   */
/*                                                                           */
/*  Named WsktestDriverEntry, not DriverEntry, because nvmeof_tcpk.c already   */
/*  defines a stub DriverEntry and both objects go into one image; the linker  */
/*  is told /ENTRY:WsktestDriverEntry.  Same prototype, same contract.         */
/* ------------------------------------------------------------------------- */

NTSTATUS WsktestDriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    NTSTATUS st;
    NTSTATUS stRegister = STATUS_UNSUCCESSFUL;
    NTSTATUS stCapture = STATUS_UNSUCCESSFUL;
    NTSTATUS stOpen1 = STATUS_UNSUCCESSFUL;
    NTSTATUS stOpen2 = STATUS_UNSUCCESSFUL;
    NTSTATUS stCloseNull = STATUS_UNSUCCESSFUL;
    PVOID    conn1 = NULL;
    PVOID    conn2 = NULL;
    ULONG    elapsed;

    UNREFERENCED_PARAMETER(RegistryPath);

    g_entryTime = KeQueryInterruptTime();
    /* Set first, so that the driver can always be stopped again: a driver with
     * no unload routine refuses SERVICE_CONTROL_STOP and would have to be
     * deleted with the machine still holding it. */
    Driver->DriverUnload = WsktestUnload;

    LogF("=== wsktest: WSK IRP completion pattern, one load, loopback 127.0.0.1:9 ===");
    LogF("[%6u ms] entry point (WsktestDriverEntry) entered; all log times are ms from here",
         ElapsedMs());
    LogF("expected: both NvtcpOpen calls FAIL (nothing listens on port 9) and that failure"
         " runs socket/bind/connect/completion/close/free");
    WriteLog();

    /* --------------------------------------------------------------------- */
    /*  Step 1: WskRegister                                                  */
    /* --------------------------------------------------------------------- */
    TraceValue(L"wsktest_stage", 1);

    /* Version 1.0 is what nvmeofk.c registers with on this machine; a client
     * that asks for a version the provider does not implement gets
     * STATUS_NOINTERFACE from the capture below, not from here. */
    g_wskDispatch.Version = MAKE_WSK_VERSION(1, 0);
    g_wskDispatch.Reserved = 0;
    g_wskDispatch.WskClientEvent = NULL;      /* no event callbacks are used */
    g_wskClient.ClientContext = NULL;
    g_wskClient.Dispatch = &g_wskDispatch;

    st = WskRegister(&g_wskClient, &g_registration);
    stRegister = st;
    if (NT_SUCCESS(st)) {
        g_registered = TRUE;
    }
    LogF("[%6u ms] step 1: WskRegister(client NPI version 1.0) = 0x%08X", ElapsedMs(),
         (unsigned)st);
    WriteLog();

    if (!g_registered) {
        /* Nothing else can run, and WskDeregister must NOT be called for a
         * registration that never succeeded. */
        TraceValue(L"wsktest_abort_stage", 1);
        LogF("TEST ABORTED at step 1: WskRegister failed, so no WSK call is legal from here");
        WriteLog();
        goto Finish;
    }

    /* --------------------------------------------------------------------- */
    /*  Step 2: WskCaptureProviderNPI, bounded at 5 s                        */
    /* --------------------------------------------------------------------- */
    TraceValue(L"wsktest_stage", 2);

    st = WskCaptureProviderNPI(&g_registration, WSKTEST_CAPTURE_WAIT_MS, &g_wskProvider);
    stCapture = st;
    LogF("[%6u ms] step 2: WskCaptureProviderNPI(wait %u ms) = 0x%08X  dispatch=%p  client=%p",
         ElapsedMs(), (unsigned)WSKTEST_CAPTURE_WAIT_MS, (unsigned)st,
         (PVOID)g_wskProvider.Dispatch, g_wskProvider.Client);
    WriteLog();

    if (!NT_SUCCESS(st)) {
        TraceValue(L"wsktest_abort_stage", 2);
        LogF("TEST ABORTED at step 2: no provider NPI, so NvtcpOpen cannot be called");
        WriteLog();
        goto Deregister;      /* still registered: WskDeregister is still required */
    }

    /* --------------------------------------------------------------------- */
    /*  Step 3: first NvtcpOpen - EXPECTED TO FAIL                           */
    /*                                                                       */
    /*  This is the call that runs the pattern: ExAllocatePool2, IoAllocateIrp,*/
    /*  IoSetCompletionRoutine, IoAllocateMdl, WskSocket, WskBind, WskConnect */
    /*  (refused), wait, IoFreeIrp, WskCloseSocket, wait, IoFreeIrp, free.    */
    /* --------------------------------------------------------------------- */
    TraceValue(L"wsktest_stage", 3);

    LogF("[%6u ms] step 3: NvtcpOpen(127.0.0.1:9) #1, failure is the expected result",
         ElapsedMs());
    WriteLog();

    stOpen1 = NvtcpOpen(&g_registration, &g_wskProvider, WSKTEST_LOOPBACK_NET_ORDER,
                        (USHORT)WSKTEST_DEAD_PORT, &conn1);
    /* Recorded immediately, before anything else can go wrong. */
    TraceValue(L"wsktest_open1", (ULONG)stOpen1);
    LogF("[%6u ms] step 3: NvtcpOpen #1 = 0x%08X  conn=%p  %s", ElapsedMs(),
         (unsigned)stOpen1, conn1,
         NT_SUCCESS(stOpen1) ? "UNEXPECTED SUCCESS" : "(failure, as expected)");
    WriteLog();

    if (NT_SUCCESS(stOpen1)) {
        /* The dead port answered.  A live socket left behind would make
         * WskDeregister block forever, so close it and say so loudly. */
        NTSTATUS stc = NvtcpClose(conn1);
        TraceValue(L"wsktest_open1_unexpected", 1);
        LogF("[%6u ms] step 3: !! unexpected success; NvtcpClose(conn1) = 0x%08X", ElapsedMs(),
             (unsigned)stc);
        WriteLog();
        conn1 = NULL;
    }

    /* --------------------------------------------------------------------- */
    /*  Step 4: second NvtcpOpen - also EXPECTED TO FAIL                     */
    /*                                                                       */
    /*  Two iterations in one load: the first use of the async context, the   */
    /*  pool, the statically initialised connection list and the spin lock is */
    /*  the interesting one, so a bug that only bites on first use would hide */
    /*  in a single-iteration test.  And if the failed first attempt left     */
    /*  state behind, this call is what trips over it.                        */
    /* --------------------------------------------------------------------- */
    TraceValue(L"wsktest_stage", 4);

    LogF("[%6u ms] step 4: NvtcpOpen(127.0.0.1:9) #2, same expectations", ElapsedMs());
    WriteLog();

    stOpen2 = NvtcpOpen(&g_registration, &g_wskProvider, WSKTEST_LOOPBACK_NET_ORDER,
                        (USHORT)WSKTEST_DEAD_PORT, &conn2);
    TraceValue(L"wsktest_open2", (ULONG)stOpen2);
    LogF("[%6u ms] step 4: NvtcpOpen #2 = 0x%08X  conn=%p  %s", ElapsedMs(),
         (unsigned)stOpen2, conn2,
         NT_SUCCESS(stOpen2) ? "UNEXPECTED SUCCESS" : "(failure, as expected)");
    LogF("[%6u ms] step 4: open #1 = 0x%08X, open #2 = 0x%08X -> %s", ElapsedMs(),
         (unsigned)stOpen1, (unsigned)stOpen2,
         (stOpen1 == stOpen2) ? "IDENTICAL, no state leaked between the two attempts"
                              : "DIFFERENT, the second attempt behaved differently");
    WriteLog();

    if (NT_SUCCESS(stOpen2)) {
        NTSTATUS stc = NvtcpClose(conn2);
        TraceValue(L"wsktest_open2_unexpected", 1);
        LogF("[%6u ms] step 4: !! unexpected success; NvtcpClose(conn2) = 0x%08X", ElapsedMs(),
             (unsigned)stc);
        WriteLog();
        conn2 = NULL;
    }

    /* --------------------------------------------------------------------- */
    /*  Step 5: NvtcpClose(NULL) - must be safe, and must not free anything   */
    /* --------------------------------------------------------------------- */
    TraceValue(L"wsktest_stage", 5);

    stCloseNull = NvtcpClose(NULL);
    TraceValue(L"wsktest_close_null", (ULONG)stCloseNull);
    LogF("[%6u ms] step 5: NvtcpClose(NULL) = 0x%08X  (returned, no crash: pass)", ElapsedMs(),
         (unsigned)stCloseNull);
    WriteLog();

    /* --------------------------------------------------------------------- */
    /*  Step 6: WskReleaseProviderNPI                                        */
    /*                                                                       */
    /*  One release per successful capture.  This is the point after which no */
    /*  WSK downcall is legal at all, so it must come after every socket is   */
    /*  closed - which, for two failed opens, has already happened inside      */
    /*  NvtcpOpen.                                                           */
    /* --------------------------------------------------------------------- */
    TraceValue(L"wsktest_stage", 6);

    LogF("[%6u ms] step 6: calling WskReleaseProviderNPI", ElapsedMs());
    WriteLog();
    WskReleaseProviderNPI(&g_registration);
    LogF("[%6u ms] step 6: WskReleaseProviderNPI returned", ElapsedMs());
    WriteLog();

Deregister:
    /* --------------------------------------------------------------------- */
    /*  Step 7: WskDeregister - genuinely the last WSK step                   */
    /*                                                                       */
    /*  wsk.h: "WskDeregister will wait until all captured instances of the   */
    /*  provider NPI are released, any outstanding calls to functions in      */
    /*  WSK_PROVIDER_DISPATCH have returned, and all sockets are closed."     */
    /*  If one WSK request is still in flight this never returns: no bugcheck, */
    /*  no dump, just a DriverEntry that never finishes.  The line below is    */
    /*  therefore the last thing written to disk before the answer is known.  */
    /* --------------------------------------------------------------------- */
    TraceValue(L"wsktest_stage", 7);

    LogF("[%6u ms] step 7: calling WskDeregister (blocks forever if anything is still"
         " outstanding)", ElapsedMs());
    WriteLog();

    WskDeregister(&g_registration);
    g_registered = FALSE;

    LogF("[%6u ms] step 7: WskDeregister returned", ElapsedMs());
    WriteLog();

Finish:
    /* --------------------------------------------------------------------- */
    /*  Step 8: budget check, then the done marker                           */
    /* --------------------------------------------------------------------- */
    elapsed = ElapsedMs();
    TraceValue(L"wsktest_elapsed_ms", elapsed);

    if (elapsed > WSKTEST_BUDGET_MS) {
        TraceValue(L"wsktest_slow", 1);
        LogF("TEST FAILED: DriverEntry took %u ms, over the %u ms budget", elapsed,
             (unsigned)WSKTEST_BUDGET_MS);
        WriteLog();
    }

    LogF("[%6u ms] steps: register=0x%08X capture=0x%08X open1=0x%08X open2=0x%08X"
         " closeNull=0x%08X", elapsed,
         (unsigned)stRegister, (unsigned)stCapture,
         (unsigned)stOpen1, (unsigned)stOpen2, (unsigned)stCloseNull);
    LogF("[%6u ms] wsktest_done = 1 (total %u ms, budget %u ms)", elapsed, elapsed,
         (unsigned)WSKTEST_BUDGET_MS);
    /* Order matters: the marker is written before the final log publish, so a
     * reader that sees wsktest_done = 1 always sees a log whose last line is
     * this one. */
    TraceValue(L"wsktest_done", 1);
    WriteLog();

    /* Always succeed: the log is the result.  A failed load would be deleted by
     * the service control manager and the evidence with it, and this driver has
     * nothing to fail at that a STOP of the service cannot clean up. */
    return STATUS_SUCCESS;
}
