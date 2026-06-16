param(
    [string]$GhidraHome = "E:\Outros\ghidra_12.1.2_PUBLIC",
    [string]$ScriptJson = "",
    [string]$ProjectRoot = "",
    [string]$ProjectName = "TaskBarHero-Il2Cpp-1.00.13",
    [string]$JavaHome = "",
    [int]$MaxMethods = 350
)

$ErrorActionPreference = "Stop"

if (-not $ScriptJson) {
    $ScriptJson = Join-Path $PSScriptRoot ".cache\dump\script.json"
}

if (-not $ProjectRoot) {
    $ProjectRoot = Join-Path $PSScriptRoot "ghidra-projects-cache"
}

if (-not $JavaHome) {
    if ($env:JAVA_HOME -and (Test-Path -LiteralPath (Join-Path $env:JAVA_HOME "bin\java.exe"))) {
        $JavaHome = $env:JAVA_HOME
    }
    else {
        $adoptiumRoot = "C:\Program Files\Eclipse Adoptium"
        if (Test-Path -LiteralPath $adoptiumRoot) {
            $candidate = Get-ChildItem -Path $adoptiumRoot -Directory -Filter "jdk*" |
                Sort-Object Name -Descending |
                Select-Object -First 1
            if ($candidate) {
                $JavaHome = $candidate.FullName
            }
        }
    }
}

if ($JavaHome) {
    $javaExe = Join-Path $JavaHome "bin\java.exe"
    if (-not (Test-Path -LiteralPath $javaExe)) {
        throw "JavaHome does not contain bin\java.exe: $JavaHome"
    }
    $env:JAVA_HOME = $JavaHome
    $env:Path = "$JavaHome\bin;$env:Path"
}

$headless = Join-Path $GhidraHome "support\analyzeHeadless.bat"
$scriptDir = Join-Path $PSScriptRoot "ghidra"
$outputDir = Join-Path $ProjectRoot "decompiled-combat"
$logPath = Join-Path $ProjectRoot "$ProjectName-decompile.log"
$projectFile = Join-Path $ProjectRoot "$ProjectName.gpr"

foreach ($path in @($headless, $ScriptJson, $scriptDir, $projectFile)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required path does not exist: $path"
    }
}

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$argsList = @(
    $ProjectRoot,
    $ProjectName,
    "-scriptPath", $scriptDir,
    "-log", $logPath,
    "-process", "GameAssembly.dll",
    "-noanalysis",
    "-postScript", "DecompileIl2CppCombatTargets.java",
    $ScriptJson,
    $outputDir,
    [string]$MaxMethods
)

Write-Host "Ghidra: $headless"
Write-Host "Project: $ProjectRoot\$ProjectName"
Write-Host "Script JSON: $ScriptJson"
Write-Host "JAVA_HOME: $env:JAVA_HOME"
Write-Host "Output: $outputDir"
Write-Host "MaxMethods: $MaxMethods"
Write-Host "Log: $logPath"

& $headless @argsList
if ($LASTEXITCODE -ne 0) {
    throw "Ghidra combat decompile failed with exit code $LASTEXITCODE"
}

Write-Host "Done."
