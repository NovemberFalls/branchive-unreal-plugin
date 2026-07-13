# Build + run the standalone Lore parser fixture test with MSVC (cl.exe).
# Run from a "x64 Native Tools Command Prompt for VS 2022" (or after vcvars64.bat)
# so cl.exe is on PATH.
param(
    [string]$FixturesDir
)

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Lore = Join-Path $Here "..\..\Source\BranchiveSourceControl\Private\Lore"
if (-not $FixturesDir) {
    $FixturesDir = Join-Path $Here "..\..\..\contract\fixtures"
}
$Out = Join-Path $Here "parser_test.exe"

Write-Host "Compiling with cl.exe ..."
& cl.exe /nologo /std:c++17 /EHsc /I "$Lore" `
    (Join-Path $Here "parser_test.cpp") `
    (Join-Path $Lore "LoreParse.cpp") `
    (Join-Path $Lore "LoreErrors.cpp") `
    /Fe:"$Out"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Running against fixtures: $FixturesDir"
& $Out $FixturesDir
exit $LASTEXITCODE
