<#
  Run.ps1 - suite runner driven by the one ordered $Suites registry below.

  Registry: initially exactly one suite (OverlayBinding) with two variants (deterministic, native). `-Suite All`
  expands to the registered rows only - it is an expansion, never an alias for a different suite - and a suite is
  registered only together with its complete fixture/manifest set. Bare `-ListSuites` (no -Suite passed)
  enumerates the whole registry (WB080 Task 2 repair B2); an explicit -Suite keeps selection behavior. Modes: Host only; there is no Live adapter, so
  Live and unknown suites fail explicitly with a FAIL receipt and a nonzero exit instead of pretending to run.

  OverlayBinding records two variants:
    deterministic : tests/wb_groups/overlay_binding.sources          - the fixture links its own recording
                    MinHook shim and substitutes the DXGI/MinHook boundary in-process.
    native        : tests/wb_groups/overlay_binding.native.sources   - compiled with WB_NATIVE_MINHOOK_SMOKE,
                    linked against the real MinHook sources, intercepting real DXGI D3D12 objects.

  A manifest line is one of: '# comment', '@include <dir>', '@define <NAME>', '@evidence <path>' (hashed into
  the receipt, never passed to the compiler) or a translation-unit path. Preflight walks EVERY selected suite's
  EVERY variant manifest before any compiler runs: a missing fixture/manifest/TU/include/evidence input, a TU
  named more than once in one manifest, or a TU the fixture textually includes fails there. A TU shared between
  two variant manifests (e.g. i18n.cpp) is expected - each variant compiles its own set - and is recorded, not
  treated as a duplicate.

  A variant row may carry `cflags` (the compiler's own flag set replaces the default '/W4 /WX' - CorePatternScan
  compiles the production core TU at its production warning level) and `cases` (a case list: the variant then
  compiles once and launches one fresh Job-supervised process per case, with the fixture directory then `--case`
  and the case name, recording variants/<variant>/cases/<case>/{result.json,stdout.log,stderr.log} plus the
  fixture's own cases/<case>.json machine receipt).

  Stage timeout authority lives in this file: CompileTimeoutSeconds (default 300) and TestTimeoutSeconds
  (default 120) per variant. Every stage is one child process created suspended inside its own Win32 job object
  (KILL_ON_JOB_CLOSE, non-inheritable handle) and assigned to that job before it is resumed, with stdout and
  stderr drained to completion; a timeout terminates the whole job and the receipt records the stage, PID, exit
  code and error. An outer supervisor may bound the whole run, but it must not add a competing stage timer.
  A suite passes only with real positive assertion counts: one `ASSERTIONS=<positive integer>` line per variant,
  aggregated over the executed variants; zero assertions can never pass.
#>
[CmdletBinding()]
param(
  [string]$Suite = 'OverlayBinding',
  [string]$Mode = 'Host',
  [string]$EvidenceRoot = 'tests/wb_groups/artifacts',
  [ValidateRange(1, 86400)][int]$CompileTimeoutSeconds = 300,
  [ValidateRange(1, 86400)][int]$TestTimeoutSeconds = 120,
  [switch]$ListSuites
)
$ErrorActionPreference = 'Stop'

$JOB_EMPTY_WAIT_MS = 10000

# ---------------------------------------------------------------------------------------------------------
# The one ordered suite registry. Selection, fixture path, manifests, executable/log names, run ids and receipt
# metadata are all derived from these rows; adding a suite means shipping its complete fixture + manifest set
# and adding one row here. 'All' expands exactly these rows.
# ---------------------------------------------------------------------------------------------------------
$Suites = @(
  [ordered]@{
    name = 'OverlayBinding'
    stem = 'overlay_binding'
    fixture = 'overlay_binding_tests.cpp'
    variants = @(
      [ordered]@{ name = 'deterministic'; manifest = 'overlay_binding.sources';        define = '';                       linksMinHook = $false; smoke = $false; exe = 'overlay_binding.exe' },
      [ordered]@{ name = 'native';        manifest = 'overlay_binding.native.sources'; define = 'WB_NATIVE_MINHOOK_SMOKE'; linksMinHook = $true;  smoke = $true;  exe = 'overlay_binding_native.exe' }
    )
  },
  [ordered]@{
    name = 'CorePatternScan'
    stem = 'core_pattern_scan'
    fixture = 'core_pattern_scan_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'core_pattern_scan.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'core_pattern_scan.exe'
        cflags = '/W3 /WX'   # the production core TU is built at /W3 (asi/cdmodkit/build.bat); see the manifest header
        cases = @('unique', 'ambiguous', 'core-policy', 'chunk-boundary', 'unreadable', 'malformed', 'cross-section', 'pattern63', 'pattern64', 'pattern65', 'cache-retry')
      }
    )
  },
  [ordered]@{
    name = 'InputHotkeys'
    stem = 'input_hotkeys'
    fixture = 'input_hotkeys_tests.cpp'
    variants = @(
      [ordered]@{ name = 'deterministic'; manifest = 'input_hotkeys.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'input_hotkeys.exe' }
    )
  },
  [ordered]@{
    name = 'Codec'
    stem = 'codec'
    fixture = 'codec_tests.cpp'
    variants = @(
      [ordered]@{ name = 'deterministic'; manifest = 'codec.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'codec.exe' }
    )
  },
  [ordered]@{
    name = 'AnchorTransform'
    stem = 'anchor_transform'
    fixture = 'anchor_transform_tests.cpp'
    variants = @(
      [ordered]@{ name = 'deterministic'; manifest = 'anchor_transform.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'anchor_transform.exe' }
    )
  },
  [ordered]@{
    name = 'ObjectLifetime'
    stem = 'object_lifetime'
    fixture = 'object_lifetime_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'object_lifetime.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'object_lifetime.exe'
        cflags = '/W3 /WX'   # this variant compiles the production core TU and editor.cpp, both built at /W3 upstream; see the manifest header
      }
    )
  },
  [ordered]@{
    name = 'InputOwnership'
    stem = 'input_ownership'
    fixture = 'input_ownership_tests.cpp'
    variants = @(
      [ordered]@{ name = 'deterministic'; manifest = 'input_ownership.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'input_ownership.exe' }
    )
  },
  [ordered]@{
    name = 'ProjectLifecycle'
    stem = 'project_lifecycle'
    fixture = 'project_lifecycle_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'project_lifecycle.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'project_lifecycle.exe'
        cflags = '/W3 /WX'   # this variant compiles the production core TU and editor.cpp, both built at /W3 upstream; see the manifest header
      }
    )
  },
  [ordered]@{
    name = 'PlacementResults'
    stem = 'placement_results'
    fixture = 'placement_results_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'placement_results.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'placement_results.exe'
        cflags = '/W3 /WX'   # this variant compiles the production core TU, built at /W3 upstream; see the manifest header
      }
    )
  },
  [ordered]@{
    name = 'GroupPlacement'
    stem = 'group_placement'
    fixture = 'group_placement_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'group_placement.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'group_placement.exe'
        cflags = '/W3 /WX'   # this variant compiles the production core TU and editor.cpp, both built at /W3 upstream; see the manifest header
      }
    )
  },
  [ordered]@{
    name = 'Export'
    stem = 'export'
    fixture = 'export_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'export.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'export.exe'
        cflags = '/W3 /WX'   # this variant compiles the production core TU, built at /W3 upstream; see the manifest header
      }
    )
  },
  [ordered]@{
    name = 'Grounding'
    stem = 'grounding'
    fixture = 'grounding_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'grounding.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'grounding.exe'
        cflags = '/W3 /WX'
      }
    )
  },
  [ordered]@{
    name = 'ProjectTable'
    stem = 'project_table'
    fixture = 'project_table_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'project_table.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'project_table.exe'
        cflags = '/W3 /WX'
      }
    )
  },
  [ordered]@{
    name = 'UiShell'
    stem = 'ui_shell'
    fixture = 'ui_shell_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'ui_shell.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'ui_shell.exe'
        cflags = '/W3 /WX' # actual production editor/CORE at their production warning level
      }
    )
  },
  [ordered]@{
    name = 'EditorCleanup'
    stem = 'editor_cleanup'
    fixture = 'editor_cleanup_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'editor_cleanup.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'editor_cleanup.exe'
        cflags = '/W3 /WX'   # this variant compiles the production core TU and editor.cpp, both built at /W3 upstream; see the manifest header
      }
    )
  },
  [ordered]@{
    name = 'I18nOverride'
    stem = 'i18n_override'
    fixture = 'i18n_override_tests.cpp'
    variants = @(
      [ordered]@{ name = 'deterministic'; manifest = 'i18n_override.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'i18n_override.exe' }
    )
  },
  [ordered]@{
    name = 'ReportProjection'
    stem = 'report_projection'
    fixture = 'report_projection_tests.cpp'
    variants = @(
      [ordered]@{
        name = 'deterministic'; manifest = 'report_projection.sources'; define = ''; linksMinHook = $false; smoke = $false; exe = 'report_projection.exe'
        cflags = '/W3 /WX' # links the actual production core TU at its production warning level
      }
    )
  }
)

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

