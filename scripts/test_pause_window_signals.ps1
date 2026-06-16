param(
  [string]$ProcessName = "TaskBarHero",
  [string]$SavePath = (Join-Path $env:USERPROFILE "AppData\LocalLow\TesseractStudio\TaskbarHero\SaveFile_Live.es3"),
  [int]$PauseMs = 1,
  [int]$WaitAfterActionSec = 30,
  [int]$PollMs = 250,
  [int]$Rounds = 1,
  [switch]$IncludePowerBroadcast,
  [switch]$ListActions,
  [switch]$VerbosePolling
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Fail($message) {
  Write-Host "ERRO: $message" -ForegroundColor Red
  exit 1
}

function Get-SaveState([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) {
    return [pscustomobject]@{
      Exists = $false
      LastWriteUtc = $null
      Length = 0
    }
  }

  $item = Get-Item -LiteralPath $path
  [pscustomobject]@{
    Exists = $true
    LastWriteUtc = $item.LastWriteTimeUtc
    Length = $item.Length
  }
}

function Format-SaveState($state) {
  if (-not $state.Exists) { return "missing" }
  return ("{0:o}, {1:N0} bytes" -f $state.LastWriteUtc, $state.Length)
}

function Test-SaveChanged($before, $after) {
  if ($before.Exists -ne $after.Exists) { return $true }
  if (-not $before.Exists -and -not $after.Exists) { return $false }
  return ($before.LastWriteUtc -ne $after.LastWriteUtc) -or ($before.Length -ne $after.Length)
}

function Wait-SaveChange([string]$path, $baseline, [int]$timeoutSec, [int]$pollMs, [switch]$verbosePolling) {
  $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
  while ($stopwatch.Elapsed.TotalSeconds -lt $timeoutSec) {
    Start-Sleep -Milliseconds $pollMs
    $current = Get-SaveState $path
    if ($verbosePolling) {
      Write-Host ("    poll {0,6:N2}s: {1}" -f $stopwatch.Elapsed.TotalSeconds, (Format-SaveState $current))
    }
    if (Test-SaveChanged $baseline $current) {
      return [pscustomobject]@{
        Changed = $true
        ElapsedMs = $stopwatch.ElapsedMilliseconds
        State = $current
      }
    }
  }

  [pscustomobject]@{
    Changed = $false
    ElapsedMs = $stopwatch.ElapsedMilliseconds
    State = Get-SaveState $path
  }
}

if (-not ("TbhWin32PauseProbe" -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class TbhWin32PauseProbe {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

  [DllImport("user32.dll")]
  public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

  [DllImport("user32.dll")]
  public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

  [DllImport("user32.dll")]
  public static extern bool IsWindowVisible(IntPtr hWnd);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);

  [DllImport("user32.dll")]
  public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);

  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hWnd);

  [DllImport("user32.dll")]
  public static extern IntPtr SendMessageTimeout(
    IntPtr hWnd,
    uint msg,
    UIntPtr wParam,
    IntPtr lParam,
    uint flags,
    uint timeout,
    out UIntPtr result);

  public static IntPtr FindWindowForPid(uint targetPid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr hWnd, IntPtr lParam) {
      uint pid;
      GetWindowThreadProcessId(hWnd, out pid);
      if (pid != targetPid || !IsWindowVisible(hWnd)) {
        return true;
      }

      StringBuilder title = new StringBuilder(512);
      GetWindowText(hWnd, title, title.Capacity);
      if (title.Length == 0) {
        return true;
      }

      found = hWnd;
      return false;
    }, IntPtr.Zero);
    return found;
  }
}
"@
}

$WM_ACTIVATEAPP = 0x001C
$WM_SYSCOMMAND = 0x0112
$WM_POWERBROADCAST = 0x0218
$SC_MINIMIZE = 0xF020
$SC_RESTORE = 0xF120
$PBT_APMSUSPEND = 0x0004
$PBT_APMRESUMEAUTOMATIC = 0x0012
$PBT_APMRESUMESUSPEND = 0x0007
$SMTO_ABORTIFHUNG = 0x0002
$SW_MINIMIZE = 6
$SW_RESTORE = 9

function Send-WindowMessage([IntPtr]$hwnd, [uint32]$message, [uint64]$wparam, [int64]$lparam = 0, [int]$timeoutMs = 1000) {
  $result = [UIntPtr]::Zero
  [void][TbhWin32PauseProbe]::SendMessageTimeout(
    $hwnd,
    $message,
    [UIntPtr]::new($wparam),
    [IntPtr]::new($lparam),
    [uint32]$SMTO_ABORTIFHUNG,
    [uint32]$timeoutMs,
    [ref]$result)
}

