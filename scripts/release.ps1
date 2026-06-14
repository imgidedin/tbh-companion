<#
.SYNOPSIS
  Build + release do TBH Companion: recalcula o mapa IL2CPP, compila, versiona
  pela versao do jogo e publica um GitHub Release com o .exe para download.

.DESCRIPTION
  Etapas:
    1. Recalcula o mapa IL2CPP (scripts\refresh_il2cpp_map.py) para a versao
       atual do jogo. Com o jogo aberto, faz a verificacao na memoria viva.
    2. Fecha a instancia local, compila o executavel (build.bat) e relanca ao fim.
    3. Descobre a versao do jogo e monta a tag: <versao>; se ja existir um
       release dessa versao, usa <versao>-1, <versao>-2, ...
    4. Commita as mudancas do agente e envia para o origin
       (https://github.com/imgidedin/tbh-companion.git).
    5. Cria o GitHub Release na tag com o TBH_Companion.exe (e um .zip) anexados.

.PARAMETER GameDir
  Pasta do TaskBarHero (com GameAssembly.dll). Autodetecta no Steam se omitido.

.PARAMETER SkipMap
  Pula a etapa de recalcular o mapa IL2CPP (usa o main.cpp como esta).

.PARAMETER NoLive
  Passa --no-live ao refresh (nao le a memoria do jogo).

.PARAMETER Draft
  Cria o release como rascunho (nao publicado).

.PARAMETER DryRun
  Faz tudo localmente (mapa, build, tag) mas NAO commita, envia nem publica.

.EXAMPLE
  pwsh scripts\release.ps1                 # release normal (jogo aberto)
  pwsh scripts\release.ps1 -SkipMap        # so build + release
  pwsh scripts\release.ps1 -DryRun         # ensaio, sem publicar
#>
[CmdletBinding()]
param(
  [string]$GameDir,
  [switch]$SkipMap,
  [switch]$NoLive,
  [switch]$Draft,
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$RepoUrl = "https://github.com/imgidedin/tbh-companion.git"
$AgentDir = Split-Path -Parent $PSScriptRoot         # raiz do repo do agente
$ExePath = Join-Path $AgentDir "build\TBH_Companion.exe"
$DistDir = Join-Path $AgentDir "dist"
$MapScript = Join-Path $PSScriptRoot "refresh_il2cpp_map.py"
$BuildBat = Join-Path $AgentDir "build.bat"
$ProcessScript = Join-Path $PSScriptRoot "companion_process.ps1"
$Git = @("-C", $AgentDir)                            # roda git sempre no repo do agente

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "ERRO: $msg" -ForegroundColor Red; exit 1 }
# Roda um comando nativo sem que o stderr dele aborte o script (PS trata stderr
# de exe como erro terminante quando ErrorActionPreference=Stop). Checa o exit code.
function Exec([scriptblock]$cmd) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try { & $cmd } finally { $ErrorActionPreference = $old }
}
function StopCompanion() {
  Exec { & powershell -NoProfile -ExecutionPolicy Bypass -File $ProcessScript -ExePath $ExePath -Stop }
  if ($LASTEXITCODE -ne 0) { Fail "nao foi possivel encerrar o TBH_Companion.exe local." }
}
function StartCompanion() {
  Exec { & powershell -NoProfile -ExecutionPolicy Bypass -File $ProcessScript -ExePath $ExePath -Start }
  if ($LASTEXITCODE -ne 0) { Fail "nao foi possivel iniciar o TBH_Companion.exe local." }
}