public static class OverlayBindingStage
{
    public class Result
    {
        public bool Created, Assigned, Resumed, TimedOut, TimedOutTerminated, DescendantsTerminated, JobEmptyConfirmed;
        public int RootPid; public int RootExitCode = -1; public long DurationMs;
        public string Error = "";
        public uint ActiveAtRootExit = 0; public uint ActiveAfterCleanup = 0;
        public int[] PidsAtRootExit = new int[0];
        public int MsgNewProcess = 0, MsgExitProcess = 0, MsgActiveZero = 0, MsgTotal = 0;
        public string Stdout = "", Stderr = "";
    }

    const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;
    const int JobObjectBasicAccountingInformation = 1;
    const int JobObjectAssociateCompletionPortInformation = 7;
    const int JobObjectExtendedLimitInformation = 9;
    const uint CREATE_SUSPENDED = 0x4;
    const uint CREATE_NO_WINDOW = 0x08000000;
    const uint EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
    const uint STARTF_USESTDHANDLES = 0x100;
    const uint HANDLE_FLAG_INHERIT = 1;
    const uint INFINITE = 0xFFFFFFFF;
    const int PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x00020002;
    const int ERROR_ABANDONED_WAIT_0 = 735;
    const uint GENERIC_READ = 0x80000000;
    const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2;
    const uint OPEN_EXISTING = 3;

