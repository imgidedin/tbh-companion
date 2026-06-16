param(
    [string]$GhidraHome = "E:\Outros\ghidra_12.1.2_PUBLIC",
    [string]$GameAssembly = "E:\SteamLibrary\steamapps\common\TaskbarHero\GameAssembly.dll",
    [string]$ScriptJson = "",
    [string]$ProjectRoot = "",
    [string]$ProjectName = "TaskBarHero-Il2Cpp-1.00.13",
    [string]$JavaHome = "",
    [int]$AnalysisTimeoutSeconds = 0,
    [switch]$CreateFunctions,
    [switch]$Fresh
)

$ErrorActionPreference = "Stop"

if (-not $ScriptJson) {
    $ScriptJson = Join-Path $PSScriptRoot ".cache\dump\script.json"
}

if (-not $ProjectRoot) {
    $ProjectRoot = Join-Path $PSScriptRoot "ghidra-projects-cache"
}

$headless = Join-Path $GhidraHome "support\analyzeHeadless.bat"
$scriptDir = Join-Path $PSScriptRoot "ghidra"
$reportPath = Join-Path $ProjectRoot "$ProjectName-report.md"
$logPath = Join-Path $ProjectRoot "$ProjectName-headless.log"
$mode = if ($CreateFunctions) { "functions" } else { "labels-only" }

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

foreach ($path in @($headless, $GameAssembly, $ScriptJson, $scriptDir)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required path does not exist: $path"
    }
}

New-Item -ItemType Directory -Force -Path $ProjectRoot | Out-Null

$argsList = @(
    $ProjectRoot,
    $ProjectName,
    "-scriptPath", $scriptDir,
    "-log", $logPath,
    "-import", $GameAssembly,
    "-processor", "x86:LE:64:default",
    "-cspec", "windows"
)

if ($Fresh) {
    Write-Host "Fresh requested: using import overwrite; not passing Ghidra -deleteProject because it creates a temporary project."
}
$argsList += "-overwrite"

if ($AnalysisTimeoutSeconds -le 0) {
    $argsList += "-noanalysis"
}
else {
    $argsList += @("-analysisTimeoutPerFile", [string]$AnalysisTimeoutSeconds)
}

$argsList += @(
    "-postScript", "ApplyIl2CppDumperLabelsHeadless.java",
    $ScriptJson,
    $mode,
    $reportPath
)

Write-Host "Ghidra: $headless"
Write-Host "Project: $ProjectRoot\$ProjectName"
Write-Host "GameAssembly: $GameAssembly"
Write-Host "Script JSON: $ScriptJson"
Write-Host "JAVA_HOME: $env:JAVA_HOME"
Write-Host "Mode: $mode"
Write-Host "Report: $reportPath"
Write-Host "Log: $logPath"

& $headless @argsList
if ($LASTEXITCODE -ne 0) {
    throw "Ghidra analyzeHeadless failed with exit code $LASTEXITCODE"
}

Write-Host "Done."
