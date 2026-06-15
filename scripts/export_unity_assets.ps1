<#
  Exporta/organiza assets do TaskBarHero para uso no companion.

  Fluxo recomendado com AssetRipper GUI:
    1) Exporte o jogo pelo AssetRipper para uma pasta ExportedProject.
    2) Rode este script com -OrganizeExportedProject "...\ExportedProject".

  Por seguranca, o modo de exportacao direta por CLI continua desativado por
  padrao. A protecao contra rodar duas vezes na mesma versao existe, mas tambem
  fica desligada ate validarmos o fluxo completo.
#>

[CmdletBinding()]
param(
  [string]$GameDir,
  [string]$OutDir,

  [string]$OrganizeExportedProject,

  [string]$ExporterExe,
  [string[]]$ExporterArgs = @("{GameDir}", "-o", "{RawDir}"),
  [switch]$Enable,

  [switch]$EnforceVersionGuard,
  [switch]$Force,
  [switch]$NoOrganize,
  [switch]$NoContactSheets,
  [switch]$SkipLocalizationBundleExtract,
  [string]$PythonExe
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AgentDir = Split-Path -Parent $ScriptDir
$DefaultOutDir = Join-Path $AgentDir "exported-assets"
$GameExe = "TaskbarHero.exe"

function Fail($message) {
  Write-Error $message
  exit 1
}

function Resolve-FullPath([string]$path) {
  $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path)
}

