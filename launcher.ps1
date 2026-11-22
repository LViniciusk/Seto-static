# Seto-static launcher: Setup (clone/extract/build) and Play modes.
# Run with: powershell -ExecutionPolicy Bypass -File launcher.ps1
# Headless check: powershell -ExecutionPolicy Bypass -File launcher.ps1 -DryRun
param([switch]$DryRun, [int]$WatchPid = 0, [switch]$Setup)

# Hidden helper: watch the game window and terminate the process after closing.
# The game itself is launched without a console or log files.
if ($WatchPid -gt 0) {
    $ErrorActionPreference = 'Stop'
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SetoGameProcessWatcher {
    private delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumProc fn, IntPtr param);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int capacity);

    public static IntPtr FindGameWindow(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, unused) => {
            uint owner;
            GetWindowThreadProcessId(hwnd, out owner);
            if (owner != pid) return true;
            var cls = new StringBuilder(128);
            GetClassName(hwnd, cls, cls.Capacity);
            if (cls.ToString() != "SetoStaticLive") return true;
            found = hwnd;
            return false;
        }, IntPtr.Zero);
        return found;
    }
}
'@
    $watchedProcess = $null
    try {
        $watchedProcess = [System.Diagnostics.Process]::GetProcessById($WatchPid)

        # Wait for the actual window, rather than treating startup as closure.
        $watchedWindow = [IntPtr]::Zero
        $deadline = [DateTime]::UtcNow.AddMinutes(5)
        while ([DateTime]::UtcNow -lt $deadline) {
            $watchedProcess.Refresh()
            if ($watchedProcess.HasExited) { return }
            $watchedWindow = [SetoGameProcessWatcher]::FindGameWindow([uint32]$WatchPid)
            if ($watchedWindow -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 250
        }
        if ($watchedWindow -eq [IntPtr]::Zero) { return }

        while (-not $watchedProcess.HasExited) {
            if (-not [SetoGameProcessWatcher]::IsWindow($watchedWindow)) {
                if (-not $watchedProcess.WaitForExit(2000)) {
                    $watchedProcess.Kill()
                    $watchedProcess.WaitForExit()
                }
                break
            }
            Start-Sleep -Milliseconds 350
            $watchedProcess.Refresh()
        }
    } catch {
        # The game may already have closed; never affect unrelated processes.
    } finally {
        if ($watchedProcess) { $watchedProcess.Dispose() }
    }
    return
}

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$iniPath = Join-Path $repoRoot 'launcher.ini'

# Ensure MSYS2 UCRT64 runtime/toolchain DLLs are available when launched from Explorer,
# appended so it does not shadow the user's primary Python installation.
$ucrtBin = 'C:\msys64\ucrt64\bin'
if ((Test-Path -LiteralPath $ucrtBin -PathType Container) -and ($env:PATH -notlike "*$ucrtBin*")) {
    $env:PATH = "$env:PATH;$ucrtBin"
}

$lightPack = 'build/texturepack-tf1-tf3'
$tcgPack = 'build/texturepack-tcg'
$defaultTexturePack = foreach ($pack in @($tcgPack, $lightPack)) {
    if (Test-Path -LiteralPath (Join-Path $repoRoot "$pack/textures.ini") -PathType Leaf) {
        $pack
        break
    }
}

$defaults = [ordered]@{
    scale        = '2'
    render_scale = 'auto'
    msaa         = '0'
    filter       = '0'
    post_filter  = '0'
    present      = 'ged'
    iso          = ''
    tf3_iso      = ''
    eboot        = 'build/reloc/elf/eboot-reloc.elf'
    flash0       = 'build/flash0'
    hd_tex_dir   = [string]$defaultTexturePack
    server       = ''
    nickname     = ''
}

$requiredPrxs = @(
    'build/reloc/elf/font-reloc.elf', 'build/reloc/elf/title-reloc.elf',
    'build/reloc/elf/story-reloc.elf', 'build/reloc/elf/field-reloc.elf',
    'build/reloc/elf/deck-reloc.elf', 'build/reloc/elf/decktut-reloc.elf',
    'build/reloc/elf/duel-mgr-reloc.elf', 'build/reloc/elf/duel-eng-reloc.elf',
    'build/reloc/elf/duel-draw-reloc.elf', 'build/reloc/elf/duel-swap-reloc.elf',
    'build/reloc/elf/duel-tut-reloc.elf', 'build/reloc/elf/duel-rec-reloc.elf',
    'build/reloc/elf/charalist-reloc.elf', 'build/reloc/elf/shop-reloc.elf'
)

$extraRuntimeArtifacts = @(
    'build/reloc/elf/duel-tf3eng-reloc.elf',
    'build/reloc/elf/cardalbum-reloc.elf', 'build/reloc/elf/limitlist-reloc.elf',
    'build/reloc/elf/recipeviewer-reloc.elf', 'build/reloc/elf/tutoriallist-reloc.elf',
    'build/reloc/elf/selectcard-reloc.elf', 'build/reloc/elf/labo-reloc.elf',
    'build/reloc/elf/conv-reloc.elf', 'build/reloc/elf/password-reloc.elf',
    'build/reloc/elf/htmlview-reloc.elf',
    'build/gx_data/cardinfo_eng.ehp',
    'build/gx_data/cardm_e.cip',
    'build/gx_data/cardh_e.cip'
)

function Load-Config {
    $cfg = [ordered]@{}
    foreach ($k in $defaults.Keys) { $cfg[$k] = $defaults[$k] }
    if (Test-Path -LiteralPath $iniPath) {
        foreach ($line in Get-Content -LiteralPath $iniPath -Encoding UTF8) {
            if ($line -match '^\s*#' -or $line -match '^\s*$') { continue }
            $kv = $line -split '=', 2
            if ($kv.Count -eq 2) { $cfg[$kv[0].Trim()] = $kv[1].Trim() }
        }
    }
    if ([string]::IsNullOrWhiteSpace($cfg.eboot) -or
        $cfg.eboot -eq 'build/s7-eboot-reloc.elf' -or
        $cfg.eboot -eq 'build/eboot-reloc.elf') {
        $cfg.eboot = 'build/reloc/elf/eboot-reloc.elf'
    }
    if ([string]::IsNullOrWhiteSpace($cfg.flash0)) {
        $cfg.flash0 = 'build/flash0'
    }
    if ($cfg.hd_tex_profile -eq 'tcg' -and $cfg.hd_tex_dir -eq $lightPack -and
        $defaultTexturePack -eq $tcgPack) {
        $cfg.hd_tex_dir = $tcgPack
    }
    if ($cfg.hd_tex_dir -in @($lightPack, $tcgPack) -and
        !(Test-Path -LiteralPath (Join-Path $repoRoot "$($cfg.hd_tex_dir)/textures.ini") -PathType Leaf)) {
        $cfg.hd_tex_dir = [string]$defaultTexturePack
    }
    $cfg.Remove('hd_tex_profile')
    if ($cfg.present -in @('gl', 'ge')) {
        $cfg.present = 'ged'
    }
    return $cfg
}

function Save-Config($cfg) {
    $lines = [string[]]@($cfg.Keys | ForEach-Object { "$_=$($cfg[$_])" })
    [IO.File]::WriteAllLines($iniPath, $lines, (New-Object System.Text.UTF8Encoding($false)))
}

function Resolve-RepoPath($p) {
    if ([string]::IsNullOrWhiteSpace($p)) { return '' }
    if ([IO.Path]::IsPathRooted($p)) { return $p }
    return Join-Path $repoRoot $p
}

function Quote($p) { return '"' + $p + '"' }

function Test-Installed($cfg) {
    $exe = Join-Path $repoRoot 'build/out/Seto-static.exe'
    if (!(Test-Path -LiteralPath $exe -PathType Leaf) -or (Get-Item -LiteralPath $exe).Length -le 1000000) {
        return $false
    }
    $eboot = Resolve-RepoPath $cfg.eboot
    if ([string]::IsNullOrWhiteSpace($eboot) -or !(Test-Path -LiteralPath $eboot -PathType Leaf)) {
        return $false
    }
    foreach ($rel in ($requiredPrxs + $extraRuntimeArtifacts)) {
        if (!(Test-Path -LiteralPath (Resolve-RepoPath $rel) -PathType Leaf)) {
            return $false
        }
    }
    $flashDir = Resolve-RepoPath $cfg.flash0
    $defaultFlashDir = Resolve-RepoPath 'build/flash0'
    $flashOk = (![string]::IsNullOrWhiteSpace($flashDir) -and (Test-Path -LiteralPath (Join-Path $flashDir 'font/ltn0.pgf') -PathType Leaf)) -or
               (Test-Path -LiteralPath (Join-Path $defaultFlashDir 'font/ltn0.pgf') -PathType Leaf)
    if (-not $flashOk) {
        return $false
    }
    return $true
}

function Sleep-Ui([int]$Ms, [scriptblock]$Pump) {
    if ($null -eq $Pump) {
        Start-Sleep -Milliseconds $Ms
        return
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $Ms) {
        & $Pump
        Start-Sleep -Milliseconds 30
    }
}