$actions = @(
  [pscustomobject]@{
    Name = "WM_ACTIVATEAPP false/true"
    Body = {
      Send-WindowMessage $script:hwnd ([uint32]$script:WM_ACTIVATEAPP) 0
      Start-Sleep -Milliseconds $script:PauseMs
      Send-WindowMessage $script:hwnd ([uint32]$script:WM_ACTIVATEAPP) 1
      [void][TbhWin32PauseProbe]::SetForegroundWindow($script:hwnd)
    }
  },
  [pscustomobject]@{
    Name = "ShowWindowAsync minimize/restore"
    Body = {
      [void][TbhWin32PauseProbe]::ShowWindowAsync($script:hwnd, $script:SW_MINIMIZE)
      Start-Sleep -Milliseconds $script:PauseMs
      [void][TbhWin32PauseProbe]::ShowWindowAsync($script:hwnd, $script:SW_RESTORE)
      [void][TbhWin32PauseProbe]::SetForegroundWindow($script:hwnd)
    }
  },
  [pscustomobject]@{
    Name = "WM_SYSCOMMAND SC_MINIMIZE/SC_RESTORE"
    Body = {
      Send-WindowMessage $script:hwnd ([uint32]$script:WM_SYSCOMMAND) ([uint64]$script:SC_MINIMIZE)
      Start-Sleep -Milliseconds $script:PauseMs
      Send-WindowMessage $script:hwnd ([uint32]$script:WM_SYSCOMMAND) ([uint64]$script:SC_RESTORE)
      [void][TbhWin32PauseProbe]::SetForegroundWindow($script:hwnd)
    }
  }
)

if ($IncludePowerBroadcast) {
  $actions += [pscustomobject]@{
    Name = "WM_POWERBROADCAST suspend/resume"
    Body = {
      Send-WindowMessage $script:hwnd ([uint32]$script:WM_POWERBROADCAST) ([uint64]$script:PBT_APMSUSPEND)
      Start-Sleep -Milliseconds $script:PauseMs
      Send-WindowMessage $script:hwnd ([uint32]$script:WM_POWERBROADCAST) ([uint64]$script:PBT_APMRESUMEAUTOMATIC)
      Send-WindowMessage $script:hwnd ([uint32]$script:WM_POWERBROADCAST) ([uint64]$script:PBT_APMRESUMESUSPEND)
      [void][TbhWin32PauseProbe]::SetForegroundWindow($script:hwnd)
    }
  }
}

if ($ListActions) {
  Write-Host "Acoes disponiveis:"
  foreach ($action in $actions) {
    Write-Host "  - $($action.Name)"
  }
  Write-Host ""
  Write-Host "Observacao: este script nao chama IL2CPP e nao escreve memoria do jogo."
  exit 0
}

if ($PauseMs -lt 0) { Fail "-PauseMs precisa ser >= 0." }
if ($WaitAfterActionSec -lt 1) { Fail "-WaitAfterActionSec precisa ser >= 1." }
if ($PollMs -lt 50) { Fail "-PollMs precisa ser >= 50." }
if ($Rounds -lt 1) { Fail "-Rounds precisa ser >= 1." }
if (-not (Test-Path -LiteralPath $SavePath)) { Fail "Save nao encontrado: $SavePath" }

$processes = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
if ($processes.Count -eq 0) { Fail "Processo nao encontrado: $ProcessName" }

$process = $processes | Sort-Object StartTime -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $process) { $process = $processes[0] }

$script:hwnd = [IntPtr]::Zero
if ($process.MainWindowHandle -and $process.MainWindowHandle -ne 0) {
  $script:hwnd = [IntPtr]$process.MainWindowHandle
} else {
  $script:hwnd = [TbhWin32PauseProbe]::FindWindowForPid([uint32]$process.Id)
}

if ($script:hwnd -eq [IntPtr]::Zero) {
  Fail "Nao encontrei janela visivel para PID $($process.Id)."
}

Write-Host "TaskBarHero pause/window signal probe"
Write-Host "  processo: $($process.ProcessName) pid=$($process.Id)"
Write-Host "  hwnd:     0x$($script:hwnd.ToInt64().ToString('X'))"
Write-Host "  save:     $SavePath"
Write-Host "  pause:    ${PauseMs}ms"
Write-Host "  espera:   ${WaitAfterActionSec}s por acao"
Write-Host ""
Write-Host "Importante: isto testa sinais de janela/OS. Nao e uma chamada real a bal.OnApplicationPause(bool)."
Write-Host ""

for ($round = 1; $round -le $Rounds; $round++) {
  Write-Host "Round $round/$Rounds"
  foreach ($action in $actions) {
    $before = Get-SaveState $SavePath
    Write-Host "  Acao: $($action.Name)"
    Write-Host "    antes:  $(Format-SaveState $before)"

    & $action.Body

    $result = Wait-SaveChange $SavePath $before $WaitAfterActionSec $PollMs -verbosePolling:$VerbosePolling
    Write-Host "    depois: $(Format-SaveState $result.State)"
    if ($result.Changed) {
      Write-Host ("    resultado: mudou apos {0:N2}s" -f ($result.ElapsedMs / 1000.0)) -ForegroundColor Green
    } else {
      Write-Host "    resultado: sem mudanca" -ForegroundColor Yellow
    }
  }
  Write-Host ""
}