function Find-GameDir([string]$explicit) {
  if ($explicit) {
    $full = Resolve-FullPath $explicit
    if (Test-Path (Join-Path $full $GameExe)) {
      return $full
    }
    Fail "GameDir invalido: nao encontrei $GameExe em $full"
  }

  $candidates = New-Object System.Collections.Generic.List[string]
  $steamRoots = @(
    "C:\Program Files (x86)\Steam",
    "C:\Program Files\Steam"
  )

  foreach ($steamRoot in $steamRoots) {
    $vdf = Join-Path $steamRoot "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
      $content = [System.IO.File]::ReadAllText($vdf)
      $matches = [regex]::Matches($content, '"path"\s+"([^"]+)"')
      foreach ($match in $matches) {
        $library = $match.Groups[1].Value.Replace("\\", "\")
        $candidates.Add((Join-Path $library "steamapps\common\TaskbarHero")) | Out-Null
      }
    }
  }

  foreach ($drive in "C","D","E","F","G") {
    $candidates.Add("$drive`:\SteamLibrary\steamapps\common\TaskbarHero") | Out-Null
    $candidates.Add("$drive`:\Program Files (x86)\Steam\steamapps\common\TaskbarHero") | Out-Null
  }

  foreach ($candidate in $candidates) {
    if (Test-Path (Join-Path $candidate $GameExe)) {
      return (Resolve-FullPath $candidate)
    }
  }

  Fail "Nao encontrei a instalacao do TaskBarHero. Passe -GameDir `"caminho\TaskbarHero`"."
}

function Get-GameVersion([string]$gameDir) {
  $versionFile = Join-Path $gameDir "Version.txt"
  if (Test-Path $versionFile) {
    $version = ([System.IO.File]::ReadAllText($versionFile)).Trim()
    if ($version) {
      return $version
    }
  }
  return "desconhecida"
}

function ConvertTo-SafeName([string]$value) {
  $invalid = [System.IO.Path]::GetInvalidFileNameChars()
  $safe = $value
  foreach ($char in $invalid) {
    $safe = $safe.Replace([string]$char, "-")
  }
  return $safe.Replace(" ", "-")
}

function Ensure-Dir([string]$path) {
  if (-not (Test-Path $path)) {
    New-Item -ItemType Directory -Path $path | Out-Null
  }
}

function Assert-ChildPath([string]$parent, [string]$child) {
  $parentFull = Resolve-FullPath $parent
  $childParent = Split-Path -Parent $child
  if (-not (Test-Path $childParent)) {
    Ensure-Dir $childParent
  }
  $childFull = Resolve-FullPath $child
  $prefix = $parentFull.TrimEnd('\') + '\'
  if (-not $childFull.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail "Caminho recusado para operacao recursiva: $childFull nao fica dentro de $parentFull"
  }
}

function Reset-Dir([string]$path, [string]$safeParent) {
  if (Test-Path $path) {
    Assert-ChildPath $safeParent $path
    Remove-Item -LiteralPath $path -Recurse -Force
  }
  Ensure-Dir $path
}

function Test-Python([string]$candidate) {
  if (-not $candidate) {
    return $false
  }
  if (-not (Test-Path $candidate)) {
    return $false
  }
  if ($candidate -like "*\WindowsApps\python*.exe") {
    return $false
  }
  try {
    & $candidate -c "import sys" *> $null
    return $LASTEXITCODE -eq 0
  } catch {
    return $false
  }
}

function Resolve-PythonExe() {
  if ($PythonExe) {
    $full = Resolve-FullPath $PythonExe
    if (Test-Python $full) {
      return $full
    }
    Fail "PythonExe invalido: $full"
  }

  $codexRuntime = Join-Path $env:USERPROFILE ".cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
  if (Test-Python $codexRuntime) {
    return $codexRuntime
  }

  $command = Get-Command python.exe -ErrorAction SilentlyContinue
  if ($command -and (Test-Python $command.Source)) {
    return $command.Source
  }

  return $null
}

function Ensure-UnityPy([string]$python) {
  $oldPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    & $python -c "import UnityPy" *> $null
    $hasUnityPy = $LASTEXITCODE -eq 0
  } finally {
    $ErrorActionPreference = $oldPreference
  }

  if ($hasUnityPy) {
    return
  }

  Write-Host "  UnityPy nao encontrado; instalando no Python selecionado..."
  $oldPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    & $python -m pip install UnityPy
    $pipExitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $oldPreference
  }

  if ($pipExitCode -ne 0) {
    Fail "Nao consegui instalar UnityPy. Instale manualmente com: $python -m pip install UnityPy"
  }
}

function Expand-ExporterArgs([string[]]$args, [string]$gameDir, [string]$rawDir, [string]$versionDir, [string]$version) {
  $expanded = New-Object System.Collections.Generic.List[string]
  foreach ($arg in $args) {
    $expanded.Add(
      $arg.
        Replace("{GameDir}", $gameDir).
        Replace("{RawDir}", $rawDir).
        Replace("{OutDir}", $versionDir).
        Replace("{Version}", $version)
    ) | Out-Null
  }
  return $expanded.ToArray()
}

function Get-PortableImageBucket([System.IO.FileInfo]$file) {
  $name = $file.BaseName.ToLowerInvariant()

  if ($name -match "noto|emoji|sdf atlas|font") { return "fonts" }
  if ($name -match "^hero_\d+|^herodead_|arrage_chaanim|chaillust|selecthero|shadow_hero|classspriteatlas|animationsheet_hero") { return "heroes" }
  if ($name -match "^pet|petslot|bg_pet|arrange_nameplate_pet") { return "pets" }
  if ($name -match "monster|boss|hydra|sibuna|turret") { return "monsters" }
  if ($name -match "item|gear|rune|currency|gold|soulstone|material|chest|box|cube") { return "items" }
  if ($name -match "stage|layer|house|castle|tree|terrain|portal") { return "stages" }
  if ($name -match "skill|effect|magiccircle|flame|frozen|attack|slash|hit|dead|dash|jump|fire|thin|medium|large") { return "skills-effects" }
  if ($name -match "ui|icon|button|panel|slot|tooltip|gauge|slider|lock|warning|achievement|loading|gradebg|bg_|border|boader|decor|nameplate") { return "ui" }

  return "misc"
}

function Get-RawAssetBucket([System.IO.FileInfo]$file) {
  $ext = $file.Extension.ToLowerInvariant()
  $name = $file.BaseName.ToLowerInvariant()

  if ($ext -in @(".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tga", ".dds", ".psd")) {
    return "images\" + (Get-PortableImageBucket $file)
  }
  if ($ext -in @(".wav", ".mp3", ".ogg", ".m4a", ".aiff")) { return "audio" }
  if ($ext -in @(".ttf", ".otf", ".fontsettings")) { return "fonts" }
  if ($ext -in @(".txt", ".json", ".csv", ".tsv", ".xml", ".bytes", ".asset")) { return "data\raw" }
  if ($ext -in @(".fbx", ".obj", ".dae", ".blend", ".mesh")) { return "meshes" }
  if ($ext -in @(".anim", ".controller", ".overridecontroller")) { return "animations" }
  if ($ext -eq ".prefab") { return "prefabs" }
  if ($ext -eq ".mat") { return "materials" }
  if ($ext -in @(".shader", ".cginc", ".hlsl")) { return "shaders" }

  return "unknown"
}

function Ensure-PortableTree([string]$packDir) {
  $dirs = @(
    "images\heroes",
    "images\pets",
    "images\monster-sprites",
    "images\monsters",
    "images\items",
    "images\stages",
    "images\skills-effects",
    "images\ui",
    "images\fonts",
    "images\misc",
    "audio",
    "data\raw-csv",
    "data\json",
    "data\localization",
    "contact-sheets"
  )
  foreach ($dir in $dirs) {
    Ensure-Dir (Join-Path $packDir $dir)
  }
}

function Ensure-RawTree([string]$assetsDir) {
  $dirs = @(
    "images\heroes",
    "images\pets",
    "images\monster-sprites",
    "images\monsters",
    "images\items",
    "images\stages",
    "images\skills-effects",
    "images\ui",
    "images\fonts",
    "images\misc",
    "audio",
    "fonts",
    "data\raw",
    "meshes",
    "animations",
    "prefabs",
    "materials",
    "shaders",
    "unknown"
  )
  foreach ($dir in $dirs) {
    Ensure-Dir (Join-Path $assetsDir $dir)
  }
}

function Get-UniqueDestination([string]$targetDir, [System.IO.FileInfo]$file) {
  $dest = Join-Path $targetDir $file.Name
  if (-not (Test-Path $dest)) {
    return $dest
  }

  $hash = (Get-FileHash -Algorithm SHA1 -Path $file.FullName).Hash.Substring(0, 8).ToLowerInvariant()
  $stem = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
  $ext = $file.Extension
  $dest = Join-Path $targetDir "$stem-$hash$ext"

  $i = 2
  while (Test-Path $dest) {
    $dest = Join-Path $targetDir "$stem-$hash-$i$ext"
    $i++
  }
  return $dest
}

function Copy-ManifestFile([System.IO.FileInfo]$file, [string]$targetRoot, [string]$bucket) {
  $targetDir = Join-Path $targetRoot $bucket
  Ensure-Dir $targetDir
  $dest = Get-UniqueDestination $targetDir $file
  Copy-Item -LiteralPath $file.FullName -Destination $dest

  return [pscustomobject]@{
    source = $file.FullName
    relative = $dest.Substring($targetRoot.Length).TrimStart('\')
    bucket = $bucket
    size = $file.Length
    sha1 = (Get-FileHash -Algorithm SHA1 -Path $file.FullName).Hash.ToLowerInvariant()
  }
}

function Organize-RawExport([string]$rawDir, [string]$assetsDir) {
  $entries = New-Object System.Collections.Generic.List[object]
  if (-not (Test-Path $rawDir)) {
    return $entries
  }

  Ensure-RawTree $assetsDir
  $files = Get-ChildItem -LiteralPath $rawDir -Recurse -File
  foreach ($file in $files) {
    $bucket = Get-RawAssetBucket $file
    $entries.Add((Copy-ManifestFile $file $assetsDir $bucket)) | Out-Null
  }
  return $entries
}

function Convert-TextAssetCsvs([string]$textAssetDir, [string]$packDir) {
  $converted = New-Object System.Collections.Generic.List[object]
  if (-not (Test-Path $textAssetDir)) {
    return $converted
  }

  $rawCsvDir = Join-Path $packDir "data\raw-csv"
  $jsonDir = Join-Path $packDir "data\json"
  Ensure-Dir $rawCsvDir
  Ensure-Dir $jsonDir

  $txtFiles = Get-ChildItem -LiteralPath $textAssetDir -File -Filter *.txt
  foreach ($file in $txtFiles) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $rawCsvDir $file.Name) -Force
  }

  $selected = @(
    "HeroInfoData",
    "MonsterInfoData",
    "PetInfoData",
    "PetStatInfoData",
    "StageInfoData",
    "StageLevelInfoData",
    "ItemInfoData",
    "ItemGroupInfoData",
    "GearInfoData",
    "MaterialInfoData",
    "RuneInfoData",
    "SkillInfoData",
    "PassiveSkillInfoData",
    "GradeInfoData",
    "CurrencyInfoData",
    "SoundInfoData"
  )

  foreach ($name in $selected) {
    $path = Join-Path $textAssetDir "$name.txt"
    if (-not (Test-Path $path)) {
      continue
    }

    try {
      $rows = Import-Csv -LiteralPath $path
      $out = Join-Path $jsonDir "$name.json"
      $rows | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 -Path $out
      $converted.Add([pscustomobject]@{
        name = $name
        rows = @($rows).Count
        json = $out.Substring($packDir.Length).TrimStart('\')
      }) | Out-Null
    } catch {
      Write-Warning "Nao consegui converter $path para JSON: $($_.Exception.Message)"
    }
  }

  return $converted
}

function Copy-Localization([string]$projectDir, [string]$packDir) {
  $entries = New-Object System.Collections.Generic.List[object]
  $sourceDir = Join-Path $projectDir "Assets\Localization"
  if (-not (Test-Path $sourceDir)) {
    return $entries
  }

  $destRoot = Join-Path $packDir "data\localization"
  Ensure-Dir $destRoot

  $files = Get-ChildItem -LiteralPath $sourceDir -Recurse -File
  foreach ($file in $files) {
    $relative = $file.FullName.Substring($sourceDir.Length).TrimStart('\')
    $dest = Join-Path $destRoot $relative
    Ensure-Dir (Split-Path -Parent $dest)
    Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
    $entries.Add([pscustomobject]@{
      source = $file.FullName
      relative = $dest.Substring($packDir.Length).TrimStart('\')
      size = $file.Length
    }) | Out-Null
  }
  return $entries
}

function Export-LocalizationBundles([string]$gameDir, [string]$packDir) {
  $entries = New-Object System.Collections.Generic.List[object]
  if ($SkipLocalizationBundleExtract) {
    return $entries
  }

  $python = Resolve-PythonExe
  if (-not $python) {
    Fail "Python nao encontrado para extrair localization bundles. Passe -PythonExe ou use -SkipLocalizationBundleExtract."
  }

  Ensure-UnityPy $python

  $extractor = Join-Path $ScriptDir "extract_unity_localization.py"
  if (-not (Test-Path $extractor)) {
    Fail "Extractor de localization nao encontrado: $extractor"
  }

  $outDir = Join-Path $packDir "data\localization\extracted"
  Ensure-Dir $outDir

  $output = & $python $extractor --game-dir $gameDir --out-dir $outDir --locales en-US pt-BR
  if ($LASTEXITCODE -ne 0) {
    Fail "Extractor de localization falhou."
  }

  try {
    $result = $output | Select-Object -Last 1 | ConvertFrom-Json
  } catch {
    Fail "Extractor de localization retornou saida invalida: $output"
  }

  foreach ($path in @($result.json, $result.tsv)) {
    if (-not (Test-Path $path)) {
      Fail "Extractor de localization nao gerou arquivo esperado: $path"
    }
    $file = Get-Item -LiteralPath $path
    $entries.Add([pscustomobject]@{
      source = $file.FullName
      relative = $file.FullName.Substring($packDir.Length).TrimStart('\')
      size = $file.Length
      generated = $true
    }) | Out-Null
  }

  return $entries
}

function Copy-PortableImages([string]$projectDir, [string]$packDir) {
  $entries = New-Object System.Collections.Generic.List[object]
  $textureDir = Join-Path $projectDir "Assets\Texture2D"
  if (-not (Test-Path $textureDir)) {
    return $entries
  }

  $imageRoot = Join-Path $packDir "images"
  $imageExts = @(".png", ".jpg", ".jpeg", ".webp", ".bmp")
  $files = Get-ChildItem -LiteralPath $textureDir -Recurse -File | Where-Object { $imageExts -contains $_.Extension.ToLowerInvariant() }

  foreach ($file in $files) {
    $bucket = Get-PortableImageBucket $file
    $entry = Copy-ManifestFile $file $imageRoot $bucket
    $entry | Add-Member -NotePropertyName name -NotePropertyValue $file.Name
    $entries.Add($entry) | Out-Null
  }

  return $entries
}

function Copy-PortableAudio([string]$projectDir, [string]$packDir) {
  $entries = New-Object System.Collections.Generic.List[object]
  $audioDir = Join-Path $projectDir "Assets\AudioClip"
  if (-not (Test-Path $audioDir)) {
    return $entries
  }

  $destRoot = Join-Path $packDir "audio"
  $audioExts = @(".wav", ".ogg", ".mp3", ".m4a", ".aiff")
  $files = Get-ChildItem -LiteralPath $audioDir -Recurse -File | Where-Object { $audioExts -contains $_.Extension.ToLowerInvariant() }

  foreach ($file in $files) {
    $entries.Add((Copy-ManifestFile $file $destRoot "")) | Out-Null
  }

  return $entries
}

function New-GuidIndex([string]$assetsDir) {
  $guidIndex = @{}
  $metaFiles = Get-ChildItem -LiteralPath $assetsDir -Recurse -File -Filter *.meta
  foreach ($meta in $metaFiles) {
    $text = [System.IO.File]::ReadAllText($meta.FullName)
    $match = [regex]::Match($text, "(?m)^guid:\s*([0-9a-f]+)\s*$")
    if ($match.Success) {
      $assetPath = $meta.FullName.Substring(0, $meta.FullName.Length - 5)
      $guidIndex[$match.Groups[1].Value] = $assetPath
    }
  }
  return $guidIndex
}

function Get-OverrideClipGuids([string]$text) {
  @([regex]::Matches($text, "m_OverrideClip:\s*\{fileID:\s*7400000,\s*guid:\s*([0-9a-f]{32}),") | ForEach-Object {
    $_.Groups[1].Value
  })
}

function Get-UnityGuids([string]$text) {
  @([regex]::Matches($text, "guid:\s*([0-9a-f]{32})") | ForEach-Object {
    $_.Groups[1].Value
  })
}

function Get-SpriteTexturePath([string]$spritePath, [hashtable]$guidIndex) {
  if (-not $spritePath -or -not (Test-Path $spritePath)) {
    return $null
  }

  $text = [System.IO.File]::ReadAllText($spritePath)
  $match = [regex]::Match($text, "texture:\s*\{fileID:\s*2800000,\s*guid:\s*([0-9a-f]{32}),")
  if (-not $match.Success) {
    return $null
  }

  $textureGuid = $match.Groups[1].Value
  if ($guidIndex.ContainsKey($textureGuid)) {
    return $guidIndex[$textureGuid]
  }

  return $null
}

function Get-SpriteTextureRect([string]$spritePath) {
  $text = [System.IO.File]::ReadAllText($spritePath)
  $match = [regex]::Match(
    $text,
    "textureRect:\s*\r?\n\s*serializedVersion:\s*\d+\s*\r?\n\s*x:\s*([-0-9.]+)\s*\r?\n\s*y:\s*([-0-9.]+)\s*\r?\n\s*width:\s*([-0-9.]+)\s*\r?\n\s*height:\s*([-0-9.]+)"
  )

  if (-not $match.Success) {
    return $null
  }

  return [pscustomobject]@{
    x = [double]$match.Groups[1].Value
    y = [double]$match.Groups[2].Value
    width = [double]$match.Groups[3].Value
    height = [double]$match.Groups[4].Value
  }
}

function Get-AnimationKind([string]$clipName) {
  $name = $clipName.ToLowerInvariant()
  if ($name -match "idle") { return "idle" }
  if ($name -match "walk|run|move") { return "walk" }
  if ($name -match "dead|death|die") { return "dead" }
  if ($name -match "attack|skill|atk|strike|hit") { return "attack" }
  return "other"
}

function Resolve-AnimationClipFrames([string]$clipPath, [string]$projectDir, [hashtable]$guidIndex, [string]$kindOverride) {
  $clipText = [System.IO.File]::ReadAllText($clipPath)
  $clipName = [System.IO.Path]::GetFileName($clipPath)
  $kind = Get-AnimationKind $clipName
  if ($kindOverride) {
    $kind = $kindOverride
  }

  $frames = New-Object System.Collections.Generic.List[object]
  $clipTextures = New-Object System.Collections.Generic.HashSet[string]
  $clipSprites = New-Object System.Collections.Generic.HashSet[string]
  $matches = [regex]::Matches(
    $clipText,
    "-\s*time:\s*([-0-9.]+)\s*\r?\n\s*value:\s*\{fileID:\s*21300000,\s*guid:\s*([0-9a-f]{32}),"
  )

  $index = 0
  foreach ($match in $matches) {
    $spriteGuid = $match.Groups[2].Value
    if (-not $guidIndex.ContainsKey($spriteGuid)) {
      continue
    }

    $spritePath = $guidIndex[$spriteGuid]
    if (-not $spritePath -or -not (Test-Path $spritePath) -or ([System.IO.Path]::GetExtension($spritePath) -ne ".asset")) {
      continue
    }
    if ($spritePath -notlike "*\Sprite\*") {
      continue
    }

    $texturePath = Get-SpriteTexturePath $spritePath $guidIndex
    $relativeTexture = ""
    if ($texturePath) {
      $relativeTexture = $texturePath.Substring($projectDir.Length).TrimStart('\')
      $clipTextures.Add($relativeTexture) | Out-Null
    }

    $spriteName = [System.IO.Path]::GetFileName($spritePath)
    $clipSprites.Add($spriteName) | Out-Null
    $frames.Add([pscustomobject]@{
      index = $index
      time = [double]$match.Groups[1].Value
      sprite = $spriteName
      spriteAsset = $spritePath.Substring($projectDir.Length).TrimStart('\')
      texture = $relativeTexture
      rect = Get-SpriteTextureRect $spritePath
    }) | Out-Null
    $index++
  }

  $spriteArray = @($clipSprites.GetEnumerator() | Sort-Object)
  $textureArray = @($clipTextures.GetEnumerator() | Sort-Object)
  $frameArray = @($frames.ToArray())

  return [pscustomobject]@{
    clip = $clipName
    kind = $kind
    source = $clipPath.Substring($projectDir.Length).TrimStart('\')
    frameCount = $frames.Count
    sprites = $spriteArray
    textures = $textureArray
    frames = $frameArray
  }
}

function Get-BossBaseMonsterKey([string]$monsterKey) {
  if ($monsterKey -match "^[123]090[2-4]$") {
    return $monsterKey.Substring(0, 4) + "1"
  }
  return $null
}

function Resolve-MonsterSpriteAssets([object]$monster, [string]$projectDir, [hashtable]$guidIndex) {
  $assetsDir = Join-Path $projectDir "Assets"
  $monsterKey = $monster.MonsterKey
  $controllerPath = Join-Path $assetsDir "AnimatorOverrideController\Monster_$monsterKey.overrideController"
  $clipSet = New-Object System.Collections.Generic.HashSet[string]
  $spriteSet = New-Object System.Collections.Generic.HashSet[string]
  $textureSet = New-Object System.Collections.Generic.HashSet[string]
  $clips = New-Object System.Collections.Generic.List[object]

  if (Test-Path $controllerPath) {
    $controller = [System.IO.File]::ReadAllText($controllerPath)
    $clipGuids = @(Get-OverrideClipGuids $controller | Select-Object -Unique)
    foreach ($clipGuid in $clipGuids) {
      if (-not $guidIndex.ContainsKey($clipGuid)) {
        continue
      }
      $clipPath = $guidIndex[$clipGuid]
      if (-not $clipPath -or -not (Test-Path $clipPath) -or ([System.IO.Path]::GetExtension($clipPath) -ne ".anim")) {
        continue
      }

      $clip = Resolve-AnimationClipFrames $clipPath $projectDir $guidIndex ""
      $clipSet.Add($clip.clip) | Out-Null
      foreach ($spriteName in $clip.sprites) {
        $spriteSet.Add($spriteName) | Out-Null
      }
      foreach ($relativeTexture in $clip.textures) {
        $textureSet.Add($relativeTexture) | Out-Null
      }
      $clips.Add($clip) | Out-Null
    }
  }

  if ($monster.PSObject.Properties.Name -contains "SkillKey" -and $monster.SkillKey) {
    $skillKeys = @($monster.SkillKey -split "\s+" | Where-Object { $_ })
    foreach ($skillKey in $skillKeys) {
      $skillClipPath = Join-Path $assetsDir "AnimationClip\Skill_$skillKey.anim"
      if (Test-Path $skillClipPath) {
        $skillClip = Resolve-AnimationClipFrames $skillClipPath $projectDir $guidIndex "attack"
        $clipSet.Add($skillClip.clip) | Out-Null
        foreach ($spriteName in $skillClip.sprites) {
          $spriteSet.Add($spriteName) | Out-Null
        }
        foreach ($relativeTexture in $skillClip.textures) {
          $textureSet.Add($relativeTexture) | Out-Null
        }
        $clips.Add($skillClip) | Out-Null
      }
    }
  }

  return [pscustomobject]@{
    controller = $controllerPath.Substring($projectDir.Length).TrimStart('\')
    clipCount = $clipSet.Count
    spriteCount = $spriteSet.Count
    textureCount = $textureSet.Count
    textures = @($textureSet | Sort-Object)
    clips = @($clips | Sort-Object clip)
  }
}

function Merge-MonsterResolvedAssets([object]$baseResolved, [object]$variantResolved) {
  $clipByName = @{}
  foreach ($clip in $baseResolved.clips) {
    $clipByName[$clip.clip] = $clip
  }
  foreach ($clip in $variantResolved.clips) {
    $clipByName[$clip.clip] = $clip
  }

  $textureSet = New-Object System.Collections.Generic.HashSet[string]
  $spriteSet = New-Object System.Collections.Generic.HashSet[string]
  foreach ($clip in $clipByName.Values) {
    foreach ($texture in $clip.textures) {
      $textureSet.Add($texture) | Out-Null
    }
    foreach ($sprite in $clip.sprites) {
      $spriteSet.Add($sprite) | Out-Null
    }
  }

  $clips = @($clipByName.Values | Sort-Object clip)
  return [pscustomobject]@{
    controller = $baseResolved.controller
    clipCount = $clips.Count
    spriteCount = $spriteSet.Count
    textureCount = $textureSet.Count
    textures = @($textureSet.GetEnumerator() | Sort-Object)
    clips = $clips
  }
}

function Copy-MonsterTexture([string]$projectDir, [string]$packDir, [string]$relativeTexture) {
  $source = Join-Path $projectDir $relativeTexture
  if (-not (Test-Path $source)) {
    return $null
  }

  $monsterDir = Join-Path $packDir "images\monster-sprites"
  Ensure-Dir $monsterDir
  $dest = Join-Path $monsterDir ([System.IO.Path]::GetFileName($source))
  if (-not (Test-Path $dest)) {
    Copy-Item -LiteralPath $source -Destination $dest
  }

  $file = Get-Item -LiteralPath $source
  return [pscustomobject]@{
    source = $source
    relative = $dest.Substring($packDir.Length).TrimStart('\')
    size = $file.Length
    sha1 = (Get-FileHash -Algorithm SHA1 -Path $source).Hash.ToLowerInvariant()
  }
}

function New-MonsterSpriteMap([string]$projectDir, [string]$packDir) {
  $assetsDir = Join-Path $projectDir "Assets"
  $monsterInfo = Join-Path $assetsDir "TextAsset\MonsterInfoData.txt"
  if (-not (Test-Path $monsterInfo)) {
    return [pscustomobject]@{ rows = @(); copied = @() }
  }

  $guidIndex = New-GuidIndex $assetsDir
  $monsters = Import-Csv -LiteralPath $monsterInfo
  $monsterByKey = @{}
  foreach ($monster in $monsters) {
    $monsterByKey[$monster.MonsterKey] = $monster
  }
  $rows = New-Object System.Collections.Generic.List[object]
  $animationRows = New-Object System.Collections.Generic.List[object]
  $animationJsonRows = New-Object System.Collections.Generic.List[object]
  $copied = New-Object System.Collections.Generic.List[object]

  foreach ($monster in $monsters) {
    $monsterKey = $monster.MonsterKey
    $resolved = Resolve-MonsterSpriteAssets $monster $projectDir $guidIndex
    $inheritedFrom = ""

    $baseKey = Get-BossBaseMonsterKey $monsterKey
    if ($baseKey -and $monsterByKey.ContainsKey($baseKey)) {
      $baseResolved = Resolve-MonsterSpriteAssets $monsterByKey[$baseKey] $projectDir $guidIndex
      if ($baseResolved.textureCount -gt 0) {
        $resolved = Merge-MonsterResolvedAssets $baseResolved $resolved
        $inheritedFrom = $baseKey
      }
    }

    foreach ($relativeTexture in $resolved.textures) {
      $entry = Copy-MonsterTexture $projectDir $packDir $relativeTexture
      if ($entry) {
        $copied.Add($entry) | Out-Null
      }
    }

    $nameKey = ""
    if ($monster.PSObject.Properties.Name -contains "MonsterNameStringKey") {
      $nameKey = $monster.MonsterNameStringKey
    } elseif ($monster.PSObject.Properties.Name -contains "NameKey") {
      $nameKey = $monster.NameKey
    }

    $rows.Add([pscustomobject]@{
      MonsterKey = $monsterKey
      NameKey = $nameKey
      InheritedFrom = $inheritedFrom
      Controller = $resolved.controller
      ClipCount = $resolved.clipCount
      SpriteCount = $resolved.spriteCount
      TextureCount = $resolved.textureCount
      Textures = ($resolved.textures -join ";")
      Clips = (($resolved.clips | ForEach-Object { $_.clip }) -join ";")
    }) | Out-Null

    $animationJsonRows.Add([pscustomobject]@{
      monsterKey = $monsterKey
      nameKey = $nameKey
      inheritedFrom = $inheritedFrom
      animations = @($resolved.clips)
    }) | Out-Null

    foreach ($clip in $resolved.clips) {
      $animationRows.Add([pscustomobject]@{
        MonsterKey = $monsterKey
        NameKey = $nameKey
        InheritedFrom = $inheritedFrom
        Kind = $clip.kind
        Clip = $clip.clip
        Source = $clip.source
        FrameCount = $clip.frameCount
        Textures = ($clip.textures -join ";")
        Sprites = ($clip.sprites -join ";")
      }) | Out-Null
    }
  }

  $dataDir = Join-Path $packDir "data"
  Ensure-Dir $dataDir
  $jsonRows = @($rows | ForEach-Object {
    [pscustomobject]@{
      monsterKey = $_.MonsterKey
      nameKey = $_.NameKey
      inheritedFrom = $_.InheritedFrom
      controller = $_.Controller
      clipCount = $_.ClipCount
      spriteCount = $_.SpriteCount
      textureCount = $_.TextureCount
      textures = @($_.Textures -split ";" | Where-Object { $_ })
      clips = @($_.Clips -split ";" | Where-Object { $_ })
    }
  })

  $rows | Export-Csv -NoTypeInformation -Encoding UTF8 -Path (Join-Path $dataDir "monster-sprite-map.csv")
  $jsonRows | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -Path (Join-Path $dataDir "monster-sprite-map.json")
  $animationRows | Export-Csv -NoTypeInformation -Encoding UTF8 -Path (Join-Path $dataDir "monster-animation-map.csv")
  $animationJsonRows | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -Path (Join-Path $dataDir "monster-animation-map.json")

  $rowArray = @($rows.ToArray())
  $copiedArray = @($copied.ToArray() | Sort-Object relative -Unique)

  return [pscustomobject]@{
    rows = $rowArray
    copied = $copiedArray
  }
}

function New-ContactSheetPage([System.IO.FileInfo[]]$files, [string]$outPath, [string]$title) {
  if ($files.Count -eq 0) {
    return
  }

  Add-Type -AssemblyName System.Drawing

  $thumb = 96
  $label = 34
  $pad = 10
  $header = 38
  $cols = 6
  $rows = [Math]::Ceiling($files.Count / $cols)
  $cellW = $thumb + ($pad * 2)
  $cellH = $thumb + $label + ($pad * 2)
  $width = $cols * $cellW
  $height = $header + ($rows * $cellH)

  $bitmap = New-Object System.Drawing.Bitmap $width, $height
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.Clear([System.Drawing.Color]::FromArgb(18, 24, 34))
  $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

  $titleFont = New-Object System.Drawing.Font "Arial", 14, ([System.Drawing.FontStyle]::Bold)
  $labelFont = New-Object System.Drawing.Font "Arial", 7
  $titleBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(235, 242, 255))
  $labelBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(190, 203, 220))
  $borderPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(62, 74, 92))

  $graphics.DrawString($title, $titleFont, $titleBrush, 12, 10)

  for ($i = 0; $i -lt $files.Count; $i++) {
    $file = $files[$i]
    $col = $i % $cols
    $row = [Math]::Floor($i / $cols)
    $x = ($col * $cellW) + $pad
    $y = $header + ($row * $cellH) + $pad

    $graphics.DrawRectangle($borderPen, $x, $y, $thumb, $thumb)

    $image = $null
    try {
      $image = [System.Drawing.Image]::FromFile($file.FullName)
      $scale = [Math]::Min($thumb / $image.Width, $thumb / $image.Height)
      $drawW = [Math]::Max(1, [Math]::Round($image.Width * $scale))
      $drawH = [Math]::Max(1, [Math]::Round($image.Height * $scale))
      $drawX = $x + [Math]::Floor(($thumb - $drawW) / 2)
      $drawY = $y + [Math]::Floor(($thumb - $drawH) / 2)
      $graphics.DrawImage($image, $drawX, $drawY, $drawW, $drawH)
    } catch {
      $graphics.DrawString("erro", $labelFont, $labelBrush, $x + 6, $y + 38)
    } finally {
      if ($image) {
        $image.Dispose()
      }
    }

    $short = $file.BaseName
    if ($short.Length -gt 24) {
      $short = $short.Substring(0, 21) + "..."
    }
    $graphics.DrawString($short, $labelFont, $labelBrush, $x, $y + $thumb + 4)
  }

  Ensure-Dir (Split-Path -Parent $outPath)
  $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)

  $borderPen.Dispose()
  $titleBrush.Dispose()
  $labelBrush.Dispose()
  $titleFont.Dispose()
  $labelFont.Dispose()
  $graphics.Dispose()
  $bitmap.Dispose()
}

function New-ContactSheets([string]$packDir) {
  $created = New-Object System.Collections.Generic.List[object]
  $imageRoot = Join-Path $packDir "images"
  $sheetRoot = Join-Path $packDir "contact-sheets"
  if (-not (Test-Path $imageRoot)) {
    return $created
  }

  $pageSize = 72
  $buckets = Get-ChildItem -LiteralPath $imageRoot -Directory
  foreach ($bucket in $buckets) {
    $files = @(Get-ChildItem -LiteralPath $bucket.FullName -File -Filter *.png | Sort-Object Name)
    if ($files.Count -eq 0) {
      continue
    }

    $page = 1
    for ($start = 0; $start -lt $files.Count; $start += $pageSize) {
      $count = [Math]::Min($pageSize, $files.Count - $start)
      $slice = @($files[$start..($start + $count - 1)])
      $sheetName = "{0}-{1:00}.png" -f $bucket.Name, $page
      $sheetPath = Join-Path $sheetRoot $sheetName
      New-ContactSheetPage $slice $sheetPath "TaskBarHero $($bucket.Name) $page"
      $created.Add([pscustomobject]@{
        bucket = $bucket.Name
        page = $page
        count = $count
        path = $sheetPath.Substring($packDir.Length).TrimStart('\')
      }) | Out-Null
      $page++
    }
  }

  return $created
}

function Organize-ExportedProject([string]$projectDir, [string]$versionDir, [string]$version, [string]$gameDir) {
  $assetsRoot = Join-Path $projectDir "Assets"
  if (-not (Test-Path $assetsRoot)) {
    Fail "ExportedProject invalido: nao encontrei a pasta Assets em $projectDir"
  }

  $packDir = Join-Path $versionDir "frontend-pack"
  if ((Test-Path $packDir) -and -not $Force) {
    Fail "Ja existe frontend-pack em $packDir. Use -Force para recriar."
  }

  Reset-Dir $packDir $versionDir
  Ensure-PortableTree $packDir

  Write-Host "Organizando ExportedProject:"
  Write-Host "  source: $projectDir"
  Write-Host "  pack:   $packDir"

  Write-Host "  imagens..."
  $images = Copy-PortableImages $projectDir $packDir

  Write-Host "  mapa de sprites dos monstros..."
  $monsterSpriteMap = New-MonsterSpriteMap $projectDir $packDir

  Write-Host "  audio..."
  $audio = Copy-PortableAudio $projectDir $packDir

  Write-Host "  dados CSV/JSON..."
  $data = Convert-TextAssetCsvs (Join-Path $projectDir "Assets\TextAsset") $packDir

  Write-Host "  localization..."
  $copiedLocalization = @(Copy-Localization $projectDir $packDir)
  $localizationBundles = @(Export-LocalizationBundles $gameDir $packDir)
  $localization = @($copiedLocalization + $localizationBundles)

  $sheets = @()
  if (-not $NoContactSheets) {
    Write-Host "  contact sheets..."
    try {
      $sheets = New-ContactSheets $packDir
    } catch {
      Write-Warning "Nao consegui gerar contact sheets: $($_.Exception.Message)"
    }
  }

  $manifest = [pscustomobject]@{
    game = "TaskBarHero"
    gameVersion = $version
    gameDir = $gameDir
    exportedProject = $projectDir
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    frontendPack = $packDir
    counts = [pscustomobject]@{
      images = @($images).Count
      monsterSpriteRows = @($monsterSpriteMap.rows).Count
      monsterTextures = @($monsterSpriteMap.copied).Count
      audio = @($audio).Count
      dataSets = @($data).Count
      localizationFiles = @($localization).Count
      contactSheets = @($sheets).Count
    }
    images = $images
    monsterTextures = $monsterSpriteMap.copied
    audio = $audio
    data = $data
    localization = $localization
    contactSheets = $sheets
  }

  $manifestPath = Join-Path $packDir "manifest.json"
  $manifest | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 -Path $manifestPath

  Write-Host ""
  Write-Host "Frontend pack concluido."
  Write-Host "  pack:     $packDir"
  Write-Host "  manifest: $manifestPath"
  Write-Host "  imagens:  $(@($images).Count)"
  Write-Host "  audio:    $(@($audio).Count)"
  Write-Host "  datasets: $(@($data).Count)"
  Write-Host "  sheets:   $(@($sheets).Count)"
  Write-Host "  i18n:     $(@($localizationBundles).Count)"
}

$resolvedGameDir = Find-GameDir $GameDir
$version = Get-GameVersion $resolvedGameDir
$safeVersion = ConvertTo-SafeName $version
$selectedOutDir = $DefaultOutDir
if ($OutDir) {
  $selectedOutDir = $OutDir
}
$resolvedOutDir = Resolve-FullPath $selectedOutDir
$versionDir = Join-Path $resolvedOutDir "TaskBarHero-$safeVersion"
$rawDir = Join-Path $versionDir "raw"
$assetsDir = Join-Path $versionDir "assets"
$manifestPath = Join-Path $versionDir "manifest.json"
$logPath = Join-Path $versionDir "export-log.txt"

Write-Host "TaskBarHero assets export"
Write-Host "  game:    $resolvedGameDir"
Write-Host "  version: $version"
Write-Host "  out:     $versionDir"
Write-Host "  guard:   $(if ($EnforceVersionGuard) { 'enabled' } else { 'disabled' })"

if ($OrganizeExportedProject) {
  $resolvedProject = Resolve-FullPath $OrganizeExportedProject
  if (-not (Test-Path $resolvedProject)) {
    Fail "OrganizeExportedProject nao encontrado: $resolvedProject"
  }

  Ensure-Dir $resolvedOutDir
  if ((Test-Path $versionDir) -and $EnforceVersionGuard -and -not $Force) {
    Fail "Ja existe export para a versao $version em $versionDir. Use -Force ou rode sem -EnforceVersionGuard enquanto valida."
  }
  Ensure-Dir $versionDir
  Organize-ExportedProject $resolvedProject $versionDir $version $resolvedGameDir
  exit 0
}

if (-not $Enable) {
  Write-Host ""
  Write-Host "Exportacao desativada. Nada foi escrito."
  Write-Host "Fluxo GUI recomendado:"
  Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1 -OrganizeExportedProject C:\caminho\ExportedProject"
  Write-Host ""
  Write-Host "Fluxo CLI experimental:"
  Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1 -Enable -ExporterExe C:\tools\AssetRipper.CLI.exe"
  Write-Host "Se o CLI do exportador usar outros argumentos, ajuste com -ExporterArgs."
  Write-Host "Quando quiser travar repeticao por versao, adicione -EnforceVersionGuard."
  exit 0
}

if (-not $ExporterExe) {
  Fail "Passe -ExporterExe apontando para o exportador Unity/AssetRipper CLI."
}

$resolvedExporter = Resolve-FullPath $ExporterExe
if (-not (Test-Path $resolvedExporter)) {
  Fail "ExporterExe nao encontrado: $resolvedExporter"
}

Ensure-Dir $resolvedOutDir
if ((Test-Path $versionDir) -and $EnforceVersionGuard -and -not $Force) {
  Fail "Ja existe export para a versao $version em $versionDir. Use -Force ou rode sem -EnforceVersionGuard enquanto valida."
}

if ((Test-Path $versionDir) -and $Force) {
  Assert-ChildPath $resolvedOutDir $versionDir
  Remove-Item -LiteralPath $versionDir -Recurse -Force
}

Ensure-Dir $versionDir
Ensure-Dir $rawDir
Ensure-Dir $assetsDir
Ensure-RawTree $assetsDir

$expandedArgs = Expand-ExporterArgs $ExporterArgs $resolvedGameDir $rawDir $versionDir $version
Write-Host ""
Write-Host "Rodando exportador:"
Write-Host "  exe:  $resolvedExporter"
Write-Host "  args: $($expandedArgs -join ' ')"

& $resolvedExporter @expandedArgs 2>&1 | Tee-Object -FilePath $logPath
if ($LASTEXITCODE -ne 0) {
  Fail "Exportador falhou com exit code $LASTEXITCODE. Veja $logPath"
}

$assetEntries = @()
if (-not $NoOrganize) {
  Write-Host ""
  Write-Host "Organizando arquivos exportados..."
  $assetEntries = Organize-RawExport $rawDir $assetsDir
  Write-Host "  arquivos: $($assetEntries.Count)"
} else {
  Write-Host "Organizacao pulada (-NoOrganize)."
}

$manifest = [pscustomobject]@{
  game = "TaskBarHero"
  gameDir = $resolvedGameDir
  gameVersion = $version
  exportedAt = (Get-Date).ToUniversalTime().ToString("o")
  exporterExe = $resolvedExporter
  exporterArgs = $expandedArgs
  versionGuardEnabled = [bool]$EnforceVersionGuard
  rawDir = $rawDir
  assetsDir = $assetsDir
  files = $assetEntries
}

$manifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -Path $manifestPath

Write-Host ""
Write-Host "Export concluido."
Write-Host "  raw:      $rawDir"
Write-Host "  assets:   $assetsDir"
Write-Host "  manifest: $manifestPath"