# CREATE_NO_WINDOW prevents Windows Terminal from creating a tab/window.
# Discard stdout/stderr without an unconsumed pipe or file writes.
if (-not ('SetoConsoleFreeStart' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class SetoConsoleFreeStart {
    private const uint CREATE_NO_WINDOW = 0x08000000;
    private const uint STARTF_USESTDHANDLES = 0x00000100;
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x00000001;
    private const uint FILE_SHARE_WRITE = 0x00000002;
    private const uint OPEN_EXISTING = 3;
    private const uint FILE_ATTRIBUTE_NORMAL = 0x00000080;

    [StructLayout(LayoutKind.Sequential)]
    private struct SECURITY_ATTRIBUTES {
        public int nLength;
        public IntPtr lpSecurityDescriptor;
        [MarshalAs(UnmanagedType.Bool)] public bool bInheritHandle;
    }

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    private struct STARTUPINFO {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars;
        public int dwFillAttribute;
        public uint dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION {
        public IntPtr hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [DllImport("kernel32.dll", EntryPoint="CreateFileW", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern IntPtr CreateFile(string path, uint access, uint share,
        ref SECURITY_ATTRIBUTES sa, uint disposition, uint attributes, IntPtr templateFile);

    [DllImport("kernel32.dll", EntryPoint="CreateProcessW", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcess(string appName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandles, uint flags,
        IntPtr environment, string currentDirectory,
        ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInfo);

    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    private static IntPtr OpenNullHandle(uint access) {
        SECURITY_ATTRIBUTES sa = new SECURITY_ATTRIBUTES {
            nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES)),
            bInheritHandle = true
        };
        IntPtr h = CreateFile("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              ref sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
        if (h == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot open NUL");
        return h;
    }

    public static int Launch(string exe, string arguments, string directory) {
        IntPtr input = IntPtr.Zero, output = IntPtr.Zero;
        try {
            input = OpenNullHandle(GENERIC_READ);
            output = OpenNullHandle(GENERIC_WRITE);
            STARTUPINFO si = new STARTUPINFO {
                cb = Marshal.SizeOf(typeof(STARTUPINFO)),
                dwFlags = STARTF_USESTDHANDLES,
                hStdInput = input,
                hStdOutput = output,
                hStdError = output
            };
            PROCESS_INFORMATION pi;
            var cmd = new StringBuilder("\"" + exe + "\" " + arguments);
            if (!CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, true, CREATE_NO_WINDOW,
                               IntPtr.Zero, directory, ref si, out pi))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot start Seto-static");
            try { return pi.dwProcessId; }
            finally { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        } finally {
            if (input != IntPtr.Zero) CloseHandle(input);
            if (output != IntPtr.Zero) CloseHandle(output);
        }
    }
}
'@
}

function Start-Game($cfg, [switch]$DryRun, [scriptblock]$UiPump = $null) {
    $iso = Resolve-RepoPath $cfg.iso
    $eboot = Resolve-RepoPath $cfg.eboot
    $flash0 = Resolve-RepoPath $cfg.flash0

    $missing = @()
    if ([string]::IsNullOrWhiteSpace($iso) -or !(Test-Path -LiteralPath $iso -PathType Leaf)) { $missing += 'ISO' }
    if ([string]::IsNullOrWhiteSpace($eboot) -or !(Test-Path -LiteralPath $eboot -PathType Leaf)) { $missing += 'EBOOT' }
    if ([string]::IsNullOrWhiteSpace($flash0) -or !(Test-Path -LiteralPath $flash0 -PathType Container)) { $missing += 'flash0' }

    $exe = Join-Path $repoRoot 'build/out/Seto-static.exe'
    if (!(Test-Path -LiteralPath $exe -PathType Leaf) -or (Get-Item -LiteralPath $exe).Length -le 1000000) {
        $missing += 'build/out/Seto-static.exe'
    }

    $prx = $requiredPrxs | ForEach-Object { Resolve-RepoPath $_ }
    foreach ($p in $prx) { if (!(Test-Path -LiteralPath $p -PathType Leaf)) { $missing += $p } }

    if ($missing.Count) { throw ("Missing files:`n" + ($missing -join "`n")) }

    $gameArgs = (@($eboot, $iso) + $prx | ForEach-Object { Quote $_ }) -join ' '

    foreach ($e in Get-ChildItem Env: | Where-Object {
        $_.Name -like 'SETO_*' -or $_.Name -like 'PSPRECOMP_TRACE*' -or $_.Name -eq 'TF3_BRIDGE_TRACE'
    }) {
        Remove-Item ("Env:" + $e.Name) -ErrorAction SilentlyContinue
    }
    $env:PSPRECOMP_FLASH0 = $flash0
    $env:SETO_MANUAL = '1'
    $env:SETO_HOLD_WINDOW = '1'
    $env:SETO_SCALE = [string]$cfg.scale
    $env:SETO_FILTER = [string]$cfg.filter
    if ($cfg.present -eq 'ged') {
        $env:SETO_PRESENT = 'gl'
        $env:SETO_POST_FILTER = [string]$cfg.post_filter
        $env:SETO_GE_GPU = 'draw'
        $env:SETO_RENDERER = 'gl'
        $env:SETO_RENDER_SCALE = [string]$cfg.render_scale
        $env:SETO_MSAA = [string]$cfg.msaa
        if (![string]::IsNullOrWhiteSpace($cfg.hd_tex_dir)) {
            $env:SETO_HD_TEX_DIR = Resolve-RepoPath $cfg.hd_tex_dir
        }
    }
    if (![string]::IsNullOrWhiteSpace($cfg.server)) {
        $env:SETO_SERVER = [string]$cfg.server
    }
    if (![string]::IsNullOrWhiteSpace($cfg.nickname)) {
        $env:SETO_NICKNAME = [string]$cfg.nickname
    }
    if ($DryRun) {
        Write-Output "exe: $exe"
        Write-Output "args: $gameArgs"
        Write-Output "env: SETO_MANUAL=1 SETO_HOLD_WINDOW=1 SETO_SCALE=$($cfg.scale) SETO_FILTER=$($cfg.filter) SETO_RENDER_SCALE=$($cfg.render_scale) SETO_MSAA=$($cfg.msaa) SETO_POST_FILTER=$($cfg.post_filter) SETO_HD_TEX_DIR=$($cfg.hd_tex_dir)"
        Write-Output "present: $($cfg.present)"
        Write-Output "net: server=$($cfg.server) nickname=$($cfg.nickname)"
        Write-Output "flash0: $flash0"
        return
    }
    $gamePid = [SetoConsoleFreeStart]::Launch($exe, $gameArgs, $repoRoot)
    $gameProcess = [System.Diagnostics.Process]::GetProcessById($gamePid)

    # Keep the existing independent close-on-X monitor, also without a console.
    $watchArgs = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -WatchPid {1}' -f `
        $PSCommandPath, $gamePid
    $watchPsi = New-Object System.Diagnostics.ProcessStartInfo
    $watchPsi.FileName = 'powershell.exe'
    $watchPsi.Arguments = $watchArgs
    $watchPsi.WorkingDirectory = $repoRoot
    $watchPsi.UseShellExecute = $false
    $watchPsi.CreateNoWindow = $true
    [void][System.Diagnostics.Process]::Start($watchPsi)

    # Wait for the REAL game window (the executable may take >30s to load PRXs).
    # Target it by PID and window class; don't accidentally maximize another instance.
    if (-not ('SetoWindowMaxV2' -as [type])) {
        Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SetoWindowMaxV2 {
  private delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc fn, IntPtr lParam);
  [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  private static extern int GetClassName(IntPtr hWnd, StringBuilder name, int capacity);
  [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int command);

  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((hwnd, unused) => {
      uint owner;
      GetWindowThreadProcessId(hwnd, out owner);
      if (owner != pid || !IsWindowVisible(hwnd)) return true;
      var cls = new StringBuilder(128);
      GetClassName(hwnd, cls, cls.Capacity);
      if (cls.ToString() != "SetoStaticLive") return true;
      found = hwnd;
      return false;
    }, IntPtr.Zero);
    return found;
  }
}
'@
    }

    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    $hwnd = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $deadline) {
        $gameProcess.Refresh()
        if ($gameProcess.HasExited) { throw 'Seto-static exited before opening its window.' }
        $hwnd = [SetoWindowMaxV2]::Find([uint32]$gameProcess.Id)
        if ($hwnd -ne [IntPtr]::Zero) { break }
        Sleep-Ui 250 $UiPump
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw 'Could not find the Seto-static window to maximize.' }

    # Some startup paths call SW_SHOWNORMAL after creating the window.
    # Retry until Windows confirms the window really is maximized.
    $maximized = $false
    for ($i = 0; $i -lt 25; $i++) {
        [void][SetoWindowMaxV2]::ShowWindowAsync($hwnd, 3) # SW_MAXIMIZE
        Sleep-Ui 400 $UiPump
        if ([SetoWindowMaxV2]::IsZoomed($hwnd)) {
            Sleep-Ui 500 $UiPump
            if ([SetoWindowMaxV2]::IsZoomed($hwnd)) { $maximized = $true; break }
        }
    }
    if (-not $maximized) { throw 'Windows did not confirm that Seto-static was maximized.' }
}

$cfg = Load-Config
if ($DryRun) { Start-Game $cfg -DryRun; return }

Add-Type -AssemblyName PresentationFramework
Add-Type -AssemblyName PresentationCore
Add-Type -AssemblyName WindowsBase
Add-Type -AssemblyName System.Windows.Forms

if (-not ('SetoDwmTheme' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SetoDwmTheme {
    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);
    [DllImport("kernel32.dll")]
    private static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    public static void ApplyDarkTitleBar(IntPtr hwnd) {
        if (hwnd == IntPtr.Zero) return;
        try {
            int dark = 1;
            DwmSetWindowAttribute(hwnd, 20, ref dark, sizeof(int));
            int corner = 1; // DWMWCP_DONOTROUND
            DwmSetWindowAttribute(hwnd, 33, ref corner, sizeof(int));
        } catch {}
    }

    public static void HideConsole() {
        try {
            IntPtr hwnd = GetConsoleWindow();
            if (hwnd != IntPtr.Zero) ShowWindow(hwnd, 0);
        } catch {}
    }
}
'@
}
[SetoDwmTheme]::HideConsole()

if (-not ('SetoSetupRunner' -as [type])) {
    Add-Type @'
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;

public sealed class SetoSetupRunner : IDisposable {
    private readonly Process _proc;
    private readonly ConcurrentQueue<string> _lines = new ConcurrentQueue<string>();
    private IntPtr _job = IntPtr.Zero;
    private readonly object _jobLock = new object();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr hJob, int infoClass, IntPtr lpJobObjectInfo, uint cbJobObjectInfoLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateJobObject(IntPtr hJob, uint uExitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    private const int JobObjectExtendedLimitInformation = 9;
    private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;

    public bool HasExited {
        get { return _proc.HasExited; }
    }
    public int ExitCode {
        get { return _proc.ExitCode; }
    }

    private void AttachKillOnCloseJob() {
        try {
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            if (job == IntPtr.Zero) return;
            var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            int cb = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
            IntPtr ptr = Marshal.AllocHGlobal(cb);
            try {
                Marshal.StructureToPtr(info, ptr, false);
                if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, ptr, (uint)cb) &&
                    AssignProcessToJobObject(job, _proc.Handle)) {
                    _job = job;
                    job = IntPtr.Zero;
                }
            } finally {
                Marshal.FreeHGlobal(ptr);
                if (job != IntPtr.Zero) CloseHandle(job);
            }
        } catch {}
    }

    private void CloseJob() {
        lock (_jobLock) {
            if (_job != IntPtr.Zero) {
                try { CloseHandle(_job); } catch {}
                _job = IntPtr.Zero;
            }
        }
    }

    public SetoSetupRunner(string exe, string args, string workDir) {
        var psi = new ProcessStartInfo {
            FileName = exe,
            Arguments = args,
            WorkingDirectory = workDir,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };
        _proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
        _proc.OutputDataReceived += (s, e) => { if (e.Data != null) _lines.Enqueue(e.Data); };
        _proc.ErrorDataReceived += (s, e) => { if (e.Data != null) _lines.Enqueue(e.Data); };
        _proc.Start();
        AttachKillOnCloseJob();
        _proc.BeginOutputReadLine();
        _proc.BeginErrorReadLine();
    }

    public bool TryDequeue(out string line) {
        return _lines.TryDequeue(out line);
    }

    public void Kill() {
        lock (_jobLock) {
            if (_job != IntPtr.Zero) {
                try { TerminateJobObject(_job, 1); } catch {}
                try { CloseHandle(_job); } catch {}
                _job = IntPtr.Zero;
            }
        }
        try {
            if (!_proc.HasExited) {
                var tk = Process.Start(new ProcessStartInfo {
                    FileName = "taskkill.exe",
                    Arguments = "/PID " + _proc.Id + " /T /F",
                    UseShellExecute = false,
                    CreateNoWindow = true
                });
                if (tk != null) {
                    tk.WaitForExit(3000);
                    tk.Dispose();
                }
            }
        } catch {}
        try { if (!_proc.HasExited) _proc.Kill(); } catch {}
    }

    public void Dispose() {
        CloseJob();
        _proc.Dispose();
    }
}
'@
}

$scaleLabels = [ordered]@{
    '960 x 544 (2x)'   = '2'
    '1440 x 816 (3x)'  = '3'
    '1920 x 1088 (4x)' = '4'
    '2560 x 1448 (5x)' = '5'
    '2880 x 1632 (6x)' = '6'
    '3840 x 2176 (8x)' = '8'
}
$filterLabels = [ordered]@{
    'Nearest'  = '0'
    'Bilinear' = '1'
    'Bicubic'  = '2'
}
$renderScaleLabels = [ordered]@{
    'Auto'             = 'auto'
    '1x (480 x 272)'   = '1'
    '2x (960 x 544)'   = '2'
    '3x (1440 x 816)'  = '3'
    '4x (1920 x 1088)' = '4'
    '6x (2880 x 1632)' = '6'
    '8x (3840 x 2176)' = '8'
}
$msaaLabels = [ordered]@{
    'Off' = '0'
    '2x'  = '2'
    '4x'  = '4'
    '8x'  = '8'
}
$postFilterLabels = [ordered]@{
    'Off'           = '0'
    'CAS Sharpen'   = '1'
    'PSP LCD'       = '2'
    'CRT Scanlines' = '3'
}
$presentLabels = [ordered]@{
    'GDI'    = '0'
    'OpenGL' = 'ged'
}

[xml]$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Seto-static"
        Width="640"
        SizeToContent="Height"
        ResizeMode="CanMinimize"
        WindowStartupLocation="CenterScreen"
        Background="#09090B"
        Foreground="#F4F4F5"
        FontFamily="Segoe UI"
        FontSize="12.5"
        UseLayoutRounding="True"
        SnapsToDevicePixels="True">
    <Window.Resources>
        <SolidColorBrush x:Key="BgDeep" Color="#09090B"/>
        <SolidColorBrush x:Key="CardBg" Color="#111113"/>
        <SolidColorBrush x:Key="CardBorder" Color="#27272A"/>
        <SolidColorBrush x:Key="InputBg" Color="#09090B"/>
        <SolidColorBrush x:Key="InputHoverBg" Color="#161619"/>
        <SolidColorBrush x:Key="InputBorder" Color="#2E2E33"/>
        <SolidColorBrush x:Key="InputFocusBorder" Color="#FFFFFF"/>
        <SolidColorBrush x:Key="InputDisabledBg" Color="#0E0E10"/>
        <SolidColorBrush x:Key="TextPrimary" Color="#F4F4F5"/>
        <SolidColorBrush x:Key="TextSecondary" Color="#A1A1AA"/>
        <SolidColorBrush x:Key="TextMuted" Color="#52525B"/>
        <SolidColorBrush x:Key="AccentWhite" Color="#FFFFFF"/>

        <Style x:Key="DarkScrollThumb" TargetType="Thumb">
            <Setter Property="OverridesDefaultStyle" Value="True"/>
            <Setter Property="IsTabStop" Value="False"/>
            <Setter Property="Background" Value="#27272A"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Thumb">
                        <Border x:Name="ThumbBd" Background="{TemplateBinding Background}" CornerRadius="0" Margin="1"/>
                        <ControlTemplate.Triggers>
                            <Trigger Property="IsMouseOver" Value="True">
                                <Setter TargetName="ThumbBd" Property="Background" Value="#3F3F46"/>
                            </Trigger>
                            <Trigger Property="IsDragging" Value="True">
                                <Setter TargetName="ThumbBd" Property="Background" Value="#52525B"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>

        <Style TargetType="ScrollBar">
            <Setter Property="OverridesDefaultStyle" Value="True"/>
            <Setter Property="Background" Value="#09090B"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="ScrollBar">
                        <Grid Background="{TemplateBinding Background}" SnapsToDevicePixels="True">
                            <Track x:Name="PART_Track" IsDirectionReversed="True">
                                <Track.DecreaseRepeatButton>
                                    <RepeatButton Command="ScrollBar.PageUpCommand" Opacity="0" Focusable="False"/>
                                </Track.DecreaseRepeatButton>
                                <Track.Thumb>
                                    <Thumb Style="{StaticResource DarkScrollThumb}"/>
                                </Track.Thumb>
                                <Track.IncreaseRepeatButton>
                                    <RepeatButton Command="ScrollBar.PageDownCommand" Opacity="0" Focusable="False"/>
                                </Track.IncreaseRepeatButton>
                            </Track>
                        </Grid>
                        <ControlTemplate.Triggers>
                            <Trigger Property="Orientation" Value="Horizontal">
                                <Setter Property="Height" Value="8"/>
                                <Setter Property="Width" Value="Auto"/>
                                <Setter TargetName="PART_Track" Property="IsDirectionReversed" Value="False"/>
                            </Trigger>
                            <Trigger Property="Orientation" Value="Vertical">
                                <Setter Property="Width" Value="8"/>
                                <Setter Property="Height" Value="Auto"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>

        <Style TargetType="ScrollViewer">
            <Setter Property="OverridesDefaultStyle" Value="True"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="ScrollViewer">
                        <Grid Background="{TemplateBinding Background}">
                            <Grid.ColumnDefinitions>
                                <ColumnDefinition Width="*"/>
                                <ColumnDefinition Width="Auto"/>
                            </Grid.ColumnDefinitions>
                            <Grid.RowDefinitions>
                                <RowDefinition Height="*"/>
                                <RowDefinition Height="Auto"/>
                            </Grid.RowDefinitions>
                            <ScrollContentPresenter Grid.Row="0" Grid.Column="0" Margin="{TemplateBinding Padding}"/>
                            <ScrollBar x:Name="PART_VerticalScrollBar"
                                       Grid.Row="0" Grid.Column="1"
                                       Orientation="Vertical"
                                       Value="{TemplateBinding VerticalOffset}"
                                       Maximum="{TemplateBinding ScrollableHeight}"
                                       ViewportSize="{TemplateBinding ViewportHeight}"
                                       Visibility="{TemplateBinding ComputedVerticalScrollBarVisibility}"/>
                            <ScrollBar x:Name="PART_HorizontalScrollBar"
                                       Grid.Row="1" Grid.Column="0"
                                       Orientation="Horizontal"
                                       Value="{TemplateBinding HorizontalOffset}"
                                       Maximum="{TemplateBinding ScrollableWidth}"
                                       ViewportSize="{TemplateBinding ViewportWidth}"
                                       Visibility="{TemplateBinding ComputedHorizontalScrollBarVisibility}"/>
                            <Border Grid.Row="1" Grid.Column="1" Background="#09090B"/>
                        </Grid>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>

        <Style x:Key="SectionTitle" TargetType="TextBlock">
            <Setter Property="FontSize" Value="10.5"/>
            <Setter Property="FontWeight" Value="SemiBold"/>
            <Setter Property="Foreground" Value="#71717A"/>
            <Setter Property="Margin" Value="0,0,0,10"/>
        </Style>

        <Style x:Key="FieldLabel" TargetType="TextBlock">
            <Setter Property="FontSize" Value="12"/>
            <Setter Property="Foreground" Value="{StaticResource TextSecondary}"/>
            <Setter Property="Margin" Value="0,0,0,4"/>
        </Style>

        <Style x:Key="PathLabel" TargetType="TextBlock">
            <Setter Property="FontSize" Value="12"/>
            <Setter Property="Foreground" Value="{StaticResource TextSecondary}"/>
            <Setter Property="VerticalAlignment" Value="Center"/>
        </Style>

        <Style x:Key="CardBox" TargetType="Border">
            <Setter Property="Background" Value="{StaticResource CardBg}"/>
            <Setter Property="BorderBrush" Value="{StaticResource CardBorder}"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="CornerRadius" Value="0"/>
            <Setter Property="Padding" Value="14,12,14,13"/>
            <Setter Property="Margin" Value="0,0,0,10"/>
        </Style>

        <Style TargetType="TextBox">
            <Setter Property="Background" Value="{StaticResource InputBg}"/>
            <Setter Property="Foreground" Value="{StaticResource TextPrimary}"/>
            <Setter Property="BorderBrush" Value="{StaticResource InputBorder}"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="CaretBrush" Value="{StaticResource AccentWhite}"/>
            <Setter Property="SelectionBrush" Value="#3F3F46"/>
            <Setter Property="Height" Value="30"/>
            <Setter Property="Padding" Value="8,0"/>
            <Setter Property="VerticalContentAlignment" Value="Center"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="TextBox">
                        <Border x:Name="Bd"
                                Background="{TemplateBinding Background}"
                                BorderBrush="{TemplateBinding BorderBrush}"
                                BorderThickness="{TemplateBinding BorderThickness}"
                                CornerRadius="0">
                            <ScrollViewer x:Name="PART_ContentHost"
                                          Margin="{TemplateBinding Padding}"
                                          VerticalAlignment="{TemplateBinding VerticalContentAlignment}"/>
                        </Border>
                        <ControlTemplate.Triggers>
                            <Trigger Property="IsMouseOver" Value="True">
                                <Setter TargetName="Bd" Property="Background" Value="{StaticResource InputHoverBg}"/>
                                <Setter TargetName="Bd" Property="BorderBrush" Value="#52525B"/>
                            </Trigger>
                            <Trigger Property="IsKeyboardFocused" Value="True">
                                <Setter TargetName="Bd" Property="Background" Value="{StaticResource InputHoverBg}"/>
                                <Setter TargetName="Bd" Property="BorderBrush" Value="{StaticResource InputFocusBorder}"/>
                            </Trigger>
                            <Trigger Property="IsEnabled" Value="False">
                                <Setter TargetName="Bd" Property="Background" Value="{StaticResource InputDisabledBg}"/>
                                <Setter TargetName="Bd" Property="BorderBrush" Value="#1E1E22"/>
                                <Setter Property="Foreground" Value="{StaticResource TextMuted}"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>

        <Style TargetType="ComboBoxItem">
            <Setter Property="Background" Value="#111113"/>
            <Setter Property="Foreground" Value="{StaticResource TextPrimary}"/>
            <Setter Property="Padding" Value="10,6"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="ComboBoxItem">
                        <Border x:Name="ItemBd" Background="{TemplateBinding Background}" Padding="{TemplateBinding Padding}">
                            <ContentPresenter/>
                        </Border>
                        <ControlTemplate.Triggers>
                            <Trigger Property="IsHighlighted" Value="True">
                                <Setter TargetName="ItemBd" Property="Background" Value="#232327"/>
                            </Trigger>
                            <Trigger Property="IsSelected" Value="True">
                                <Setter TargetName="ItemBd" Property="Background" Value="#2E2E34"/>
                                <Setter Property="Foreground" Value="#FFFFFF"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>

        <Style TargetType="ComboBox">
            <Setter Property="Background" Value="{StaticResource InputBg}"/>
            <Setter Property="Foreground" Value="{StaticResource TextPrimary}"/>
            <Setter Property="BorderBrush" Value="{StaticResource InputBorder}"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="Height" Value="30"/>
            <Setter Property="Padding" Value="9,0,28,0"/>
            <Setter Property="VerticalContentAlignment" Value="Center"/>
            <Setter Property="SnapsToDevicePixels" Value="True"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="ComboBox">
                        <Grid>
                            <ToggleButton x:Name="ToggleButton"
                                          Focusable="False"
                                          IsChecked="{Binding IsDropDownOpen, Mode=TwoWay, RelativeSource={RelativeSource TemplatedParent}}"
                                          ClickMode="Press">
                                <ToggleButton.Template>
                                    <ControlTemplate TargetType="ToggleButton">
                                        <Border x:Name="Bd"
                                                Background="{Binding Background, RelativeSource={RelativeSource AncestorType=ComboBox}}"
                                                BorderBrush="{Binding BorderBrush, RelativeSource={RelativeSource AncestorType=ComboBox}}"
                                                BorderThickness="{Binding BorderThickness, RelativeSource={RelativeSource AncestorType=ComboBox}}"
                                                CornerRadius="0">
                                            <Grid>
                                                <Grid.ColumnDefinitions>
                                                    <ColumnDefinition Width="*"/>
                                                    <ColumnDefinition Width="26"/>
                                                </Grid.ColumnDefinitions>
                                                <Path Grid.Column="1"
                                                      HorizontalAlignment="Center"
                                                      VerticalAlignment="Center"
                                                      Data="M 0 0 L 4 4 L 8 0"
                                                      Stroke="#A1A1AA"
                                                      StrokeThickness="1.5"/>
                                            </Grid>
                                        </Border>
                                        <ControlTemplate.Triggers>
                                            <Trigger Property="IsMouseOver" Value="True">
                                                <Setter TargetName="Bd" Property="Background" Value="{StaticResource InputHoverBg}"/>
                                                <Setter TargetName="Bd" Property="BorderBrush" Value="#52525B"/>
                                            </Trigger>
                                            <Trigger Property="IsChecked" Value="True">
                                                <Setter TargetName="Bd" Property="BorderBrush" Value="{StaticResource InputFocusBorder}"/>
                                            </Trigger>
                                            <Trigger Property="IsEnabled" Value="False">
                                                <Setter TargetName="Bd" Property="Background" Value="{StaticResource InputDisabledBg}"/>
                                                <Setter TargetName="Bd" Property="BorderBrush" Value="#1E1E22"/>
                                            </Trigger>
                                        </ControlTemplate.Triggers>
                                    </ControlTemplate>
                                </ToggleButton.Template>
                            </ToggleButton>
                            <ContentPresenter x:Name="ContentSite"
                                              IsHitTestVisible="False"
                                              Content="{TemplateBinding SelectionBoxItem}"
                                              ContentTemplate="{TemplateBinding SelectionBoxItemTemplate}"
                                              Margin="{TemplateBinding Padding}"
                                              VerticalAlignment="Center"
                                              HorizontalAlignment="Left"/>
                            <Popup x:Name="Popup"
                                   Placement="Bottom"
                                   IsOpen="{TemplateBinding IsDropDownOpen}"
                                   AllowsTransparency="True"
                                   Focusable="False"
                                   PopupAnimation="None">
                                <Border x:Name="DropDownBorder"
                                        MinWidth="{TemplateBinding ActualWidth}"
                                        MaxHeight="240"
                                        Margin="0,2,0,0"
                                        Background="#111113"
                                        BorderBrush="#3F3F46"
                                        BorderThickness="1"
                                        CornerRadius="0">
                                    <ScrollViewer Margin="1,2" SnapsToDevicePixels="True">
                                        <StackPanel IsItemsHost="True" KeyboardNavigation.DirectionalNavigation="Contained"/>
                                    </ScrollViewer>
                                </Border>
                            </Popup>
                        </Grid>
                        <ControlTemplate.Triggers>
                            <Trigger Property="IsEnabled" Value="False">
                                <Setter Property="Foreground" Value="{StaticResource TextMuted}"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>

        <Style x:Key="SecondaryButton" TargetType="Button">
            <Setter Property="Background" Value="#18181B"/>
            <Setter Property="Foreground" Value="#E4E4E7"/>
            <Setter Property="BorderBrush" Value="#2E2E33"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="Height" Value="30"/>
            <Setter Property="Padding" Value="12,0"/>
            <Setter Property="FontWeight" Value="SemiBold"/>
            <Setter Property="FontSize" Value="12"/>
            <Setter Property="Cursor" Value="Hand"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border x:Name="Bd"
                                Background="{TemplateBinding Background}"
                                BorderBrush="{TemplateBinding BorderBrush}"
                                BorderThickness="{TemplateBinding BorderThickness}"
                                CornerRadius="0"
                                Padding="{TemplateBinding Padding}">
                            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
                        </Border>
                        <ControlTemplate.Triggers>
                            <Trigger Property="IsMouseOver" Value="True">
                                <Setter TargetName="Bd" Property="Background" Value="#27272A"/>
                                <Setter TargetName="Bd" Property="BorderBrush" Value="#52525B"/>
                            </Trigger>
                            <Trigger Property="IsPressed" Value="True">
                                <Setter TargetName="Bd" Property="Background" Value="#111113"/>
                            </Trigger>
                            <Trigger Property="IsEnabled" Value="False">
                                <Setter TargetName="Bd" Property="Opacity" Value="0.4"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>

        <Style x:Key="PrimaryButton" TargetType="Button" BasedOn="{StaticResource SecondaryButton}">
            <Setter Property="Background" Value="#FFFFFF"/>
            <Setter Property="Foreground" Value="#09090B"/>
            <Setter Property="BorderBrush" Value="#FFFFFF"/>
            <Setter Property="Height" Value="32"/>
            <Setter Property="FontSize" Value="12.5"/>
            <Setter Property="FontWeight" Value="Bold"/>
            <Setter Property="Padding" Value="22,0"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border x:Name="Bd"
                                Background="{TemplateBinding Background}"
                                BorderBrush="{TemplateBinding BorderBrush}"
                                BorderThickness="{TemplateBinding BorderThickness}"
                                CornerRadius="0"
                                Padding="{TemplateBinding Padding}">
                            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
                        </Border>
                        <ControlTemplate.Triggers>
                            <Trigger Property="IsMouseOver" Value="True">
                                <Setter TargetName="Bd" Property="Background" Value="#E4E4E7"/>
                                <Setter TargetName="Bd" Property="BorderBrush" Value="#E4E4E7"/>
                            </Trigger>
                            <Trigger Property="IsPressed" Value="True">
                                <Setter TargetName="Bd" Property="Background" Value="#D4D4D8"/>
                            </Trigger>
                            <Trigger Property="IsEnabled" Value="False">
                                <Setter TargetName="Bd" Property="Opacity" Value="0.45"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>
    </Window.Resources>

    <Grid Margin="16,14,16,16">
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="Auto"/>
        </Grid.RowDefinitions>

        <!-- Header -->
        <Grid Grid.Row="0" Margin="0,0,0,12">
            <Grid.ColumnDefinitions>
                <ColumnDefinition Width="*"/>
                <ColumnDefinition Width="Auto"/>
            </Grid.ColumnDefinitions>

            <TextBlock Grid.Column="0"
                       Text="SETO-STATIC"
                       FontSize="15"
                       FontWeight="Bold"
                       Foreground="#FFFFFF"
                       VerticalAlignment="Center"/>

            <Border x:Name="ReadyBadge"
                    Grid.Column="1"
                    VerticalAlignment="Center"
                    CornerRadius="0"
                    Padding="9,4"
                    BorderThickness="1"
                    Background="#141416"
                    BorderBrush="#2E2E33">
                <StackPanel Orientation="Horizontal">
                    <Rectangle x:Name="ReadyDot" Width="6" Height="6" Fill="#FFFFFF" Margin="0,0,7,0" VerticalAlignment="Center"/>
                    <TextBlock x:Name="ReadyText" Text="Ready" FontSize="11" FontWeight="SemiBold" Foreground="#E4E4E7"/>
                </StackPanel>
            </Border>
        </Grid>

        <!-- PLAY VIEW -->
        <StackPanel x:Name="PlayView" Grid.Row="1">
            <Border Style="{StaticResource CardBox}">
                <StackPanel>
                    <TextBlock Text="GRAPHICS" Style="{StaticResource SectionTitle}"/>

                    <Grid Margin="0,0,0,10">
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="1.7*"/>
                            <ColumnDefinition Width="12"/>
                            <ColumnDefinition Width="1.3*"/>
                            <ColumnDefinition Width="12"/>
                            <ColumnDefinition Width="1*"/>
                        </Grid.ColumnDefinitions>

                        <StackPanel Grid.Column="0">
                            <TextBlock Text="Presenter" Style="{StaticResource FieldLabel}"/>
                            <ComboBox x:Name="PresentBox"/>
                        </StackPanel>

                        <StackPanel Grid.Column="2">
                            <TextBlock Text="Window Resolution" Style="{StaticResource FieldLabel}"/>
                            <ComboBox x:Name="ScaleBox"/>
                        </StackPanel>

                        <StackPanel Grid.Column="4">
                            <TextBlock Text="Filter" Style="{StaticResource FieldLabel}"/>
                            <ComboBox x:Name="FilterBox"/>
                        </StackPanel>
                    </Grid>

                    <Grid>
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="1.4*"/>
                            <ColumnDefinition Width="12"/>
                            <ColumnDefinition Width="0.8*"/>
                            <ColumnDefinition Width="12"/>
                            <ColumnDefinition Width="1.2*"/>
                        </Grid.ColumnDefinitions>

                        <StackPanel Grid.Column="0">
                            <TextBlock x:Name="RenderScaleLabel" Text="Internal 3D Scale" Style="{StaticResource FieldLabel}"/>
                            <ComboBox x:Name="RenderScaleBox"/>
                        </StackPanel>

                        <StackPanel Grid.Column="2">
                            <TextBlock x:Name="MsaaLabel" Text="MSAA 3D" Style="{StaticResource FieldLabel}"/>
                            <ComboBox x:Name="MsaaBox"/>
                        </StackPanel>

                        <StackPanel Grid.Column="4">
                            <TextBlock x:Name="PostFilterLabel" Text="Post Filter" Style="{StaticResource FieldLabel}"/>
                            <ComboBox x:Name="PostFilterBox"/>
                        </StackPanel>
                    </Grid>
                </StackPanel>
            </Border>

            <Border Style="{StaticResource CardBox}">
                <Grid>
                    <Grid.RowDefinitions>
                        <RowDefinition Height="Auto"/>
                        <RowDefinition Height="36"/>
                        <RowDefinition Height="36"/>
                        <RowDefinition Height="34"/>
                        <RowDefinition Height="Auto"/>
                    </Grid.RowDefinitions>
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="96"/>
                        <ColumnDefinition Width="16"/>
                        <ColumnDefinition Width="*"/>
                        <ColumnDefinition Width="8"/>
                        <ColumnDefinition Width="78"/>
                        <ColumnDefinition Width="6"/>
                        <ColumnDefinition Width="32"/>
                    </Grid.ColumnDefinitions>

                    <TextBlock Grid.Row="0" Grid.ColumnSpan="7" Text="PATHS" Style="{StaticResource SectionTitle}"/>

                    <TextBlock Grid.Row="1" Grid.Column="0" Text="TF1 ISO" Style="{StaticResource PathLabel}"/>
                    <Rectangle x:Name="IsoDot" Grid.Row="1" Grid.Column="1" Width="6" Height="6" Fill="#FFFFFF" VerticalAlignment="Center" HorizontalAlignment="Left"/>
                    <TextBox x:Name="IsoBox" Grid.Row="1" Grid.Column="2"/>
                    <Button x:Name="IsoBrowseBtn" Grid.Row="1" Grid.Column="4" Grid.ColumnSpan="3" Content="Browse..." Style="{StaticResource SecondaryButton}"/>

                    <TextBlock Grid.Row="2" Grid.Column="0" Text="flash0 Folder" Style="{StaticResource PathLabel}"/>
                    <Rectangle x:Name="FlashDot" Grid.Row="2" Grid.Column="1" Width="6" Height="6" Fill="#FFFFFF" VerticalAlignment="Center" HorizontalAlignment="Left"/>
                    <TextBox x:Name="FlashBox" Grid.Row="2" Grid.Column="2"/>
                    <Button x:Name="FlashBrowseBtn" Grid.Row="2" Grid.Column="4" Grid.ColumnSpan="3" Content="Browse..." Style="{StaticResource SecondaryButton}"/>

                    <TextBlock x:Name="HdTexLabel" Grid.Row="3" Grid.Column="0" Text="HD Textures" Style="{StaticResource PathLabel}"/>
                    <Rectangle x:Name="HdTexDot" Grid.Row="3" Grid.Column="1" Width="6" Height="6" Fill="#52525B" VerticalAlignment="Center" HorizontalAlignment="Left"/>
                    <TextBox x:Name="HdTexBox" Grid.Row="3" Grid.Column="2"/>
                    <Button x:Name="HdTexBrowseBtn" Grid.Row="3" Grid.Column="4" Content="Browse..." Style="{StaticResource SecondaryButton}"/>
                    <Button x:Name="HdTexClearBtn" Grid.Row="3" Grid.Column="6" Content="&#215;" FontSize="14" ToolTip="Disable HD textures" Style="{StaticResource SecondaryButton}" Padding="0"/>

                    <TextBlock x:Name="HdTexHint" Grid.Row="4" Grid.Column="2" Grid.ColumnSpan="5" Margin="0,4,0,0" FontSize="11" Foreground="{StaticResource TextMuted}" Text=""/>
                </Grid>
            </Border>

            <Border Style="{StaticResource CardBox}">
                <StackPanel>
                    <TextBlock Text="NETPLAY" Style="{StaticResource SectionTitle}"/>
                    <Grid>
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="1.4*"/>
                            <ColumnDefinition Width="12"/>
                            <ColumnDefinition Width="1*"/>
                        </Grid.ColumnDefinitions>

                        <StackPanel Grid.Column="0">
                            <TextBlock Text="Server (host:port)" Style="{StaticResource FieldLabel}"/>
                            <TextBox x:Name="ServerBox"/>
                        </StackPanel>

                        <StackPanel Grid.Column="2">
                            <TextBlock Text="Nickname" Style="{StaticResource FieldLabel}"/>
                            <TextBox x:Name="NicknameBox"/>
                        </StackPanel>
                    </Grid>
                </StackPanel>
            </Border>

            <Grid Margin="0,4,0,0">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*"/>
                    <ColumnDefinition Width="Auto"/>
                </Grid.ColumnDefinitions>

                <TextBlock x:Name="StatusText"
                           Grid.Column="0"
                           VerticalAlignment="Center"
                           TextWrapping="Wrap"
                           Margin="2,0,12,0"
                           FontSize="11.5"
                           Foreground="{StaticResource TextSecondary}"
                           Text=""/>

                <StackPanel Grid.Column="1" Orientation="Horizontal" VerticalAlignment="Center">
                    <Button x:Name="GoSetupBtn" Content="Setup" Style="{StaticResource SecondaryButton}" Height="32" Margin="0,0,8,0"/>
                    <Button x:Name="ResetBtn" Content="Defaults" Style="{StaticResource SecondaryButton}" Height="32" Margin="0,0,8,0"/>
                    <Button x:Name="CancelBtn" Content="Close" Style="{StaticResource SecondaryButton}" Height="32" Margin="0,0,8,0"/>
                    <Button x:Name="SaveBtn" Content="Save" Style="{StaticResource SecondaryButton}" Height="32" Margin="0,0,8,0"/>
                    <Button x:Name="StartBtn" Content="Play" Style="{StaticResource PrimaryButton}"/>
                </StackPanel>
            </Grid>
        </StackPanel>

        <!-- SETUP VIEW -->
        <StackPanel x:Name="SetupView" Grid.Row="1" Visibility="Collapsed">
            <Border Style="{StaticResource CardBox}">
                <Grid>
                    <Grid.RowDefinitions>
                        <RowDefinition Height="Auto"/>
                        <RowDefinition Height="36"/>
                        <RowDefinition Height="34"/>
                    </Grid.RowDefinitions>
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="96"/>
                        <ColumnDefinition Width="16"/>
                        <ColumnDefinition Width="*"/>
                        <ColumnDefinition Width="8"/>
                        <ColumnDefinition Width="84"/>
                    </Grid.ColumnDefinitions>

                    <TextBlock Grid.Row="0" Grid.ColumnSpan="5" Text="SETUP" Style="{StaticResource SectionTitle}"/>

                    <TextBlock Grid.Row="1" Grid.Column="0" Text="TF1 ISO" Style="{StaticResource PathLabel}"/>
                    <Rectangle x:Name="SetupTf1Dot" Grid.Row="1" Grid.Column="1" Width="6" Height="6" Fill="#3F3F46" VerticalAlignment="Center" HorizontalAlignment="Left"/>
                    <TextBox x:Name="SetupTf1Box" Grid.Row="1" Grid.Column="2"/>
                    <Button x:Name="SetupTf1BrowseBtn" Grid.Row="1" Grid.Column="4" Content="Browse..." Style="{StaticResource SecondaryButton}"/>

                    <TextBlock Grid.Row="2" Grid.Column="0" Text="TF3 ISO" Style="{StaticResource PathLabel}"/>
                    <Rectangle x:Name="SetupTf3Dot" Grid.Row="2" Grid.Column="1" Width="6" Height="6" Fill="#3F3F46" VerticalAlignment="Center" HorizontalAlignment="Left"/>
                    <TextBox x:Name="SetupTf3Box" Grid.Row="2" Grid.Column="2"/>
                    <Button x:Name="SetupTf3BrowseBtn" Grid.Row="2" Grid.Column="4" Content="Browse..." Style="{StaticResource SecondaryButton}"/>
                </Grid>
            </Border>

            <Border Style="{StaticResource CardBox}">
                <StackPanel>
                    <Grid Margin="0,0,0,8">
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="*"/>
                            <ColumnDefinition Width="Auto"/>
                        </Grid.ColumnDefinitions>
                        <TextBlock Text="BUILD" Style="{StaticResource SectionTitle}" Margin="0"/>
                        <TextBlock x:Name="SetupStepText" Grid.Column="1" Text="" FontSize="11" Foreground="{StaticResource TextSecondary}"/>
                    </Grid>

                    <Border Height="6" Background="#09090B" BorderBrush="#27272A" BorderThickness="1" Margin="0,0,0,10">
                        <ProgressBar x:Name="SetupProgress"
                                     Minimum="0" Maximum="100" Value="0"
                                     Background="Transparent"
                                     Foreground="#FFFFFF"
                                     BorderThickness="0"/>
                    </Border>

                    <TextBox x:Name="SetupLogBox"
                             Height="170"
                             Padding="8,6"
                             FontFamily="Consolas"
                             FontSize="11"
                             IsReadOnly="True"
                             AcceptsReturn="True"
                             TextWrapping="NoWrap"
                             VerticalContentAlignment="Top"
                             HorizontalScrollBarVisibility="Auto"
                             VerticalScrollBarVisibility="Auto"/>
                </StackPanel>
            </Border>

            <Grid Margin="0,4,0,0">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*"/>
                    <ColumnDefinition Width="Auto"/>
                </Grid.ColumnDefinitions>

                <TextBlock x:Name="SetupStatusText"
                           Grid.Column="0"
                           VerticalAlignment="Center"
                           TextWrapping="Wrap"
                           Margin="2,0,12,0"
                           FontSize="11.5"
                           Foreground="{StaticResource TextSecondary}"
                           Text=""/>

                <StackPanel Grid.Column="1" Orientation="Horizontal" VerticalAlignment="Center">
                    <Button x:Name="BackPlayBtn" Content="Play Mode" Style="{StaticResource SecondaryButton}" Height="32" Margin="0,0,8,0"/>
                    <Button x:Name="SetupCloseBtn" Content="Close" Style="{StaticResource SecondaryButton}" Height="32" Margin="0,0,8,0"/>
                    <Button x:Name="SetupBuildBtn" Content="Install &amp; Build" Style="{StaticResource PrimaryButton}"/>
                </StackPanel>
            </Grid>
        </StackPanel>
    </Grid>
</Window>
'@

$reader = New-Object System.Xml.XmlNodeReader $xaml
$window = [System.Windows.Markup.XamlReader]::Load($reader)

$window.Add_SourceInitialized({
    $hwnd = (New-Object System.Windows.Interop.WindowInteropHelper($window)).Handle
    [SetoDwmTheme]::ApplyDarkTitleBar($hwnd)
})

function Get-Ctrl($name) { return $window.FindName($name) }

$readyBadge = Get-Ctrl 'ReadyBadge'
$readyDot = Get-Ctrl 'ReadyDot'
$readyText = Get-Ctrl 'ReadyText'
$playView = Get-Ctrl 'PlayView'
$setupView = Get-Ctrl 'SetupView'

$presentBox = Get-Ctrl 'PresentBox'
$scaleBox = Get-Ctrl 'ScaleBox'
$filterBox = Get-Ctrl 'FilterBox'
$renderScaleBox = Get-Ctrl 'RenderScaleBox'
$msaaBox = Get-Ctrl 'MsaaBox'
$postFilterBox = Get-Ctrl 'PostFilterBox'
$renderScaleLabel = Get-Ctrl 'RenderScaleLabel'
$msaaLabel = Get-Ctrl 'MsaaLabel'
$postFilterLabel = Get-Ctrl 'PostFilterLabel'

$isoDot = Get-Ctrl 'IsoDot'
$isoBox = Get-Ctrl 'IsoBox'
$isoBrowseBtn = Get-Ctrl 'IsoBrowseBtn'

$flashDot = Get-Ctrl 'FlashDot'
$flashBox = Get-Ctrl 'FlashBox'
$flashBrowseBtn = Get-Ctrl 'FlashBrowseBtn'

$hdTexLabel = Get-Ctrl 'HdTexLabel'
$hdTexDot = Get-Ctrl 'HdTexDot'
$hdTexBox = Get-Ctrl 'HdTexBox'
$hdTexBrowseBtn = Get-Ctrl 'HdTexBrowseBtn'
$hdTexClearBtn = Get-Ctrl 'HdTexClearBtn'
$hdTexHint = Get-Ctrl 'HdTexHint'

$serverBox = Get-Ctrl 'ServerBox'
$nicknameBox = Get-Ctrl 'NicknameBox'
$statusText = Get-Ctrl 'StatusText'

$goSetupBtn = Get-Ctrl 'GoSetupBtn'
$resetBtn = Get-Ctrl 'ResetBtn'
$cancelBtn = Get-Ctrl 'CancelBtn'
$saveBtn = Get-Ctrl 'SaveBtn'
$startBtn = Get-Ctrl 'StartBtn'

$setupTf1Dot = Get-Ctrl 'SetupTf1Dot'
$setupTf1Box = Get-Ctrl 'SetupTf1Box'
$setupTf1BrowseBtn = Get-Ctrl 'SetupTf1BrowseBtn'
$setupTf3Dot = Get-Ctrl 'SetupTf3Dot'
$setupTf3Box = Get-Ctrl 'SetupTf3Box'
$setupTf3BrowseBtn = Get-Ctrl 'SetupTf3BrowseBtn'
$setupStepText = Get-Ctrl 'SetupStepText'
$setupProgress = Get-Ctrl 'SetupProgress'
$setupLogBox = Get-Ctrl 'SetupLogBox'
$setupStatusText = Get-Ctrl 'SetupStatusText'
$backPlayBtn = Get-Ctrl 'BackPlayBtn'
$setupCloseBtn = Get-Ctrl 'SetupCloseBtn'
$setupBuildBtn = Get-Ctrl 'SetupBuildBtn'

foreach ($k in $scaleLabels.Keys) { [void]$scaleBox.Items.Add($k) }
foreach ($k in $filterLabels.Keys) { [void]$filterBox.Items.Add($k) }
foreach ($k in $presentLabels.Keys) { [void]$presentBox.Items.Add($k) }
foreach ($k in $renderScaleLabels.Keys) { [void]$renderScaleBox.Items.Add($k) }
foreach ($k in $msaaLabels.Keys) { [void]$msaaBox.Items.Add($k) }
foreach ($k in $postFilterLabels.Keys) { [void]$postFilterBox.Items.Add($k) }

function Set-ColorBrush($hex) {
    $converter = New-Object System.Windows.Media.BrushConverter
    return $converter.ConvertFromString($hex)
}

$brushOk = Set-ColorBrush '#FFFFFF'
$brushWarn = Set-ColorBrush '#A1A1AA'
$brushErr = Set-ColorBrush '#E4E4E7'
$brushMuted = Set-ColorBrush '#3F3F46'
$brushTextSec = Set-ColorBrush '#A1A1AA'
$brushTextMuted = Set-ColorBrush '#52525B'

$script:setupRunner = $null
$script:setupTimer = $null
$script:currentMode = 'play'

function Apply-ConfigToUi($c) {
    $scaleLabel = ($scaleLabels.Keys | Where-Object { $scaleLabels[$_] -eq [string]$c.scale } | Select-Object -First 1)
    $filterLabel = ($filterLabels.Keys | Where-Object { $filterLabels[$_] -eq [string]$c.filter } | Select-Object -First 1)
    $presentLabel = ($presentLabels.Keys | Where-Object { $presentLabels[$_] -eq [string]$c.present } | Select-Object -First 1)
    $renderScaleLabelVal = ($renderScaleLabels.Keys | Where-Object { $renderScaleLabels[$_] -eq [string]$c.render_scale } | Select-Object -First 1)
    $msaaLabelVal = ($msaaLabels.Keys | Where-Object { $msaaLabels[$_] -eq [string]$c.msaa } | Select-Object -First 1)
    $postFilterLabelVal = ($postFilterLabels.Keys | Where-Object { $postFilterLabels[$_] -eq [string]$c.post_filter } | Select-Object -First 1)

    $scaleBox.SelectedItem = if ($scaleLabel) { $scaleLabel } else { '960 x 544 (2x)' }
    $filterBox.SelectedItem = if ($filterLabel) { $filterLabel } else { 'Nearest' }
    $presentBox.SelectedItem = if ($presentLabel) { $presentLabel } else { 'GDI' }
    $renderScaleBox.SelectedItem = if ($renderScaleLabelVal) { $renderScaleLabelVal } else { 'Auto' }
    $msaaBox.SelectedItem = if ($msaaLabelVal) { $msaaLabelVal } else { 'Off' }
    $postFilterBox.SelectedItem = if ($postFilterLabelVal) { $postFilterLabelVal } else { 'Off' }

    $isoBox.Text = [string]$c.iso
    $flashBox.Text = [string]$c.flash0
    $hdTexBox.Text = [string]$c.hd_tex_dir
    $serverBox.Text = [string]$c.server
    $nicknameBox.Text = [string]$c.nickname

    $tf1Guess = if (![string]::IsNullOrWhiteSpace($c.iso) -and (Test-Path -LiteralPath (Resolve-RepoPath $c.iso) -PathType Leaf)) { [string]$c.iso } `
                elseif (Test-Path -LiteralPath (Join-Path $repoRoot 'tagforce iso/TF1.iso') -PathType Leaf) { Join-Path $repoRoot 'tagforce iso/TF1.iso' } `
                elseif (Test-Path -LiteralPath (Join-Path $repoRoot 'TF1.iso') -PathType Leaf) { Join-Path $repoRoot 'TF1.iso' } `
                else { '' }
    $tf3Guess = if (![string]::IsNullOrWhiteSpace($c.tf3_iso) -and (Test-Path -LiteralPath (Resolve-RepoPath $c.tf3_iso) -PathType Leaf)) { [string]$c.tf3_iso } `
                elseif (Test-Path -LiteralPath (Join-Path $repoRoot 'tagforce iso/TF3.iso') -PathType Leaf) { Join-Path $repoRoot 'tagforce iso/TF3.iso' } `
                elseif (Test-Path -LiteralPath (Join-Path $repoRoot 'TF3.iso') -PathType Leaf) { Join-Path $repoRoot 'TF3.iso' } `
                else { '' }
    $setupTf1Box.Text = $tf1Guess
    $setupTf3Box.Text = $tf3Guess
}

function Update-ConfigFromUi {
    $cfg.scale = $scaleLabels[[string]$scaleBox.SelectedItem]
    $cfg.filter = $filterLabels[[string]$filterBox.SelectedItem]
    $cfg.present = $presentLabels[[string]$presentBox.SelectedItem]
    $cfg.render_scale = $renderScaleLabels[[string]$renderScaleBox.SelectedItem]
    $cfg.msaa = $msaaLabels[[string]$msaaBox.SelectedItem]
    $cfg.post_filter = $postFilterLabels[[string]$postFilterBox.SelectedItem]
    $cfg.iso = $isoBox.Text.Trim()
    $cfg.eboot = 'build/reloc/elf/eboot-reloc.elf'
    $cfg.flash0 = $flashBox.Text.Trim()
    $cfg.hd_tex_dir = $hdTexBox.Text.Trim()
    $cfg.server = $serverBox.Text.Trim()
    $cfg.nickname = $nicknameBox.Text.Trim()
    if (![string]::IsNullOrWhiteSpace($setupTf3Box.Text)) {
        $cfg.tf3_iso = $setupTf3Box.Text.Trim()
    }
}

function Refresh-PlayState {
    $presentVal = $presentLabels[[string]$presentBox.SelectedItem]
    $isGl = ($presentVal -eq 'ged')
    $isGed = ($presentVal -eq 'ged')

    $postFilterBox.IsEnabled = $isGl
    $postFilterLabel.Foreground = if ($isGl) { $brushTextSec } else { $brushTextMuted }

    $renderScaleBox.IsEnabled = $isGed
    $msaaBox.IsEnabled = $isGed
    $hdTexBox.IsEnabled = $isGed
    $hdTexBrowseBtn.IsEnabled = $isGed
    $hdTexClearBtn.IsEnabled = $isGed
    $renderScaleLabel.Foreground = if ($isGed) { $brushTextSec } else { $brushTextMuted }
    $msaaLabel.Foreground = if ($isGed) { $brushTextSec } else { $brushTextMuted }
    $hdTexLabel.Foreground = if ($isGed) { $brushTextSec } else { $brushTextMuted }

    $missing = @()
    $isoPath = Resolve-RepoPath $isoBox.Text.Trim()
    $isoOk = ![string]::IsNullOrWhiteSpace($isoPath) -and (Test-Path -LiteralPath $isoPath -PathType Leaf)
    $isoDot.Fill = if ($isoOk) { $brushOk } else { $brushMuted }
    if (-not $isoOk) { $missing += 'TF1 ISO' }

    $ebootPath = Resolve-RepoPath 'build/reloc/elf/eboot-reloc.elf'
    $ebootOk = Test-Path -LiteralPath $ebootPath -PathType Leaf
    if (-not $ebootOk) { $missing += 'EBOOT' }

    $flashPath = Resolve-RepoPath $flashBox.Text.Trim()
    $flashOk = ![string]::IsNullOrWhiteSpace($flashPath) -and (Test-Path -LiteralPath $flashPath -PathType Container)
    $flashDot.Fill = if ($flashOk) { $brushOk } else { $brushMuted }
    if (-not $flashOk) { $missing += 'flash0' }

    $exePath = Join-Path $repoRoot 'build/out/Seto-static.exe'
    $exeOk = (Test-Path -LiteralPath $exePath -PathType Leaf) -and ((Get-Item -LiteralPath $exePath).Length -gt 1000000)
    if (-not $exeOk) { $missing += 'Seto-static.exe' }

    $prxMissing = 0
    foreach ($p in $requiredPrxs) {
        if (!(Test-Path -LiteralPath (Resolve-RepoPath $p) -PathType Leaf)) { $prxMissing++ }
    }
    if ($prxMissing -gt 0) { $missing += "$prxMissing PRX(s)" }

    $hdPath = Resolve-RepoPath $hdTexBox.Text.Trim()
    if ([string]::IsNullOrWhiteSpace($hdPath)) {
        $hdTexDot.Fill = $brushMuted
        $hdTexHint.Foreground = $brushTextMuted
        $hdTexHint.Text = 'Disabled'
    } elseif (Test-Path -LiteralPath (Join-Path $hdPath 'textures.ini') -PathType Leaf) {
        $hdTexDot.Fill = if ($isGed) { $brushOk } else { $brushMuted }
        $hdTexHint.Foreground = if ($isGed) { $brushTextSec } else { $brushTextMuted }
        $hdTexHint.Text = 'textures.ini OK'
    } else {
        $hdTexDot.Fill = $brushMuted
        $hdTexHint.Foreground = $brushTextSec
        $hdTexHint.Text = 'textures.ini not found'
    }

    if ($script:currentMode -eq 'play') {
        if ($missing.Count -eq 0) {
            $readyBadge.BorderBrush = Set-ColorBrush '#2E2E33'
            $readyDot.Fill = $brushOk
            $readyText.Foreground = Set-ColorBrush '#E4E4E7'
            $readyText.Text = 'Ready'
            $readyBadge.ToolTip = 'All required files detected.'
        } else {
            $readyBadge.BorderBrush = Set-ColorBrush '#52525B'
            $readyDot.Fill = $brushMuted
            $readyText.Foreground = Set-ColorBrush '#A1A1AA'
            $readyText.Text = "Missing: $($missing.Count)"
            $readyBadge.ToolTip = 'Missing: ' + ($missing -join ', ')
        }
    }
}

function Refresh-SetupState {
    $tf1 = Resolve-RepoPath $setupTf1Box.Text.Trim()
    $tf3 = Resolve-RepoPath $setupTf3Box.Text.Trim()

    $tf1Ok = ![string]::IsNullOrWhiteSpace($tf1) -and (Test-Path -LiteralPath $tf1 -PathType Leaf)
    $tf3Ok = ![string]::IsNullOrWhiteSpace($tf3) -and (Test-Path -LiteralPath $tf3 -PathType Leaf)

    $setupTf1Dot.Fill = if ($tf1Ok) { $brushOk } else { $brushMuted }
    $setupTf3Dot.Fill = if ($tf3Ok) { $brushOk } else { $brushMuted }

    $running = $null -ne $script:setupRunner
    $installed = Test-Installed $cfg
    $backPlayBtn.Visibility = if ($installed) { [System.Windows.Visibility]::Visible } else { [System.Windows.Visibility]::Collapsed }
    $backPlayBtn.IsEnabled = (-not $running) -and $installed
    $setupBuildBtn.IsEnabled = (-not $running) -and $tf1Ok -and $tf3Ok

    if ($script:currentMode -eq 'setup') {
        $readyBadge.BorderBrush = Set-ColorBrush '#2E2E33'
        $readyDot.Fill = if ($running) { $brushOk } elseif ($installed) { $brushOk } else { $brushMuted }
        $readyText.Foreground = Set-ColorBrush '#E4E4E7'
        $readyText.Text = if ($running) { 'Building' } elseif ($installed) { 'Installed' } else { 'Setup' }
        $readyBadge.ToolTip = $null
    }
}

function Switch-Mode([string]$mode) {
    $script:currentMode = $mode
    if ($mode -eq 'setup') {
        $playView.Visibility = [System.Windows.Visibility]::Collapsed
        $setupView.Visibility = [System.Windows.Visibility]::Visible
        $startBtn.IsDefault = $false
        $setupBuildBtn.IsDefault = $true
        Refresh-SetupState
    } else {
        $setupView.Visibility = [System.Windows.Visibility]::Collapsed
        $playView.Visibility = [System.Windows.Visibility]::Visible
        $setupBuildBtn.IsDefault = $false
        $startBtn.IsDefault = $true
        Refresh-PlayState
    }
}

$pumpUi = {
    $frame = New-Object System.Windows.Threading.DispatcherFrame
    $null = $window.Dispatcher.BeginInvoke(
        [System.Windows.Threading.DispatcherPriority]::Background,
        [System.Threading.SendOrPostCallback]{ param($f) $f.Continue = $false },
        $frame
    )
    [System.Windows.Threading.Dispatcher]::PushFrame($frame)
}

function Set-PlayBusy([bool]$busy) {
    $startBtn.IsEnabled = -not $busy
    $saveBtn.IsEnabled = -not $busy
    $resetBtn.IsEnabled = -not $busy
    $goSetupBtn.IsEnabled = -not $busy
    $cancelBtn.IsEnabled = -not $busy
}

function Pick-IsoFile([string]$title, [string]$current) {
    $dlg = New-Object Microsoft.Win32.OpenFileDialog
    $dlg.Title = $title
    $dlg.Filter = 'PSP ISO (*.iso)|*.iso|All files (*.*)|*.*'
    $cur = Resolve-RepoPath $current
    if (![string]::IsNullOrWhiteSpace($cur)) {
        $dir = Split-Path -Path $cur -Parent
        if ($dir -and (Test-Path -LiteralPath $dir -PathType Container)) { $dlg.InitialDirectory = $dir }
    }
    if ($dlg.ShowDialog($window) -eq $true) { return $dlg.FileName }
    return $null
}

function Pick-Folder([string]$desc, [string]$current) {
    $picker = New-Object System.Windows.Forms.FolderBrowserDialog
    $picker.Description = $desc
    $cur = Resolve-RepoPath $current
    $picker.SelectedPath = if ($cur -and (Test-Path -LiteralPath $cur -PathType Container)) { $cur } else { $repoRoot }
    $picker.ShowNewFolderButton = $false
    try {
        if ($picker.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            return $picker.SelectedPath
        }
        return $null
    } finally {
        $picker.Dispose()
    }
}

Apply-ConfigToUi $cfg
$startMode = if ($Setup -or -not (Test-Installed $cfg)) { 'setup' } else { 'play' }
Switch-Mode $startMode

$presentBox.Add_SelectionChanged({ Refresh-PlayState })
$isoBox.Add_TextChanged({ Refresh-PlayState })
$flashBox.Add_TextChanged({ Refresh-PlayState })
$hdTexBox.Add_TextChanged({ Refresh-PlayState })

$setupTf1Box.Add_TextChanged({ Refresh-SetupState })
$setupTf3Box.Add_TextChanged({ Refresh-SetupState })

$isoBrowseBtn.Add_Click({
    $p = Pick-IsoFile 'Select TF1 ISO image' $isoBox.Text.Trim()
    if ($p) { $isoBox.Text = $p }
})
$flashBrowseBtn.Add_Click({
    $p = Pick-Folder 'Choose the PPSSPP flash0 folder' $flashBox.Text.Trim()
    if ($p) { $flashBox.Text = $p }
})
$hdTexBrowseBtn.Add_Click({
    $p = Pick-Folder 'Choose the folder containing textures.ini' $hdTexBox.Text.Trim()
    if ($p) { $hdTexBox.Text = $p }
})
$hdTexClearBtn.Add_Click({ $hdTexBox.Text = '' })

$setupTf1BrowseBtn.Add_Click({
    $p = Pick-IsoFile 'Select TF1 ISO (ULUS-10136)' $setupTf1Box.Text.Trim()
    if ($p) { $setupTf1Box.Text = $p }
})
$setupTf3BrowseBtn.Add_Click({
    $p = Pick-IsoFile 'Select TF3 ISO (ULES-01183)' $setupTf3Box.Text.Trim()
    if ($p) { $setupTf3Box.Text = $p }
})

$goSetupBtn.Add_Click({
    if ([string]::IsNullOrWhiteSpace($setupTf1Box.Text) -and ![string]::IsNullOrWhiteSpace($isoBox.Text)) {
        $setupTf1Box.Text = $isoBox.Text.Trim()
    }
    Switch-Mode 'setup'
})

$backPlayBtn.Add_Click({
    Switch-Mode 'play'
})

$resetBtn.Add_Click({
    Apply-ConfigToUi $defaults
    Refresh-PlayState
    $statusText.Foreground = $brushWarn
    $statusText.Text = 'Defaults loaded.'
})

$saveBtn.Add_Click({
    try {
        Update-ConfigFromUi
        Save-Config $cfg
        $statusText.Foreground = $brushOk
        $statusText.Text = 'Saved to launcher.ini'
    } catch {
        $statusText.Foreground = $brushErr
        $statusText.Text = $_.Exception.Message
    }
})

$startBtn.Add_Click({
    try {
        Update-ConfigFromUi
        Save-Config $cfg
        Set-PlayBusy $true
        $statusText.Foreground = $brushWarn
        $statusText.Text = 'Starting Seto-static...'
        & $pumpUi
        Start-Game $cfg -UiPump $pumpUi
        $window.Close()
    } catch {
        Set-PlayBusy $false
        $statusText.Foreground = $brushErr
        $statusText.Text = $_.Exception.Message
    }
})

$setupBuildBtn.Add_Click({
    if ($null -ne $script:setupRunner) { return }

    try {
        $tf1 = Resolve-RepoPath $setupTf1Box.Text.Trim()
        $tf3 = Resolve-RepoPath $setupTf3Box.Text.Trim()

        if (!(Test-Path -LiteralPath $tf1 -PathType Leaf) -or !(Test-Path -LiteralPath $tf3 -PathType Leaf)) {
            $setupStatusText.Foreground = $brushErr
            $setupStatusText.Text = 'Select both TF1.iso and TF3.iso.'
            return
        }

        $pyExe = $null
        $pyCandidates = @(Get-Command python -All -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -and ($_.Source -notlike '*\WindowsApps\*') } |
            Select-Object -ExpandProperty Source -Unique)
        $prevEap = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            foreach ($cand in $pyCandidates) {
                & $cand -c "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('PIL') else 1)" *>$null
                if ($LASTEXITCODE -eq 0) { $pyExe = $cand; break }
            }
        } finally {
            $ErrorActionPreference = $prevEap
        }
        if (-not $pyExe -and $pyCandidates.Count -gt 0) { $pyExe = $pyCandidates[0] }
        if (-not $pyExe) {
            $setupStatusText.Foreground = $brushErr
            $setupStatusText.Text = 'Python not found in PATH.'
            return
        }

        $cfg.iso = $setupTf1Box.Text.Trim()
        $cfg.tf3_iso = $setupTf3Box.Text.Trim()
        if ([string]::IsNullOrWhiteSpace($cfg.flash0)) { $cfg.flash0 = 'build/flash0' }
        $cfg.eboot = 'build/reloc/elf/eboot-reloc.elf'
        Save-Config $cfg

        $setupLogBox.Clear()
        $setupProgress.Value = 2
        $setupStepText.Text = 'Starting...'
        $setupStatusText.Foreground = $brushWarn
        $setupStatusText.Text = 'Installing...'

        $script:setupStartTime = Get-Date
        $script:setupLastLineTime = Get-Date
        $script:setupMajorStep = 0
        $script:setupMajorLabel = 'Starting...'
        $script:setupSubLabel = ''
        $script:setupTickCount = 0

        $setupScript = Join-Path $repoRoot 'tools\setup_build.py'
        $setupArgs = '-u "{0}" --tf1-iso "{1}" --tf3-iso "{2}"' -f $setupScript, $tf1, $tf3
        $script:setupRunner = New-Object SetoSetupRunner($pyExe, $setupArgs, $repoRoot)
        Refresh-SetupState

        $script:setupTimer = New-Object System.Windows.Threading.DispatcherTimer
        $script:setupTimer.Interval = [TimeSpan]::FromMilliseconds(100)
        $script:setupTimer.Add_Tick({
            $script:setupTickCount++
            $line = $null
            $added = $false
            while ($script:setupRunner -and $script:setupRunner.TryDequeue([ref]$line)) {
                $setupLogBox.AppendText($line + "`r`n")
                $added = $true
                $script:setupLastLineTime = Get-Date

                if ($line -match '^\[(\d+)/7\]\s*(.+)$') {
                    $cur = [int]$Matches[1]
                    $script:setupMajorStep = $cur
                    $script:setupMajorLabel = $Matches[2].Trim()
                    $script:setupSubLabel = ''
                    $setupStepText.Text = $script:setupMajorLabel
                    # Map stages 1..6 across 5..45%, stage 7 starts at 45%.
                    $pct = if ($cur -lt 7) { [int](5 + ($cur - 1) * 40 / 6) } else { 45 }
                    if ($pct -gt $setupProgress.Value) { $setupProgress.Value = $pct }
                } elseif ($line -match '^\[(\d+)/(\d+)\]\s+(Building|Linking|Generating|Compiling)\b') {
                    $cur = [int]$Matches[1]
                    $tot = [int]$Matches[2]
                    if ($tot -gt 0) {
                        $script:setupSubLabel = " ($cur/$tot)"
                        $setupStepText.Text = "$($script:setupMajorLabel)$($script:setupSubLabel)"
                        if ($script:setupMajorStep -ge 7) {
                            $pct = [int](45 + ($cur * 53.0 / $tot))
                            $setupProgress.Value = [Math]::Min(98, [Math]::Max([int]$setupProgress.Value, $pct))
                        }
                    }
                }
            }
            if ($added) { $setupLogBox.ScrollToEnd() }

            if ($script:setupRunner -and -not $script:setupRunner.HasExited -and ($script:setupTickCount % 4 -eq 0)) {
                $elapsed = (Get-Date) - $script:setupStartTime
                $idleSec = ((Get-Date) - $script:setupLastLineTime).TotalSeconds
                $dots = '.' * ((($script:setupTickCount / 4) % 3) + 1)
                $elStr = '{0:mm\:ss}' -f $elapsed
                if ($idleSec -gt 5) {
                    $setupStatusText.Text = "Working (compiling/linking)$dots ($elStr)"
                } else {
                    $setupStatusText.Text = "Installing$dots ($elStr)"
                }
            }

            if ($script:setupRunner -and $script:setupRunner.HasExited) {
                while ($script:setupRunner.TryDequeue([ref]$line)) {
                    $setupLogBox.AppendText($line + "`r`n")
                }
                $setupLogBox.ScrollToEnd()

                $code = $script:setupRunner.ExitCode
                $script:setupTimer.Stop()
                $script:setupRunner.Dispose()
                $script:setupRunner = $null

                if ($code -eq 0 -and (Test-Installed $cfg)) {
                    $elapsed = (Get-Date) - $script:setupStartTime
                    $elStr = '{0:mm\:ss}' -f $elapsed
                    $setupProgress.Value = 100
                    $setupStepText.Text = "Complete ($elStr)"
                    $setupStatusText.Foreground = $brushOk
                    $setupStatusText.Text = "Build complete ($elStr)."
                    $isoBox.Text = $setupTf1Box.Text.Trim()
                    if ([string]::IsNullOrWhiteSpace($flashBox.Text) -or
                        !(Test-Path -LiteralPath (Join-Path (Resolve-RepoPath $flashBox.Text.Trim()) 'font/ltn0.pgf') -PathType Leaf)) {
                        $flashBox.Text = 'build/flash0'
                        $cfg.flash0 = 'build/flash0'
                        Save-Config $cfg
                    }
                    Refresh-SetupState
                    Switch-Mode 'play'
                    $statusText.Foreground = $brushOk
                    $statusText.Text = 'Setup finished.'
                } else {
                    $setupStepText.Text = "Failed (exit $code)"
                    $setupStatusText.Foreground = $brushErr
                    $setupStatusText.Text = 'Setup failed. Check log.'
                    Refresh-SetupState
                }
            }
        })
        $script:setupTimer.Start()
    } catch {
        $setupStatusText.Foreground = $brushErr
        $setupStatusText.Text = $_.Exception.Message
        $setupLogBox.AppendText("ERROR: $($_.Exception.Message)`r`n")
        $setupLogBox.ScrollToEnd()
        Refresh-SetupState
    }
})

$cancelBtn.Add_Click({ $window.Close() })
$setupCloseBtn.Add_Click({ $window.Close() })
$window.Add_Closing({
    if ($script:setupTimer) { $script:setupTimer.Stop() }
    if ($script:setupRunner) {
        $script:setupRunner.Kill()
        $script:setupRunner.Dispose()
        $script:setupRunner = $null
    }
})

[void]$window.ShowDialog()
