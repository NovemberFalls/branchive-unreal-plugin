# Build + run the standalone Branchive Cloud sign-in core unit test with MSVC (cl.exe).
# Run from a "x64 Native Tools Command Prompt for VS 2022" (or after vcvars64.bat)
# so cl.exe is on PATH. Compiles the SAME LorePkce.cpp / LoreLoopback.cpp /
# LoreConfigPin.cpp translation units the UE module builds.
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Lore = Join-Path $Here "..\..\Source\BranchiveSourceControl\Private\Lore"
$Out = Join-Path $Here "auth_test.exe"

Write-Host "Compiling auth_test with cl.exe ..."
& cl.exe /nologo /std:c++17 /EHsc /I "$Lore" `
    (Join-Path $Here "auth_test.cpp") `
    (Join-Path $Lore "LorePkce.cpp") `
    (Join-Path $Lore "LoreLoopback.cpp") `
    (Join-Path $Lore "LoreConfigPin.cpp") `
    /Fe:"$Out" `
    /link ws2_32.lib crypt32.lib
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Running auth_test ..."
& $Out
exit $LASTEXITCODE
