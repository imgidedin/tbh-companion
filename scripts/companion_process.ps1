[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$ExePath,

  [switch]$Stop,
  [switch]$Start,
  [int]$WaitMilliseconds = 700
)

$ErrorActionPreference = "Stop"

$ResolvedExePath = if (Test-Path -LiteralPath $ExePath) {
  (Resolve-Path -LiteralPath $ExePath).Path
} else {
  [System.IO.Path]::GetFullPath($ExePath)
}

function Get-CompanionProcess {
  Get-CimInstance Win32_Process -Filter "name = 'TBH_Companion.exe'" |
    Where-Object {
      $_.ExecutablePath -and
      [string]::Equals($_.ExecutablePath, $ResolvedExePath, [System.StringComparison]::OrdinalIgnoreCase)
    }
}

if ($Stop) {
  $processes = @(Get-CompanionProcess)
  foreach ($process in $processes) {
    Stop-Process -Id $process.ProcessId -Force
  }

  $deadline = [DateTime]::UtcNow.AddMilliseconds($WaitMilliseconds)
  do {
    $remaining = @(Get-CompanionProcess)
    if ($remaining.Count -eq 0) { break }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)

  $remaining = @(Get-CompanionProcess)
  if ($remaining.Count -gt 0) {
    throw "Nao foi possivel encerrar $($remaining.Count) instancia(s) em $ResolvedExePath."
  }
}

if ($Start) {
  if (-not (Test-Path -LiteralPath $ResolvedExePath)) {
    throw "Executavel nao encontrado: $ResolvedExePath"
  }
  Start-Process -FilePath $ResolvedExePath -WindowStyle Hidden | Out-Null
}
