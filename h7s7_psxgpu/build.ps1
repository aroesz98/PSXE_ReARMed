# Build / flash helper for the PSX GPU board (Windows PowerShell).
#
#   .\build.ps1              - incremental build (MCUboot + application, sysbuild)
#   .\build.ps1 -Pristine    - clean build
#   .\build.ps1 -Flash       - build and flash both images (ST-LINK, STM32CubeProgrammer)
#   .\build.ps1 -FlashApp    - flash only the application (MCUboot already on the board)
#   .\build.ps1 -NemaRoot C:\TouchGFX\4.26.1\touchgfx_components\gpu2d\NemaGFX_NemaP_m7_r01
#
param(
    [switch]$Pristine,
    [switch]$Flash,
    [switch]$FlashApp,
    [string]$Workspace = "$env:USERPROFILE\zephyrproject",
    [string]$NemaRoot = ""
)

# west/cmake/ninja print progress on stderr: with "Stop", Windows PowerShell 5.1
# would turn that into a terminating error as soon as the output is redirected.
# Native tools are checked through $LASTEXITCODE instead.
$ErrorActionPreference = "Continue"
$app = $PSScriptRoot
$build = Join-Path $app "build"
$west = Join-Path $Workspace ".venv\Scripts\west.exe"
if (-not (Test-Path $west)) { $west = "west" }
$env:ZEPHYR_BASE = Join-Path $Workspace "zephyr"

$wargs = @("build", "-b", "stm32h7s78_dk/stm32h7s7xx/ext_flash_app", "--sysbuild",
          "-s", $app, "-d", $build)
if ($Pristine) { $wargs += @("-p", "always") }
# picked up by CMakeLists.txt on the first (pristine) configure
if ($NemaRoot) { $env:NEMAGFX_ROOT = $NemaRoot; $wargs += @("-p", "always") }

Push-Location $Workspace
try {
    & $west @wargs
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    if ($Flash) {
        & $west flash -d $build
        if ($LASTEXITCODE -ne 0) { throw "flash failed" }
    } elseif ($FlashApp) {
        & $west flash -d $build --domain h7s7_psxgpu
        if ($LASTEXITCODE -ne 0) { throw "flash failed" }
    }
} finally {
    Pop-Location
}