Push-Location $AgentDir
try {
  # --- pre-requisitos -------------------------------------------------------
  foreach ($tool in @("git", "gh", "py")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "'$tool' nao encontrado no PATH." }
  }
  gh auth status 1>$null 2>$null
  if ($LASTEXITCODE -ne 0) { Fail "gh nao autenticado. Rode: gh auth login" }

  $gameArg = @()
  if ($GameDir) { $gameArg = @("--game-dir", $GameDir) }

  # --- 1) mapa IL2CPP -------------------------------------------------------
  if ($SkipMap) {
    Step "Pulando recalculo do mapa IL2CPP (-SkipMap)."
  } else {
    Step "Recalculando o mapa IL2CPP para a versao atual do jogo..."
    $mapArgs = @($MapScript) + $gameArg
    if ($NoLive) { $mapArgs += "--no-live" }
    Exec { & py @mapArgs }
    if ($LASTEXITCODE -ne 0) { Fail "refresh_il2cpp_map.py falhou." }
  }

  # --- 2) versao do jogo ----------------------------------------------------
  Step "Detectando a versao do jogo..."
  $version = (Exec { & py $MapScript @gameArg --print-version } 2>$null | Select-Object -Last 1).Trim()
  if (-not $version) { Fail "Nao consegui detectar a versao do jogo." }
  Write-Host "    versao do jogo: $version"

  # --- 3) build -------------------------------------------------------------
  Step "Compilando o executavel (build.bat)..."
  # Encerra uma instancia de dev rodando a partir do proprio build\ (senao o
  # linker falha com LNK1104). Nao mexe em exes instalados em outro lugar.
  StopCompanion
  Exec { & cmd.exe /c "call `"$BuildBat`" --no-restart" }
  if ($LASTEXITCODE -ne 0) { Fail "build.bat falhou." }
  if (-not (Test-Path $ExePath)) { Fail "Executavel nao encontrado: $ExePath" }
  Write-Host ("    OK: {0} ({1:N0} bytes)" -f $ExePath, (Get-Item $ExePath).Length)

  # --- 4) configurar remote + calcular a tag --------------------------------
  $hasOrigin = [bool](Exec { & git @Git remote } | Where-Object { $_ -eq "origin" })
  if (-not $hasOrigin) {
    Step "Adicionando remote origin -> $RepoUrl"
    if (-not $DryRun) { Exec { & git @Git remote add origin $RepoUrl }; $hasOrigin = $true }
  }

  Step "Calculando a tag de release..."
  if ($hasOrigin) { Exec { & git @Git fetch --tags origin 2>&1 | Out-Null } }
  $existing = @()
  $existing += (Exec { & git @Git tag --list })
  # A lista de releases vem do GitHub via --repo (funciona mesmo sem remote local).
  $relList = Exec { & gh release list --repo $RepoUrl --limit 200 --json tagName --jq '.[].tagName' 2>$null }
  if ($relList) { $existing += $relList }
  $existing = $existing | Where-Object { $_ } | Sort-Object -Unique

  $tag = $version
  $n = 0
  while ($existing -contains $tag) { $n++; $tag = "$version-$n" }
  Write-Host "    tag escolhida: $tag"

  # --- 5) empacotar o asset -------------------------------------------------
  Step "Empacotando o release..."
  New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
  $exeAsset = Join-Path $DistDir "TBH_Companion.exe"
  Copy-Item $ExePath $exeAsset -Force
  $zipAsset = Join-Path $DistDir "TBH_Companion-$tag.zip"
  if (Test-Path $zipAsset) { Remove-Item $zipAsset -Force }
  $zipItems = @($exeAsset)
  $readme = Join-Path $AgentDir "README.md"
  if (Test-Path $readme) { $zipItems += $readme }
  Compress-Archive -Path $zipItems -DestinationPath $zipAsset -CompressionLevel Optimal
  Write-Host "    asset: $exeAsset"
  Write-Host "    asset: $zipAsset"

  # --- 6) commit + push -----------------------------------------------------
  $notes = @"
TBH Companion para TaskBarHero **$version**.

- Mapa IL2CPP recalculado para esta versao do jogo.
- Baixe ``TBH_Companion.exe`` e rode. Configure URL do servidor, token e SteamID na primeira execucao.

Gerado por ``scripts\release.ps1``.
"@

  if ($DryRun) {
    Step "DryRun: pronto. NAO commitei/enviei/publiquei."
    Write-Host "    tag:     $tag"
    Write-Host "    assets:  $exeAsset ; $zipAsset"
    Write-Host "    repo:    $RepoUrl"
    Step "Iniciando o companion local atualizado..."
    StartCompanion
    return
  }

  Step "Commitando e enviando o codigo..."
  Exec { & git @Git add -A }
  $dirty = Exec { & git @Git status --porcelain }
  if ($dirty) {
    Exec { & git @Git commit -m "Release $tag (TaskBarHero $version)" `
                            -m "Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>" } | Out-Null
  } else {
    Write-Host "    (nada novo para commitar)"
  }
  $branch = (Exec { & git @Git rev-parse --abbrev-ref HEAD }).Trim()
  Exec { & git @Git push -u origin $branch }
  if ($LASTEXITCODE -ne 0) { Fail "git push falhou." }

  # --- 7) GitHub Release ----------------------------------------------------
  Step "Publicando o GitHub Release '$tag'..."
  $relArgs = @(
    "release", "create", $tag,
    $exeAsset, $zipAsset,
    "--repo", $RepoUrl,
    "--title", "TBH Companion $tag",
    "--notes", $notes,
    "--target", $branch
  )
  if ($Draft) { $relArgs += "--draft" }
  Exec { & gh @relArgs }
  if ($LASTEXITCODE -ne 0) { Fail "gh release create falhou." }

  Step "Iniciando o companion local atualizado..."
  StartCompanion

  Step "Concluido!"
  Write-Host "    Release: https://github.com/imgidedin/tbh-companion/releases/tag/$tag" -ForegroundColor Green
}
finally {
  Pop-Location
}
