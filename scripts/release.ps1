<#
.SYNOPSIS
  Build + release do TBH Companion: recalcula o mapa IL2CPP, compila, versiona
  pela versao do jogo e publica um GitHub Release com o .exe para download.

.DESCRIPTION
  Etapas:
    1. Recalcula o mapa IL2CPP (scripts\refresh_il2cpp_map.py) para a versao
       atual do jogo. Com o jogo aberto, faz a verificacao na memoria viva.
    2. Garante que existe frontend-pack da versao e rebuilda/commita/pusha o
       frontend com os dados/assets atuais do jogo.
    3. Fecha a instancia local, compila o executavel (build.bat) e relanca ao fim.
    4. Descobre a versao do jogo e monta a tag: <versao>; se ja existir um
       release dessa versao, usa <versao>-1, <versao>-2, ...
    5. Commita as mudancas do agente e envia para o origin
       (https://github.com/imgidedin/tbh-companion.git).
    6. Cria o GitHub Release na tag com o TBH_Companion.exe (e um .zip) anexados.

.PARAMETER GameDir
  Pasta do TaskBarHero (com GameAssembly.dll). Autodetecta no Steam se omitido.

.PARAMETER FrontendDir
  Pasta do repo tbh-farm-local-frontend. Por padrao usa o repo irmao.

.PARAMETER ExportedProject
  Pasta ExportedProject do AssetRipper. Obrigatoria quando o frontend-pack da
  versao atual ainda nao existe.

.PARAMETER SkipMap
  Pula a etapa de recalcular o mapa IL2CPP (usa o main.cpp como esta).

.PARAMETER SkipFrontend
  Nao exporta assets nem rebuilda/pusha o frontend. Use apenas para release do
  agente sem mudancas de jogo/frontend.

.PARAMETER ForceAssetExport
  Recria o frontend-pack da versao atual a partir de -ExportedProject.

.PARAMETER NoContactSheets
  Passa -NoContactSheets ao export_unity_assets.ps1 quando gerar frontend-pack.

.PARAMETER NoLive
  Passa --no-live ao refresh (nao le a memoria do jogo).

.PARAMETER Draft
  Cria o release como rascunho (nao publicado).

.PARAMETER DryRun
  Faz tudo localmente (mapa, build, tag) mas NAO commita, envia nem publica.

.PARAMETER LogPath
  Caminho do log/transcript do release. Se omitido, grava em dist\release-<timestamp>.log.

.EXAMPLE
  pwsh scripts\release.ps1                 # release normal (jogo aberto)
  pwsh scripts\release.ps1 -SkipMap        # so build + release
  pwsh scripts\release.ps1 -DryRun         # ensaio, sem publicar
#>
[CmdletBinding()]
param(
  [string]$GameDir,
  [string]$FrontendDir,
  [string]$ExportedProject,
  [switch]$SkipMap,
  [switch]$SkipFrontend,
  [switch]$ForceAssetExport,
  [switch]$NoContactSheets,
  [switch]$NoLive,
  [switch]$Draft,
  [switch]$DryRun,
  [string]$LogPath
)

$ErrorActionPreference = "Stop"

$RepoUrl = "https://github.com/imgidedin/tbh-companion.git"
$RepoSlug = "imgidedin/tbh-companion"
$AgentDir = Split-Path -Parent $PSScriptRoot         # raiz do repo do agente
$DefaultFrontendDir = Join-Path (Split-Path -Parent $AgentDir) "tbh-farm-local-frontend"
$ExePath = Join-Path $AgentDir "build\TBH_Companion.exe"
$DistDir = Join-Path $AgentDir "dist"
$MapScript = Join-Path $PSScriptRoot "refresh_il2cpp_map.py"
$BuildBat = Join-Path $AgentDir "build.bat"
$ProcessScript = Join-Path $PSScriptRoot "companion_process.ps1"
$ExportUnityScript = Join-Path $PSScriptRoot "export_unity_assets.ps1"
$Git = @("-C", $AgentDir)                            # roda git sempre no repo do agente

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) { throw $msg }
function Resolve-FullPath([string]$path) {
  $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path)
}
function ConvertTo-SafeName([string]$value) {
  $invalid = [System.IO.Path]::GetInvalidFileNameChars()
  $safe = $value
  foreach ($char in $invalid) {
    $safe = $safe.Replace([string]$char, "-")
  }
  return $safe.Replace(" ", "-")
}
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
function Get-FrontendPackInfo([string]$version) {
  $safeVersion = ConvertTo-SafeName $version
  $versionDir = Join-Path (Join-Path $AgentDir "exported-assets") "TaskBarHero-$safeVersion"
  $pack = Join-Path $versionDir "frontend-pack"
  $data = Join-Path $pack "data"
  return [pscustomobject]@{
    VersionDir = $versionDir
    Pack = $pack
    Data = $data
    Manifest = Join-Path $pack "manifest.json"
  }
}
function Test-FrontendPack([object]$pack) {
  return (
    (Test-Path $pack.Manifest) -and
    (Test-Path (Join-Path $pack.Data "raw-csv"))
  )
}
function Ensure-FrontendPack([object]$pack, [string]$resolvedExportedProject, [string]$resolvedGameDir) {
  if ((Test-FrontendPack $pack) -and -not $ForceAssetExport) {
    Write-Host "    frontend-pack existente: $($pack.Pack)"
    return
  }

  if (-not $resolvedExportedProject) {
    Fail "Nao existe frontend-pack valido em $($pack.Pack). Passe -ExportedProject `"C:\caminho\ExportedProject`" para gerar assets antes do release, ou use -SkipFrontend conscientemente."
  }
  if (-not (Test-Path (Join-Path $resolvedExportedProject "Assets"))) {
    Fail "ExportedProject invalido: nao encontrei Assets em $resolvedExportedProject"
  }

  Step "Gerando frontend-pack da versao atual..."
  $exportArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", $ExportUnityScript,
    "-OrganizeExportedProject", $resolvedExportedProject
  )
  if ($resolvedGameDir) { $exportArgs += @("-GameDir", $resolvedGameDir) }
  if ($ForceAssetExport) { $exportArgs += "-Force" }
  if ($NoContactSheets) { $exportArgs += "-NoContactSheets" }

  Exec { & powershell @exportArgs }
  if ($LASTEXITCODE -ne 0) { Fail "export_unity_assets.ps1 falhou." }
  if (-not (Test-FrontendPack $pack)) {
    Fail "export_unity_assets.ps1 concluiu, mas nao gerou frontend-pack valido em $($pack.Pack)"
  }
}
function Invoke-FrontendRelease([object]$pack, [string]$resolvedFrontendDir, [string]$resolvedExportedProject, [string]$resolvedGameDir, [string]$version) {
  if (-not (Test-Path (Join-Path $resolvedFrontendDir "scripts\rebuild-from-agent-pack.ps1"))) {
    Fail "FrontendDir invalido: nao encontrei scripts\rebuild-from-agent-pack.ps1 em $resolvedFrontendDir"
  }

  Step "Rebuildando frontend com assets/dados do jogo..."
  $rebuildArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $resolvedFrontendDir "scripts\rebuild-from-agent-pack.ps1"),
    "-AgentDir", $AgentDir,
    "-PackDir", $pack.Pack
  )
  if ($resolvedGameDir) { $rebuildArgs += @("-GameDir", $resolvedGameDir) }
  if ($resolvedExportedProject) {
    $rebuildArgs += @("-ExportedProject", $resolvedExportedProject)
  }
  if (-not $DryRun) {
    $rebuildArgs += @("-Commit", "-Push", "-CommitMessage", "Update TaskBarHero assets $version")
  }

  Exec { & powershell @rebuildArgs }
  if ($LASTEXITCODE -ne 0) { Fail "rebuild-from-agent-pack.ps1 falhou." }
}

$script:ReleaseTranscriptStarted = $false
if (-not $LogPath) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $LogPath = Join-Path $DistDir "release-$stamp.log"
}
$LogPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogPath)

function StartReleaseLog() {
  $logDir = Split-Path -Parent $LogPath
  if ($logDir) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
  try {
    Start-Transcript -Path $LogPath -Append | Out-Null
    $script:ReleaseTranscriptStarted = $true
    Write-Host "Log do release: $LogPath"
  } catch {
    Write-Warning "Nao foi possivel iniciar transcript em '$LogPath': $($_.Exception.Message)"
  }
}

function StopReleaseLog() {
  if ($script:ReleaseTranscriptStarted) {
    try { Stop-Transcript | Out-Null } catch { }
  }
}

$pushedLocation = $false
$frontendReleaseCompleted = $false
StartReleaseLog
try {
  Push-Location $AgentDir
  $pushedLocation = $true

  # --- pre-requisitos -------------------------------------------------------
  foreach ($tool in @("git", "gh", "py")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "'$tool' nao encontrado no PATH." }
  }
  Exec { & gh auth status 1>$null 2>$null }
  if ($LASTEXITCODE -ne 0) { Fail "gh nao autenticado. Rode: gh auth login" }

  $gameArg = @()
  $resolvedGameDir = $null
  if ($GameDir) {
    $resolvedGameDir = Resolve-FullPath $GameDir
    $gameArg = @("--game-dir", $resolvedGameDir)
  }

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
  $versionOutput = @(Exec { & py $MapScript @gameArg --print-version } 2>$null)
  if ($LASTEXITCODE -ne 0) { Fail "refresh_il2cpp_map.py --print-version falhou." }
  $version = ($versionOutput | Where-Object { $_ } | Select-Object -Last 1)
  if ($version) { $version = $version.Trim() }
  if (-not $version) { Fail "Nao consegui detectar a versao do jogo." }
  Write-Host "    versao do jogo: $version"

  # --- 3) frontend pack + rebuild ------------------------------------------
  if ($SkipFrontend) {
    Step "Pulando export/rebuild do frontend (-SkipFrontend)."
  } else {
    $resolvedFrontendDir = Resolve-FullPath ($(if ($FrontendDir) { $FrontendDir } else { $DefaultFrontendDir }))
    if (-not (Test-Path $resolvedFrontendDir)) {
      Fail "FrontendDir nao encontrado: $resolvedFrontendDir. Passe -FrontendDir ou use -SkipFrontend conscientemente."
    }
    $resolvedExportedProject = $null
    if ($ExportedProject) {
      $resolvedExportedProject = Resolve-FullPath $ExportedProject
    }

    $pack = Get-FrontendPackInfo $version
    Ensure-FrontendPack $pack $resolvedExportedProject $resolvedGameDir
    Invoke-FrontendRelease $pack $resolvedFrontendDir $resolvedExportedProject $resolvedGameDir $version
    $frontendReleaseCompleted = $true
  }

  # --- 4) build -------------------------------------------------------------
  Step "Compilando o executavel (build.bat)..."
  # Encerra uma instancia de dev rodando a partir do proprio build\ (senao o
  # linker falha com LNK1104). Nao mexe em exes instalados em outro lugar.
  StopCompanion
  Exec { & cmd.exe /c "call `"$BuildBat`" --no-restart --release" }
  if ($LASTEXITCODE -ne 0) { Fail "build.bat falhou." }
  if (-not (Test-Path $ExePath)) { Fail "Executavel nao encontrado: $ExePath" }
  Write-Host ("    OK: {0} ({1:N0} bytes)" -f $ExePath, (Get-Item $ExePath).Length)

  # --- 5) configurar remote + calcular a tag --------------------------------
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
  $relList = Exec { & gh release list --repo $RepoSlug --limit 200 --json tagName --jq '.[].tagName' 2>$null }
  if ($LASTEXITCODE -ne 0) { Fail "gh release list falhou para $RepoSlug." }
  if ($relList) { $existing += $relList }
  $existing = $existing | Where-Object { $_ } | Sort-Object -Unique

  $tag = $version
  $n = 0
  while ($existing -contains $tag) { $n++; $tag = "$version-$n" }
  Write-Host "    tag escolhida: $tag"

  # --- 6) empacotar o asset -------------------------------------------------
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

  # --- 7) commit + push -----------------------------------------------------
  $notes = @"
TBH Companion para TaskBarHero **$version**.

- Mapa IL2CPP recalculado para esta versao do jogo.
$(if ($frontendReleaseCompleted) { "- Frontend rebuildado com dados/assets do jogo para esta versao." } else { "- Frontend nao foi rebuildado neste release (-SkipFrontend)." })
- Baixe ``TBH_Companion.exe`` e rode. Configure URL do servidor, token e SteamID na primeira execucao.

Gerado por ``scripts\release.ps1``.
"@

  if ($DryRun) {
    Step "DryRun: pronto. NAO commitei/enviei/publiquei."
    Write-Host "    tag:     $tag"
    Write-Host "    assets:  $exeAsset ; $zipAsset"
    Write-Host "    repo:    $RepoSlug"
    Write-Host "    log:     $LogPath"
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

  # --- 8) GitHub Release ----------------------------------------------------
  Step "Publicando o GitHub Release '$tag'..."
  $relArgs = @(
    "release", "create", $tag,
    $exeAsset, $zipAsset,
    "--repo", $RepoSlug,
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
  Write-Host "    Log: $LogPath"
} catch {
  Write-Host "ERRO: $($_.Exception.Message)" -ForegroundColor Red
  if ($_.ScriptStackTrace) {
    Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray
  }
  Write-Host "Log: $LogPath" -ForegroundColor Yellow
  exit 1
}
finally {
  if ($pushedLocation) { Pop-Location }
  StopReleaseLog
}