    [StructLayout(LayoutKind.Sequential)] struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)] struct JOBOBJECT_BASIC_LIMIT_INFORMATION { public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
    [StructLayout(LayoutKind.Sequential)] struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION { public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation; public IO_COUNTERS IoInfo; public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed; }
    [StructLayout(LayoutKind.Sequential)] struct JOBOBJECT_ASSOCIATE_COMPLETION_PORT { public IntPtr CompletionKey, CompletionPort; }
    [StructLayout(LayoutKind.Sequential)] struct JOBOBJECT_BASIC_ACCOUNTING_INFORMATION { public long TotalUserTime, TotalKernelTime, ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime; public uint TotalPageFaultCount, TotalProcesses, ActiveProcesses, TotalTerminatedProcesses; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct STARTUPINFO { public int cb; public string lpReserved, lpDesktop, lpTitle; public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
    [StructLayout(LayoutKind.Sequential)] struct STARTUPINFOEX { public STARTUPINFO StartupInfo; public IntPtr lpAttributeList; }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [StructLayout(LayoutKind.Sequential)] struct SECURITY_ATTRIBUTES { public int nLength; public IntPtr lpSecurityDescriptor; public int bInheritHandle; }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObjectW(IntPtr attrs, string name);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint len);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateJobObject(IntPtr job, uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint len, out uint ret);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateIoCompletionPort(IntPtr file, IntPtr existing, UIntPtr key, uint threads);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetQueuedCompletionStatus(IntPtr port, out uint bytes, out UIntPtr key, out IntPtr overlapped, uint ms);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CreatePipe(out IntPtr read, out IntPtr write, ref SECURITY_ATTRIBUTES sa, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetHandleInformation(IntPtr h, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateFileW(string name, uint access, uint share, ref SECURITY_ATTRIBUTES sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr prev, IntPtr ret);
    [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFOEX si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    static string QuoteArg(string s)
    {
        if (string.IsNullOrEmpty(s)) return "\"\"";
        if (s.IndexOfAny(new char[] { ' ', '\t', '"' }) < 0) return s;
        return "\"" + s.Replace("\"", "\\\"") + "\"";
    }

    static int[] QueryProcessIds(IntPtr job)
    {
        int max = 128;
        int size = 8 + max * IntPtr.Size;
        IntPtr p = Marshal.AllocHGlobal(size);
        try
        {
            uint ret;
            if (!QueryInformationJobObject(job, 3, p, (uint)size, out ret)) return new int[0];
            uint n = (uint)Marshal.ReadInt32(p, 4);
            if (n > (uint)max) n = (uint)max;
            int[] ids = new int[n];
            for (uint i = 0; i < n; i++) ids[i] = Marshal.ReadIntPtr(p, 8 + (int)i * IntPtr.Size).ToInt32();
            return ids;
        }
        finally { Marshal.FreeHGlobal(p); }
    }

    static uint QueryActive(IntPtr job)
    {
        int len = Marshal.SizeOf(typeof(JOBOBJECT_BASIC_ACCOUNTING_INFORMATION));
        IntPtr p = Marshal.AllocHGlobal(len);
        try
        {
            uint ret;
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, p, (uint)len, out ret)) return 0xFFFFFFFF;
            var acct = (JOBOBJECT_BASIC_ACCOUNTING_INFORMATION)Marshal.PtrToStructure(p, typeof(JOBOBJECT_BASIC_ACCOUNTING_INFORMATION));
            return acct.ActiveProcesses;
        }
        finally { Marshal.FreeHGlobal(p); }
    }

    public static Result Run(string exe, string[] args, string cwd, int timeoutMs, int jobEmptyWaitMs)
    {
        var r = new Result();
        var clock = System.Diagnostics.Stopwatch.StartNew();
        IntPtr job = IntPtr.Zero, iocp = IntPtr.Zero;
        IntPtr outR = IntPtr.Zero, outW = IntPtr.Zero, errR = IntPtr.Zero, errW = IntPtr.Zero, hIn = IntPtr.Zero;
        IntPtr attrList = IntPtr.Zero, handleList = IntPtr.Zero;
        PROCESS_INFORMATION pi = new PROCESS_INFORMATION();
        var outSb = new StringBuilder();
        var errSb = new StringBuilder();
        var rootExited = new ManualResetEventSlim(false);
        var jobEmpty = new ManualResetEventSlim(false);
        var portClosed = new ManualResetEventSlim(false);
        int msgNew = 0, msgExit = 0, msgZero = 0, msgTotal = 0;
        try
        {
            job = CreateJobObjectW(IntPtr.Zero, null);
            if (job == IntPtr.Zero) throw new Exception("CreateJobObject failed err=" + Marshal.GetLastWin32Error());
            var ext = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            int extLen = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
            IntPtr extPtr = Marshal.AllocHGlobal(extLen);
            try
            {
                Marshal.StructureToPtr(ext, extPtr, false);
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, extPtr, (uint)extLen))
                    throw new Exception("SetInformationJobObject(KILL_ON_JOB_CLOSE) failed err=" + Marshal.GetLastWin32Error());
            }
            finally { Marshal.FreeHGlobal(extPtr); }

            iocp = CreateIoCompletionPort(new IntPtr(-1), IntPtr.Zero, UIntPtr.Zero, 1);
            if (iocp == IntPtr.Zero) throw new Exception("CreateIoCompletionPort failed err=" + Marshal.GetLastWin32Error());
            var assoc = new JOBOBJECT_ASSOCIATE_COMPLETION_PORT();
            assoc.CompletionKey = IntPtr.Zero; assoc.CompletionPort = iocp;
            int assocLen = Marshal.SizeOf(typeof(JOBOBJECT_ASSOCIATE_COMPLETION_PORT));
            IntPtr assocPtr = Marshal.AllocHGlobal(assocLen);
            try
            {
                Marshal.StructureToPtr(assoc, assocPtr, false);
                if (!SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, assocPtr, (uint)assocLen))
                    throw new Exception("associate completion port failed err=" + Marshal.GetLastWin32Error());
            }
            finally { Marshal.FreeHGlobal(assocPtr); }

            IntPtr capturedPort = iocp;
            Thread portThread = new Thread(delegate()
            {
                while (true)
                {
                    uint bytes; UIntPtr key; IntPtr ov;
                    bool ok = GetQueuedCompletionStatus(capturedPort, out bytes, out key, out ov, 500);
                    if (!ok)
                    {
                        int err = Marshal.GetLastWin32Error();
                        if (err == ERROR_ABANDONED_WAIT_0 || portClosed.IsSet) break;
                        continue;
                    }
                    Interlocked.Increment(ref msgTotal);
                    if (bytes == 6) Interlocked.Increment(ref msgNew);
                    else if (bytes == 7) { Interlocked.Increment(ref msgExit); if (QueryActive(job) == 0) jobEmpty.Set(); }
                    else if (bytes == 4) { Interlocked.Increment(ref msgZero); jobEmpty.Set(); }
                }
            });
            portThread.IsBackground = true;
            portThread.Start();

            var sa = new SECURITY_ATTRIBUTES();
            sa.nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES));
            sa.lpSecurityDescriptor = IntPtr.Zero;
            sa.bInheritHandle = 1;
            if (!CreatePipe(out outR, out outW, ref sa, 0)) throw new Exception("CreatePipe(stdout) failed err=" + Marshal.GetLastWin32Error());
            if (!SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0)) throw new Exception("SetHandleInformation(stdout) failed");
            if (!CreatePipe(out errR, out errW, ref sa, 0)) throw new Exception("CreatePipe(stderr) failed err=" + Marshal.GetLastWin32Error());
            if (!SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0)) throw new Exception("SetHandleInformation(stderr) failed");
            hIn = CreateFileW("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, ref sa, OPEN_EXISTING, 0, IntPtr.Zero);
            if (hIn == new IntPtr(-1)) throw new Exception("CreateFile(NUL) failed err=" + Marshal.GetLastWin32Error());

            IntPtr size = IntPtr.Zero;
            InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
            attrList = Marshal.AllocHGlobal(size);
            if (!InitializeProcThreadAttributeList(attrList, 1, 0, ref size)) throw new Exception("InitializeProcThreadAttributeList failed err=" + Marshal.GetLastWin32Error());
            handleList = Marshal.AllocHGlobal(IntPtr.Size * 3);
            Marshal.WriteIntPtr(handleList, 0, outW);
            Marshal.WriteIntPtr(handleList, IntPtr.Size, errW);
            Marshal.WriteIntPtr(handleList, IntPtr.Size * 2, hIn);
            if (!UpdateProcThreadAttribute(attrList, 0, (IntPtr)PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handleList, (IntPtr)(IntPtr.Size * 3), IntPtr.Zero, IntPtr.Zero))
                throw new Exception("UpdateProcThreadAttribute(HANDLE_LIST) failed err=" + Marshal.GetLastWin32Error());

            var si = new STARTUPINFOEX();
            si.StartupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFOEX));
            si.StartupInfo.dwFlags = (int)STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = hIn;
            si.StartupInfo.hStdOutput = outW;
            si.StartupInfo.hStdError = errW;
            si.lpAttributeList = attrList;

            var cmdLine = new StringBuilder();
            cmdLine.Append(QuoteArg(exe));
            for (int i = 0; i < args.Length; i++) { cmdLine.Append(' '); cmdLine.Append(QuoteArg(args[i])); }

            bool created = CreateProcessW(exe, cmdLine, IntPtr.Zero, IntPtr.Zero, true, CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, IntPtr.Zero, cwd, ref si, out pi);
            if (!created) throw new Exception("CreateProcessW failed err=" + Marshal.GetLastWin32Error());
            r.Created = true; r.RootPid = pi.dwProcessId;

            CloseHandle(outW); outW = IntPtr.Zero;
            CloseHandle(errW); errW = IntPtr.Zero;
            CloseHandle(hIn); hIn = IntPtr.Zero;

            var fsOut = new FileStream(new SafeFileHandle(outR, true), FileAccess.Read, 8192, false); outR = IntPtr.Zero;
            var fsErr = new FileStream(new SafeFileHandle(errR, true), FileAccess.Read, 8192, false); errR = IntPtr.Zero;
            Thread to = new Thread(delegate() { try { byte[] b = new byte[8192]; int n; while ((n = fsOut.Read(b, 0, b.Length)) > 0) { lock (outSb) outSb.Append(Encoding.UTF8.GetString(b, 0, n)); } } catch { } });
            Thread te = new Thread(delegate() { try { byte[] b = new byte[8192]; int n; while ((n = fsErr.Read(b, 0, b.Length)) > 0) { lock (errSb) errSb.Append(Encoding.UTF8.GetString(b, 0, n)); } } catch { } });
            to.IsBackground = true; te.IsBackground = true;
            to.Start(); te.Start();

            IntPtr rootHandle = pi.hProcess;
            Thread tw = new Thread(delegate() { WaitForSingleObject(rootHandle, INFINITE); rootExited.Set(); });
            tw.IsBackground = true; tw.Start();

            if (!AssignProcessToJobObject(job, pi.hProcess))
            {
                int err = Marshal.GetLastWin32Error();
                TerminateProcess(pi.hProcess, 1);       // never run the child unsupervised
                throw new Exception("AssignProcessToJobObject failed err=" + err);
            }
            r.Assigned = true;

            uint resumed = ResumeThread(pi.hThread);
            if (resumed == 0xFFFFFFFF) throw new Exception("ResumeThread failed err=" + Marshal.GetLastWin32Error());
            r.Resumed = true;

            bool exited = rootExited.Wait(timeoutMs);
            if (!exited)
            {
                r.TimedOut = true;
                TerminateJobObject(job, 0xE0);
                r.TimedOutTerminated = true;
                exited = rootExited.Wait(10000);
            }
            uint code = 0xFFFFFFFF;
            GetExitCodeProcess(pi.hProcess, out code);
            r.RootExitCode = (int)code;

            uint active = QueryActive(job);
            r.ActiveAtRootExit = active;
            r.PidsAtRootExit = QueryProcessIds(job);
            if (active != 0)
            {
                r.DescendantsTerminated = true;
                TerminateJobObject(job, 0xE0);
            }
            // The job handle itself signals when the active process count reaches zero; the completion port
            // supplies ACTIVE_PROCESS_ZERO message evidence for the same state.
            bool zeroSeen = jobEmpty.Wait(jobEmptyWaitMs);
            uint waited = WaitForSingleObject(job, 0);
            uint activeAfter = QueryActive(job);
            r.ActiveAfterCleanup = activeAfter;
            r.JobEmptyConfirmed = zeroSeen || (waited == 0) || (activeAfter == 0);

            to.Join(5000); te.Join(5000);
            r.Stdout = outSb.ToString();
            r.Stderr = errSb.ToString();
            r.MsgNewProcess = msgNew; r.MsgExitProcess = msgExit; r.MsgActiveZero = msgZero; r.MsgTotal = msgTotal;
        }
        catch (Exception ex)
        {
            r.Error = ex.Message;
            r.MsgNewProcess = msgNew; r.MsgExitProcess = msgExit; r.MsgActiveZero = msgZero; r.MsgTotal = msgTotal;
            r.Stdout = outSb.ToString();
            r.Stderr = errSb.ToString();
        }
        finally
        {
            try { portClosed.Set(); } catch { }
            if (iocp != IntPtr.Zero) { CloseHandle(iocp); iocp = IntPtr.Zero; }
            if (pi.hProcess != IntPtr.Zero) CloseHandle(pi.hProcess);
            if (pi.hThread != IntPtr.Zero) CloseHandle(pi.hThread);
            if (outW != IntPtr.Zero) CloseHandle(outW);
            if (errW != IntPtr.Zero) CloseHandle(errW);
            if (outR != IntPtr.Zero) CloseHandle(outR);
            if (errR != IntPtr.Zero) CloseHandle(errR);
            if (hIn != IntPtr.Zero) CloseHandle(hIn);
            if (attrList != IntPtr.Zero) DeleteProcThreadAttributeList(attrList);
            if (attrList != IntPtr.Zero) Marshal.FreeHGlobal(attrList);
            if (handleList != IntPtr.Zero) Marshal.FreeHGlobal(handleList);
            if (job != IntPtr.Zero) CloseHandle(job);   // KILL_ON_JOB_CLOSE is the last-resort containment
        }
        clock.Stop();
        r.DurationMs = clock.ElapsedMilliseconds;
        return r;
    }
}
'@ -Language CSharp

