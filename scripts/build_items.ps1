param(
  [Parameter(Mandatory=$true)][string]$ItemsJson,
  [Parameter(Mandatory=$true)][string]$ItemsZip,
  [Parameter(Mandatory=$true)][string]$ItemsHeader
)

$ErrorActionPreference = "Stop"

if (!(Test-Path -LiteralPath $ItemsJson)) {
  throw "items.json not found: $ItemsJson"
}

$zipDir = Split-Path -Parent $ItemsZip
if ($zipDir -and !(Test-Path -LiteralPath $zipDir)) {
  New-Item -ItemType Directory -Path $zipDir | Out-Null
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("tbh-items-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
  Copy-Item -LiteralPath $ItemsJson -Destination (Join-Path $tmp "items.json")
  if (Test-Path -LiteralPath $ItemsZip) {
    Remove-Item -LiteralPath $ItemsZip -Force
  }
  Compress-Archive -LiteralPath (Join-Path $tmp "items.json") -DestinationPath $ItemsZip -CompressionLevel Optimal
} finally {
  if (Test-Path -LiteralPath $tmp) {
    Remove-Item -LiteralPath $tmp -Recurse -Force
  }
}

$sha1 = [System.Security.Cryptography.SHA1]::Create()
$bytes = [IO.File]::ReadAllBytes($ItemsZip)
$sha = ([BitConverter]::ToString($sha1.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()

$header = @"
#pragma once

constexpr wchar_t EMBEDDED_ITEMS_SHA1[] = L"$sha";
"@

[IO.File]::WriteAllText($ItemsHeader, $header, [Text.Encoding]::ASCII)