function Save-Json {
  param([string]$Path, $Object)
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
  [System.IO.File]::WriteAllText($Path, ($Object | ConvertTo-Json -Depth 12), (New-Object System.Text.UTF8Encoding($false)))
}

function Resolve-RepoPath {
  param([string]$Value, [string]$RepoRoot)
  if ([string]::IsNullOrWhiteSpace($Value)) { throw 'empty path in a source manifest' }
  if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
  return [IO.Path]::GetFullPath((Join-Path $RepoRoot $Value))
}

function Read-SourceManifest {
  param([string]$ManifestPath, [string]$RepoRoot)
  if (!(Test-Path -LiteralPath $ManifestPath -PathType Leaf)) { throw "missing source manifest: $ManifestPath" }
  $defines = New-Object System.Collections.ArrayList
  $includes = New-Object System.Collections.ArrayList
  $tus = New-Object System.Collections.ArrayList
  $evidence = New-Object System.Collections.ArrayList
  $lineNo = 0
  foreach ($raw in [System.IO.File]::ReadAllLines($ManifestPath)) {
    $lineNo++
    $line = $raw.Trim()
    if ($line.Length -eq 0 -or $line.StartsWith('#')) { continue }
    if ($line.StartsWith('@define ')) { [void]$defines.Add($line.Substring(8).Trim()); continue }
    if ($line.StartsWith('@include ')) { [void]$includes.Add((Resolve-RepoPath -Value $line.Substring(9).Trim() -RepoRoot $RepoRoot)); continue }
    if ($line.StartsWith('@evidence ')) { [void]$evidence.Add((Resolve-RepoPath -Value $line.Substring(10).Trim() -RepoRoot $RepoRoot)); continue }
    if ($line.StartsWith('@')) { throw "unknown manifest directive at line $lineNo of ${ManifestPath}: $line" }
    [void]$tus.Add((Resolve-RepoPath -Value $line -RepoRoot $RepoRoot))
  }
  if ($tus.Count -eq 0) { throw "source manifest lists no translation units: $ManifestPath" }
  $seen = @{}
  foreach ($tu in $tus) {
    $key = $tu.ToUpperInvariant()
    if ($seen.ContainsKey($key)) { throw "duplicate TU entry in ${ManifestPath}: $tu" }
    $seen[$key] = $true
  }
  foreach ($tu in $tus) { if (!(Test-Path -LiteralPath $tu -PathType Leaf)) { throw "missing translation unit: $tu" } }
  foreach ($inc in $includes) { if (!(Test-Path -LiteralPath $inc -PathType Container)) { throw "missing include directory: $inc" } }
  foreach ($ev in $evidence) { if (!(Test-Path -LiteralPath $ev -PathType Leaf)) { throw "missing evidence input: $ev" } }
  return [ordered]@{ path = $ManifestPath; defines = @($defines); includes = @($includes); tus = @($tus); evidence = @($evidence) }
}

function Get-IncludedTranslationUnits {
  param([string]$FixturePath)
  $dir = Split-Path -Parent $FixturePath
  $found = New-Object System.Collections.ArrayList
  foreach ($raw in [System.IO.File]::ReadAllLines($FixturePath)) {
    if ($raw -match '^\s*#\s*include\s+"([^"]+\.cpp)"') {
      $candidate = [IO.Path]::GetFullPath((Join-Path $dir $Matches[1]))
      if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and -not $found.Contains($candidate)) { [void]$found.Add($candidate) }
    }
  }
  return @($found)
}

function Resolve-Toolchain {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw "MSVC vswhere unavailable: $vswhere" }
  $vs = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)[0]
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vs)) { throw 'MSVC x64 build tools unavailable (vswhere found no toolset)' }
  $vcvars = Join-Path $vs.Trim() 'VC\Auxiliary\Build\vcvars64.bat'
  if (!(Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw "MSVC x64 environment unavailable: $vcvars" }
  $cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe'
  if (!(Test-Path -LiteralPath $cmdExe -PathType Leaf)) { throw "command interpreter unavailable: $cmdExe" }
  return [ordered]@{ vswhere = $vswhere; vsInstall = $vs.Trim(); vcvars64 = $vcvars; cmdExe = $cmdExe }
}

function New-CompileStage {
  param($Ctx)
  # Every path in the generated batch file goes through an inherited environment variable: a batch file is read
  # by cmd.exe in the console code page, so a non-ASCII evidence path would be mangled inside it. The command
  # line itself stays Unicode, and the receipt records both the %VAR% form and the resolved form.
  $rel = { param($p) $p.Substring($Ctx.repoRoot.Length).TrimStart('\', '/') }
  $includeArgs = (($Ctx.includes | ForEach-Object { '/I"%OB_SRC_ROOT%\' + (& $rel $_) + '"' }) -join ' ')
  $defineArgs = (($Ctx.defines | ForEach-Object { '/D' + $_ }) -join ' ')
  $cppArgs = (($Ctx.cppSources | ForEach-Object { '"%OB_SRC_ROOT%\' + (& $rel $_) + '"' }) -join ' ')
  $prefix = ('cl /nologo /std:c++17 /EHsc /utf-8 ' + $Ctx.compilerFlags + ' ' + $defineArgs + ' ' + $includeArgs).Trim() + ' '
  if ($Ctx.cSources.Count -gt 0) {
    $cArgs = (($Ctx.cSources | ForEach-Object { '"%OB_SRC_ROOT%\' + (& $rel $_) + '"' }) -join ' ')
    $objArgs = (($Ctx.cSources | ForEach-Object { '"%OB_OBJ%\' + [IO.Path]::GetFileNameWithoutExtension($_) + '.obj"' }) -join ' ')
    $cLine = 'cl /nologo /c /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /Fo:"%OB_OBJ%\\" ' + $cArgs
    $cppLine = $prefix + $cppArgs + ' ' + $objArgs + ' /Fo:"%OB_OUT%\\" /Fd:"%OB_OUT%\\" /Fe:"%OB_EXE%"'
    $text = 'call "%OB_VCVARS%" >nul && ' + $cLine + ' && ' + $cppLine
  } else {
    $text = 'call "%OB_VCVARS%" >nul && ' + $prefix + $cppArgs + ' /Fo:"%OB_OUT%\\" /Fd:"%OB_OUT%\\" /Fe:"%OB_EXE%"'
  }
  $envMap = [ordered]@{ OB_SRC_ROOT = $Ctx.repoRoot; OB_VCVARS = $Ctx.vcvars; OB_OBJ = $Ctx.objDir; OB_OUT = $Ctx.outDir; OB_EXE = $Ctx.exePath }
  foreach ($k in $envMap.Keys) {
    if ([string]::IsNullOrEmpty([string]$envMap[$k])) { throw "contract: environment value $k must not be empty" }
    if ([string]$envMap[$k] -match '[%]') { throw "contract: environment value $k must not contain a percent sign" }
  }
  if ($text -match '[^\x00-\x7F]') { throw 'contract: the generated compile batch must stay ASCII' }
  $resolved = $text
  foreach ($k in $envMap.Keys) { $resolved = $resolved.Replace('%' + $k + '%', [string]$envMap[$k]) }
  $commandPath = Join-Path $Ctx.outDir ('compile-' + $Ctx.variant + '.cmd')
  [System.IO.File]::WriteAllText($commandPath, ($text + "`r`n"), (New-Object System.Text.ASCIIEncoding))
  return [ordered]@{
    exe = $Ctx.cmdExe; args = @('/d', '/c', $commandPath); commandText = $text; resolvedCommandText = $resolved
    commandPath = $commandPath; commandSha256 = (Get-FileHash -LiteralPath $commandPath -Algorithm SHA256).Hash
    commandEncoding = 'us-ascii'; env = $envMap
  }
}

function New-TestStage {
  param($Ctx)
  if ($Ctx.smoke) {
    $args = @('--result', $Ctx.smokeResultPath, '--log', $Ctx.smokeLogPath, '--run-id', $Ctx.runId)
    return [ordered]@{ exe = $Ctx.exePath; args = $args; commandText = ('"' + $Ctx.exePath + '" ' + (($args | ForEach-Object { '"' + $_ + '"' }) -join ' ')); env = $null }
  }
  $args = @($Ctx.fixtureDir)
  return [ordered]@{ exe = $Ctx.exePath; args = $args; commandText = ('"' + $Ctx.exePath + '" "' + $Ctx.fixtureDir + '"'); env = $null }
}

function Invoke-Stage {
  param($Ctx, [string]$StageName, $StageSpec, [int]$TimeoutSeconds, [int]$Index)
  $outPath = Join-Path $Ctx.stageDir ('{0}-{1}.out.txt' -f $Index, $StageName)
  $errPath = Join-Path $Ctx.stageDir ('{0}-{1}.err.txt' -f $Index, $StageName)
  $saved = @{}
  if ($StageSpec.env) {
    foreach ($k in $StageSpec.env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process'); [Environment]::SetEnvironmentVariable($k, [string]$StageSpec.env[$k], 'Process') }
  }
  try {
    $res = [OverlayBindingStage]::Run($StageSpec.exe, [string[]]$StageSpec.args, $Ctx.cwd, ($TimeoutSeconds * 1000), $JOB_EMPTY_WAIT_MS)
  } finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
  }
  New-Item -ItemType Directory -Force -Path $Ctx.stageDir | Out-Null
  [System.IO.File]::WriteAllText($outPath, [string]$res.Stdout, (New-Object System.Text.UTF8Encoding($false)))
  [System.IO.File]::WriteAllText($errPath, [string]$res.Stderr, (New-Object System.Text.UTF8Encoding($false)))
  $stage = [ordered]@{
    variant = $Ctx.variant; stage = $StageName; index = $Index
    executable = $StageSpec.exe; args = @($StageSpec.args); cwd = $Ctx.cwd
    commandText = $StageSpec.commandText; resolvedCommandText = $StageSpec.resolvedCommandText
    commandPath = $StageSpec.commandPath; commandSha256 = $StageSpec.commandSha256; commandEncoding = $StageSpec.commandEncoding
    env = $StageSpec.env
    timeoutSeconds = $TimeoutSeconds; timeoutAuthority = 'this runner'
    pid = $res.RootPid; exitCode = $res.RootExitCode; timedOut = $res.TimedOut
    terminatedOnTimeout = $res.TimedOutTerminated; descendantsTerminated = $res.DescendantsTerminated
    processCreated = $res.Created; processAssigned = $res.Assigned; processResumed = $res.Resumed
    jobEmptyConfirmed = $res.JobEmptyConfirmed; jobEmptyWaitMs = $JOB_EMPTY_WAIT_MS
    activeAtRootExit = [int]$res.ActiveAtRootExit; activeAfterCleanup = [int]$res.ActiveAfterCleanup
    pidsAtRootExit = @($res.PidsAtRootExit); durationMs = $res.DurationMs
    stdoutPath = $outPath; stderrPath = $errPath; error = $res.Error
  }
  return [ordered]@{ stage = $stage; result = $res }
}

function Invoke-Variant {
  param($SuiteRow, $Variant, [string]$FixturePath, [string]$EvidenceDir, $Toolchain, [string]$RepoRoot)
  $SuiteName = $SuiteRow.name
  $vr = [ordered]@{
    variant = $Variant.name; suite = $SuiteName; define = $Variant.define; manifest = $null; manifestSha256 = $null
    fixture = $FixturePath; fixtureSha256 = $null; status = 'FAIL'; error = ''; assertions = 0
    exePath = $null; binarySha256 = $null; binaryBytes = $null; sourceHashes = @(); compileArgv = @()
    smokeReceipt = $null; hostLog = $null; stages = @()
  }
  $variantRoot = Join-Path $EvidenceDir (Join-Path 'variants' $Variant.name)
  try {
    $outDir = Join-Path $EvidenceDir (Join-Path 'out' $Variant.name)
    $objDir = Join-Path $outDir 'c-obj'
    $fixtureDir = Join-Path $EvidenceDir (Join-Path 'fixtures' $Variant.name)
    $stageDir = Join-Path $EvidenceDir (Join-Path 'stages' $Variant.name)
    foreach ($d in @($outDir, $objDir, $fixtureDir, $stageDir, $variantRoot)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }

    $manifestPath = Join-Path (Split-Path -Parent $FixturePath) $Variant.manifest
    $manifest = Read-SourceManifest -ManifestPath $manifestPath -RepoRoot $RepoRoot
    $vr.manifest = $manifest.path
    $vr.manifestSha256 = (Get-FileHash -LiteralPath $manifest.path -Algorithm SHA256).Hash

    $included = @(Get-IncludedTranslationUnits -FixturePath $FixturePath)
    foreach ($inc in $included) {
      foreach ($tu in $manifest.tus) {
        if ($tu.Equals($inc, 'OrdinalIgnoreCase')) { throw "the fixture textually includes $inc, so it must not be compiled as its own translation unit" }
      }
    }

    $vr.fixtureSha256 = (Get-FileHash -LiteralPath $FixturePath -Algorithm SHA256).Hash
    $compileSources = @($FixturePath) + @($manifest.tus)
    $vr.compileArgv = @($compileSources)
    $cSources = @($compileSources | Where-Object { [IO.Path]::GetExtension($_).Equals('.c', 'OrdinalIgnoreCase') })
    $cppSources = @($compileSources | Where-Object { !($cSources -contains $_) })
    $hashList = New-Object System.Collections.ArrayList
    [void]$hashList.Add([ordered]@{ path = $FixturePath; sha256 = $vr.fixtureSha256; role = 'fixture' })
    foreach ($tu in $manifest.tus) { [void]$hashList.Add([ordered]@{ path = $tu; sha256 = (Get-FileHash -LiteralPath $tu -Algorithm SHA256).Hash; role = 'tu' }) }
    foreach ($inc in $included) { [void]$hashList.Add([ordered]@{ path = $inc; sha256 = (Get-FileHash -LiteralPath $inc -Algorithm SHA256).Hash; role = 'included' }) }
    foreach ($ev in $manifest.evidence) { [void]$hashList.Add([ordered]@{ path = $ev; sha256 = (Get-FileHash -LiteralPath $ev -Algorithm SHA256).Hash; role = 'evidence' }) }
    $vr.sourceHashes = @($hashList)
    $vr.defines = @($manifest.defines)

    $exeName = $Variant.exe
    $exePath = Join-Path $outDir $exeName
    $vr.exePath = $exePath
    # The compiler flag set is part of the registry row: a variant that includes the production core TU is compiled
    # at the production warning level, every other variant keeps the fixture contract's '/W4 /WX'.
    $compilerFlags = if ([string]::IsNullOrWhiteSpace([string]$Variant.cflags)) { '/W4 /WX' } else { [string]$Variant.cflags }
    $vr.compilerFlags = $compilerFlags
    $compileCtx = [ordered]@{
      variant = $Variant.name; cmdExe = $Toolchain.cmdExe; vcvars = $Toolchain.vcvars64; repoRoot = $RepoRoot
      includes = @($manifest.includes); defines = @($manifest.defines); cSources = $cSources; cppSources = $cppSources
      objDir = $objDir; outDir = $outDir; exePath = $exePath; compilerFlags = $compilerFlags
    }
    $testCtx = [ordered]@{
      variant = $Variant.name; smoke = $Variant.smoke; exePath = $exePath; fixtureDir = $fixtureDir
      smokeResultPath = Join-Path $variantRoot 'smoke.json'; smokeLogPath = Join-Path $variantRoot 'smoke.log'
      runId = (($SuiteName + '-' + $Variant.name + '-smoke'))
    }

    $compile = Invoke-Stage -Ctx ([ordered]@{ variant = $Variant.name; cwd = $RepoRoot; stageDir = $stageDir }) -StageName 'compile' -StageSpec (New-CompileStage -Ctx $compileCtx) -TimeoutSeconds $CompileTimeoutSeconds -Index 1
    $vr.stages += $compile.stage
    if (-not $compile.result.Created) { throw "compile stage did not start a child process: $($compile.result.Error)" }
    if ($compile.result.TimedOut) { throw "compile stage timed out after $CompileTimeoutSeconds s (pid $($compile.result.RootPid)); the job was terminated" }
    if ($compile.result.Error) { throw "compile stage error: $($compile.result.Error)" }
    if (-not $compile.result.JobEmptyConfirmed) { throw "compile stage left live descendants (activeAfterCleanup=$($compile.result.ActiveAfterCleanup))" }
    if ($compile.result.RootExitCode -ne 0) { throw "compile failed (exit $($compile.result.RootExitCode))" }
    if (!(Test-Path -LiteralPath $exePath -PathType Leaf)) { throw "compile produced no executable: $exePath" }

    # ---- test stage(s). A variant with a `cases` list compiles once and launches one fresh Job-supervised
    # process per case (the fixture directory, then `--case` and the case name): static function-local state such
    # as the production ScanImage cache therefore never crosses a case. Each case must produce exactly one
    # positive ASSERTIONS line, exit 0, leave an empty job and write its own cases/<case>.json machine receipt;
    # the raw streams and the receipt are copied into variants/<variant>/cases/<case>/.
    $caseNames = @()
    if ($null -ne $Variant.cases) { $caseNames = @($Variant.cases) }
    $perCase = @()
    $assertions = @()
    if ($caseNames.Count -gt 0) {
      $caseRoot = Join-Path $variantRoot 'cases'
      $index = 2
      foreach ($caseName in $caseNames) {
        if ([string]::IsNullOrWhiteSpace($caseName)) { throw 'contract: a case name must not be empty' }
        $caseDir = Join-Path $caseRoot $caseName
        New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
        $caseArgs = @($fixtureDir, '--case', $caseName)
        $caseSpec = [ordered]@{
          exe = $exePath; args = $caseArgs
          commandText = ('"' + $exePath + '" "' + $fixtureDir + '" --case ' + $caseName)
          env = $null
        }
        $caseStage = Invoke-Stage -Ctx ([ordered]@{ variant = $Variant.name; cwd = $RepoRoot; stageDir = $stageDir }) -StageName ('case-' + $caseName) -StageSpec $caseSpec -TimeoutSeconds $TestTimeoutSeconds -Index $index
        $index++
        $vr.stages += $caseStage.stage
        $caseAssertions = @()
        foreach ($line in (@([string]$caseStage.result.Stdout -split "`r?`n") + @([string]$caseStage.result.Stderr -split "`r?`n"))) {
          if ($line -match '^ASSERTIONS=([1-9][0-9]*)$') { $caseAssertions += [int]$Matches[1] }
        }
        $caseJsonPath = Join-Path $fixtureDir (Join-Path 'cases' ($caseName + '.json'))
        [System.IO.File]::WriteAllText((Join-Path $caseDir 'stdout.log'), [string]$caseStage.result.Stdout, (New-Object System.Text.UTF8Encoding($false)))
        [System.IO.File]::WriteAllText((Join-Path $caseDir 'stderr.log'), [string]$caseStage.result.Stderr, (New-Object System.Text.UTF8Encoding($false)))
        $caseJson = $null
        if (Test-Path -LiteralPath $caseJsonPath -PathType Leaf) {
          try { $caseJson = Get-Content -LiteralPath $caseJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $caseJson = $null }
          Copy-Item -LiteralPath $caseJsonPath -Destination (Join-Path $caseDir 'cases.json') -Force
        }
        $caseProblem = ''
        if (-not $caseStage.result.Created) { $caseProblem = "case '$caseName' stage did not start a child process: $($caseStage.result.Error)" }
        elseif ($caseStage.result.TimedOut) { $caseProblem = "case '$caseName' timed out after $TestTimeoutSeconds s (pid $($caseStage.result.RootPid))" }
        elseif ($caseStage.result.Error) { $caseProblem = "case '$caseName' stage error: $($caseStage.result.Error)" }
        elseif (-not $caseStage.result.JobEmptyConfirmed) { $caseProblem = "case '$caseName' left live descendants (activeAfterCleanup=$($caseStage.result.ActiveAfterCleanup))" }
        elseif ($caseStage.result.RootExitCode -ne 0) { $caseProblem = "case '$caseName' failed (exit $($caseStage.result.RootExitCode))" }
        elseif ($caseAssertions.Count -ne 1) { $caseProblem = "case '$caseName' returned $($caseAssertions.Count) ASSERTIONS=positive-integer lines; exactly one is required" }
        elseif ($null -eq $caseJson) { $caseProblem = "case '$caseName' wrote no machine receipt: $caseJsonPath" }
        elseif ($caseJson.status -ne 'PASS') { $caseProblem = "case '$caseName' machine receipt status '$($caseJson.status)' != PASS" }
        elseif ([int]$caseJson.assertions -ne $caseAssertions[0]) { $caseProblem = "case '$caseName' machine receipt assertions $($caseJson.assertions) != $($caseAssertions[0])" }
        $caseReceipt = [ordered]@{
          name = $caseName; status = $(if ($caseProblem -eq '') { 'PASS' } else { 'FAIL' }); error = $caseProblem
          assertions = $(if ($caseAssertions.Count -eq 1) { $caseAssertions[0] } else { 0 })
          pid = $caseStage.stage.pid; exitCode = $caseStage.stage.exitCode; timedOut = $caseStage.stage.timedOut
          jobEmptyConfirmed = $caseStage.stage.jobEmptyConfirmed
          stdoutPath = Join-Path $caseDir 'stdout.log'; stderrPath = Join-Path $caseDir 'stderr.log'
          machineReceiptPath = $(if (Test-Path -LiteralPath (Join-Path $caseDir 'cases.json')) { Join-Path $caseDir 'cases.json' } else { $null })
          stageIndex = $caseStage.stage.index; durationMs = $caseStage.stage.durationMs
        }
        Save-Json -Path (Join-Path $caseDir 'result.json') -Object $caseReceipt
        $perCase += $caseReceipt
        # The variant receipt keeps every case that actually ran, even when a later one fails and the runner stops.
        $vr.cases = @($perCase)
        $vr.assertions = [int](($assertions | Measure-Object -Sum).Sum)
        if ($caseProblem -ne '') { throw $caseProblem }
        $assertions += $caseAssertions[0]
      }
      $vr.cases = @($perCase)
    } else {
      $test = Invoke-Stage -Ctx ([ordered]@{ variant = $Variant.name; cwd = $RepoRoot; stageDir = $stageDir }) -StageName 'test' -StageSpec (New-TestStage -Ctx $testCtx) -TimeoutSeconds $TestTimeoutSeconds -Index 2
      $vr.stages += $test.stage
      if (-not $test.result.Created) { throw "test stage did not start a child process: $($test.result.Error)" }
      if ($test.result.TimedOut) { throw "test stage timed out after $TestTimeoutSeconds s (pid $($test.result.RootPid)); the job was terminated" }
      if ($test.result.Error) { throw "test stage error: $($test.result.Error)" }
      if (-not $test.result.JobEmptyConfirmed) { throw "test stage left live descendants (activeAfterCleanup=$($test.result.ActiveAfterCleanup))" }
      if ($test.result.RootExitCode -ne 0) { throw "suite $SuiteName ($($Variant.name)) failed (exit $($test.result.RootExitCode))" }
      foreach ($line in (@([string]$test.result.Stdout -split "`r?`n") + @([string]$test.result.Stderr -split "`r?`n"))) {
        if ($line -match '^ASSERTIONS=([1-9][0-9]*)$') { $assertions += [int]$Matches[1] }
      }
      if ($assertions.Count -ne 1) { throw "suite $SuiteName ($($Variant.name)) returned $($assertions.Count) ASSERTIONS=positive-integer lines; exactly one is required" }
    }

    $vr.binarySha256 = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    $vr.binaryBytes = (Get-Item -LiteralPath $exePath).Length
    if ($Variant.smoke) {
      if (!(Test-Path -LiteralPath $testCtx.smokeResultPath -PathType Leaf)) { throw "native smoke produced no receipt: $($testCtx.smokeResultPath)" }
      $smoke = Get-Content -LiteralPath $testCtx.smokeResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
      if ($smoke.suite -ne $SuiteName) { throw "native smoke receipt suite '$($smoke.suite)' != $SuiteName" }
      if ($smoke.mode -ne 'NativeMinHookSmoke') { throw "native smoke receipt mode '$($smoke.mode)' != NativeMinHookSmoke" }
      if ($smoke.runId -ne $testCtx.runId) { throw "native smoke receipt runId '$($smoke.runId)' != $($testCtx.runId)" }
      if ($smoke.status -ne 'PASS') { throw "native smoke receipt status '$($smoke.status)' != PASS" }
      if ([int]$smoke.assertions -ne $assertions[0]) { throw "native smoke receipt assertions $($smoke.assertions) != $($assertions[0])" }
      $vr.smokeReceipt = [ordered]@{ path = $testCtx.smokeResultPath; mode = $smoke.mode; runId = $smoke.runId; assertions = [int]$smoke.assertions; legacy = $smoke.legacy; realRegions = @($smoke.realRegions); substitutedGpuRegions = @($smoke.substitutedGpuRegions) }
    }
    $hostLogPath = if ($Variant.smoke) { $testCtx.smokeLogPath } else { Join-Path $fixtureDir ($SuiteRow.stem + '.log') }
    if (!(Test-Path -LiteralPath $hostLogPath -PathType Leaf) -or (Get-Item -LiteralPath $hostLogPath).Length -eq 0) { throw "the fixture wrote no run log: $hostLogPath" }
    $vr.hostLog = [ordered]@{ path = $hostLogPath; sha256 = (Get-FileHash -LiteralPath $hostLogPath -Algorithm SHA256).Hash; bytes = (Get-Item -LiteralPath $hostLogPath).Length }

    $vr.assertions = if ($perCase.Count -gt 0) { [int](($assertions | Measure-Object -Sum).Sum) } else { $assertions[0] }
    if ($perCase.Count -gt 0 -and $vr.assertions -le 0) { throw "suite $SuiteName ($($Variant.name)) produced no positive per-case assertion count" }
    $vr.status = 'PASS'

  } catch {
    $vr.error = $_.Exception.Message
  } finally {
    Save-Json -Path (Join-Path $variantRoot 'result.json') -Object $vr
  }
  return $vr
}

function Test-SuiteManifests {
  # Preflight one suite's every variant manifest BEFORE any compiler runs. Read-SourceManifest already rejects a
  # missing manifest/TU/include/evidence input, an unknown directive and a TU named twice in one manifest; this
  # walks ALL of the suite's manifests (not just the first) and rejects a fixture-textually-included TU listed as
  # its own translation unit in any of them.
  param($SuiteRow, [string]$FixturePath, [string]$RepoRoot)
  if (!(Test-Path -LiteralPath $FixturePath -PathType Leaf)) { throw "missing suite fixture: $FixturePath" }
  $fixtureDir = Split-Path -Parent $FixturePath
  $included = @(Get-IncludedTranslationUnits -FixturePath $FixturePath)
  $checks = New-Object System.Collections.ArrayList
  foreach ($variant in $SuiteRow.variants) {
    $manifestPath = Join-Path $fixtureDir $variant.manifest
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "missing source manifest: $manifestPath" }
    $manifest = Read-SourceManifest -ManifestPath $manifestPath -RepoRoot $RepoRoot
    foreach ($inc in $included) {
      foreach ($tu in $manifest.tus) {
        if ($tu.Equals($inc, 'OrdinalIgnoreCase')) { throw "the fixture textually includes $inc, so it must not be compiled as its own translation unit (manifest $($variant.manifest))" }
      }
    }
    [void]$checks.Add([ordered]@{
      suite = $SuiteRow.name; variant = $variant.name; manifest = $manifest.path
      manifestSha256 = (Get-FileHash -LiteralPath $manifest.path -Algorithm SHA256).Hash
      tuCount = $manifest.tus.Count; evidenceCount = $manifest.evidence.Count; includeCount = $manifest.includes.Count
      fixtureIncludedTuCount = $included.Count; duplicateTuEntriesInManifest = 0; status = 'PASS'
    })
  }
  return @($checks)
}

function Get-DuplicateTuSummary {
  # Machine-visible duplicate-TU summary across the selected suites' manifests. A TU shared by two VARIANT
  # manifests is expected (each variant compiles its own set) and recorded; a TU named twice inside one manifest
  # is fatal in Read-SourceManifest. The walk covers every manifest, not just the top fixture.
  param($SelectedSuites, [string]$FixtureDir, [string]$RepoRoot)
  $byManifest = [ordered]@{}
  foreach ($row in $SelectedSuites) {
    foreach ($variant in $row.variants) {
      $manifestPath = Join-Path $FixtureDir $variant.manifest
      if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { continue }
      $tus = New-Object System.Collections.ArrayList
      foreach ($raw in [System.IO.File]::ReadAllLines($manifestPath)) {
        $line = $raw.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith('#') -or $line.StartsWith('@')) { continue }
        [void]$tus.Add((Resolve-RepoPath -Value $line -RepoRoot $RepoRoot).ToUpperInvariant())
      }
      $byManifest[($row.name + '/' + $variant.manifest)] = @($tus)
    }
  }
  $shared = New-Object System.Collections.ArrayList
  $keys = @($byManifest.Keys)
  for ($i = 0; $i -lt $keys.Count; $i++) {
    for ($j = $i + 1; $j -lt $keys.Count; $j++) {
      foreach ($tu in $byManifest[$keys[$i]]) {
        if ($byManifest[$keys[$j]] -contains $tu) { [void]$shared.Add([ordered]@{ manifestA = $keys[$i]; manifestB = $keys[$j]; tu = $tu }) }
      }
    }
  }
  return [ordered]@{
    manifestsChecked = $keys.Count; sharedTuAcrossVariantManifests = @($shared)
    note = 'shared TUs between variant manifests are expected (each variant compiles its own set); a duplicate inside one manifest is fatal in Read-SourceManifest'
  }
}

function Clear-OwnedTree {
  param([string]$EvidenceDir)
  $removed = New-Object System.Collections.ArrayList
  foreach ($name in @('out', 'fixtures', 'stages', 'variants', 'suites', 'run.log')) {
    $target = Join-Path $EvidenceDir $name
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force; [void]$removed.Add($target) }
  }
  return @($removed)
}

$startedAt = (Get-Date).ToUniversalTime().ToString('o')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$testsDir = (Resolve-Path $PSScriptRoot).Path
if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) { throw 'EvidenceRoot must not be empty' }
$evidence = if ([IO.Path]::IsPathRooted($EvidenceRoot)) { [IO.Path]::GetFullPath($EvidenceRoot) } else { [IO.Path]::GetFullPath((Join-Path $repoRoot $EvidenceRoot)) }
$registryNames = @($Suites | ForEach-Object { $_.name })
$receipt = [ordered]@{
  kind = 'wb079-suite-run-receipt'; schemaVersion = 1
  suite = $Suite; requestedSuite = $Suite; mode = $Mode
  status = 'FAIL'; exitCode = 1; assertions = 0; selectedSuites = @(); executedSuites = @(); suites = @(); variants = @(); stages = @()
  registry = @($Suites | ForEach-Object { [ordered]@{ name = $_.name; stem = $_.stem; fixture = $_.fixture; variants = @($_.variants | ForEach-Object { $_.name }); cases = @($_.variants | ForEach-Object { @($_.cases) } | ForEach-Object { $_ } | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) }) } })
  manifestChecks = @(); duplicateTuSummary = $null
  evidenceRoot = $evidence; repoRoot = $repoRoot; testsDir = $testsDir
  compileTimeoutSeconds = $CompileTimeoutSeconds; testTimeoutSeconds = $TestTimeoutSeconds
  timeoutAuthority = 'this runner owns CompileTimeoutSeconds and TestTimeoutSeconds per variant; no outer stage timer'
  startedAt = $startedAt; endedAt = $null; error = ''; cleanup = $null
}
$discoveryWritten = $false

try {
  New-Item -ItemType Directory -Force -Path $evidence | Out-Null

  # ---- bare `-ListSuites` (WB080 Task 2 repair B2, T2-L): with no explicit -Suite, discovery enumerates
  # the whole registry. An explicitly passed -Suite (including 'All') keeps the inherited selection behavior.
  $suiteExplicit = $PSBoundParameters.ContainsKey('Suite')
  if ($ListSuites -and -not $suiteExplicit) { $Suite = 'All' }

  # ---- registry-driven selection: unknown suites fail explicitly, 'All' expands exactly the registered rows.
  if ($Suite -eq 'All') { $selected = @($Suites) }
  elseif ($registryNames -contains $Suite) { $selected = @($Suites | Where-Object { $_.name -eq $Suite }) }
  else { throw "unknown suite '$Suite': registered suites are $(($registryNames -join ', ')); 'All' expands exactly these rows" }
  $isAll = ($Suite -eq 'All') -or ($selected.Count -gt 1)
  $selectedNames = @($selected | ForEach-Object { $_.name })
  $receipt.selectedSuites = $selectedNames

  if ($Mode -ne 'Host') { throw "unsupported mode '$Mode': this runner executes Mode Host only; no Live adapter ships here" }

  if ($ListSuites) {
    $discovery = [ordered]@{
      kind = 'wb079-suite-discovery'; requestedSuite = $Suite; mode = $Mode
      suiteExplicit = $suiteExplicit
      suites = $selectedNames
      fixtures = @($selected | ForEach-Object { Join-Path $testsDir $_.fixture })
      variants = @($selected | ForEach-Object { @($_.variants | ForEach-Object { $_.name }) })
      registry = $receipt.registry; repoRoot = $repoRoot; testsDir = $testsDir
      cases = @($selected | ForEach-Object { @($_.variants | ForEach-Object { @($_.cases) }) } | ForEach-Object { $_ } | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
      at = (Get-Date).ToUniversalTime().ToString('o')
      note = 'discovery only: no compiler ran and no run receipt exists for this invocation'
    }
    Save-Json -Path (Join-Path $evidence 'discovery.json') -Object $discovery
    $discoveryWritten = $true
    $discoveryVariants = @($selected | ForEach-Object { @($_.variants | ForEach-Object { $_.name }) })
    Write-Output ('suites=' + ($selectedNames -join ',') + '; variants=' + (($discoveryVariants | ForEach-Object { $_ }) -join ','))
    exit 0
  }

  # Diagnostic binaries may select cases; an official suite receipt must never certify a subset.
  foreach ($selector in @('WB_OB_CASES', 'WB_IH_CASES', 'WB_IO_CASES')) {
    if (![string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($selector, 'Process'))) {
      throw "case-selection override '$selector' is set: official suite runs require every registered case"
    }
  }

  # ---- the registry is the single source of truth for shipped fixtures.
  $registeredFixtures = @($Suites | ForEach-Object { $_.fixture })
  $stray = @(Get-ChildItem -LiteralPath $testsDir -Filter '*_tests.cpp' -File | Where-Object { $registeredFixtures -notcontains $_.Name })
  if ($stray.Count -gt 0) { throw ('unregistered fixture(s) in ' + $testsDir + ': ' + (($stray | ForEach-Object { $_.Name }) -join ', ')) }

  # ---- preflight every manifest of every selected suite BEFORE any compiler runs.
  $manifestChecks = New-Object System.Collections.ArrayList
  foreach ($row in $selected) {
    $fixturePath = Join-Path $testsDir $row.fixture
    foreach ($chk in @(Test-SuiteManifests -SuiteRow $row -FixturePath $fixturePath -RepoRoot $repoRoot)) { [void]$manifestChecks.Add($chk) }
  }
  $receipt.manifestChecks = $manifestChecks.ToArray()
  $receipt.duplicateTuSummary = Get-DuplicateTuSummary -SelectedSuites $selected -FixtureDir $testsDir -RepoRoot $repoRoot

  $toolchain = Resolve-Toolchain
  $receipt.toolchain = $toolchain
  $removed = @(Clear-OwnedTree -EvidenceDir $evidence)
  $receipt.cleanup = [ordered]@{ tree = $evidence; staleRemoved = @($removed); stages = @(); activeAfterCleanupTotal = 0 }
  $logPath = Join-Path $evidence 'run.log'
  $suiteReceipts = New-Object System.Collections.ArrayList
  foreach ($row in $selected) {
    $suiteEvidence = if ($isAll) { Join-Path $evidence (Join-Path 'suites' $row.name) } else { $evidence }
    New-Item -ItemType Directory -Force -Path $suiteEvidence | Out-Null
    $fixturePath = Join-Path $testsDir $row.fixture
    $suiteStartedAt = (Get-Date).ToUniversalTime().ToString('o')
    $sr = [ordered]@{
      name = $row.name; fixture = $fixturePath; evidenceDir = $suiteEvidence
      status = 'FAIL'; assertions = 0; error = ''; variants = @(); stages = @()
      resultPath = $(if ($isAll) { Join-Path $suiteEvidence 'result.json' } else { Join-Path $evidence 'result.json' })
      resultSha256 = $null
    }
    try {
      foreach ($variant in $row.variants) {
        $variantReceipt = Invoke-Variant -SuiteRow $row -Variant $variant -FixturePath $fixturePath -EvidenceDir $suiteEvidence -Toolchain $toolchain -RepoRoot $repoRoot
        $sr.variants += $variantReceipt
        foreach ($stage in $variantReceipt.stages) {
          $sr.stages += $stage
          $receipt.stages += $stage
          $receipt.cleanup.stages += [ordered]@{ suite = $row.name; variant = $stage.variant; stage = $stage.stage; jobEmptyConfirmed = $stage.jobEmptyConfirmed; activeAfterCleanup = $stage.activeAfterCleanup; descendantsTerminated = $stage.descendantsTerminated; timedOut = $stage.timedOut; pid = $stage.pid; exitCode = $stage.exitCode }
        }
        if ($variantReceipt.status -ne 'PASS') { throw ('variant ' + $variant.name + ': ' + $variantReceipt.error) }
      }
      $sr.assertions = [int](($sr.variants | ForEach-Object { [int]$_.assertions } | Measure-Object -Sum).Sum)
      if ($sr.assertions -le 0) { throw ("suite $($row.name) reported no positive assertion count; a zero-assertion run cannot pass") }
      $sr.status = 'PASS'
    } catch {
      $sr.error = $_.Exception.Message
    }
    if ($isAll) {
      $child = [ordered]@{
        kind = 'wb079-suite-run-receipt'; schemaVersion = 1
        suite = $row.name; requestedSuite = $row.name; mode = $Mode
        status = $sr.status; exitCode = $(if ($sr.status -eq 'PASS') { 0 } else { 1 }); assertions = $sr.assertions
        selectedSuites = @($row.name); executedSuites = @($row.name); variants = $sr.variants; stages = $sr.stages
        registry = @($receipt.registry | Where-Object { $_.name -eq $row.name })
        manifestChecks = @($receipt.manifestChecks | Where-Object { $_.suite -eq $row.name })
        duplicateTuSummary = $receipt.duplicateTuSummary
        evidenceRoot = $suiteEvidence; repoRoot = $repoRoot; testsDir = $testsDir
        compileTimeoutSeconds = $CompileTimeoutSeconds; testTimeoutSeconds = $TestTimeoutSeconds
        timeoutAuthority = 'this runner owns CompileTimeoutSeconds and TestTimeoutSeconds per variant; no outer stage timer'
        startedAt = $suiteStartedAt; endedAt = (Get-Date).ToUniversalTime().ToString('o'); error = $sr.error
      }
      Save-Json -Path $sr.resultPath -Object $child
      $sr.resultSha256 = (Get-FileHash -LiteralPath $sr.resultPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    [void]$suiteReceipts.Add($sr)
    foreach ($v in $sr.variants) { $receipt.variants += $v }
    Add-Content -LiteralPath $logPath -Value ('[' + (Get-Date).ToUniversalTime().ToString('o') + '] suite ' + $row.name + ': ' + $sr.status + ' assertions=' + $sr.assertions + ' error=' + $sr.error) -Encoding UTF8
    if ($sr.status -ne 'PASS') { throw ('suite ' + $row.name + ': ' + $sr.error) }
  }
  $receipt.suites = $suiteReceipts.ToArray()
  $receipt.executedSuites = @($suiteReceipts | ForEach-Object { $_.name })
  foreach ($stage in $receipt.stages) { if ([int]$stage.activeAfterCleanup -gt $receipt.cleanup.activeAfterCleanupTotal) { $receipt.cleanup.activeAfterCleanupTotal = [int]$stage.activeAfterCleanup } }
  $receipt.assertions = [int](($receipt.variants | ForEach-Object { [int]$_.assertions } | Measure-Object -Sum).Sum)
  if ($receipt.assertions -le 0) { throw 'no positive assertion count was produced by any executed variant' }
  $receipt.status = 'PASS'
  $receipt.exitCode = 0
} catch {
  $receipt.error = $_.Exception.Message
} finally {
  if (-not $discoveryWritten) {
    $receipt.endedAt = (Get-Date).ToUniversalTime().ToString('o')
    Save-Json -Path (Join-Path $evidence 'result.json') -Object $receipt
  }
}
Write-Output ('suite ' + $Suite + ' (' + $Mode + '): ' + $receipt.status + ', assertions=' + $receipt.assertions + '; ' + (Join-Path $evidence 'result.json'))
if ($receipt.error) { Write-Output ('error: ' + $receipt.error) }
exit $receipt.exitCode
