#Requires -Version 5.1
<#
.SYNOPSIS
    Ember - one-command local build for Windows.

.DESCRIPTION
    Builds the Ember plug-in (VST3 + Standalone) through the presets in
    CMakePresets.json, so this script and CI build exactly the same way.
    Uses the windows-release / windows-debug presets, which both use the
    Visual Studio 17 2022 generator targeting x64.

    macOS and Linux use scripts/build.sh instead.

.PARAMETER Configuration
    Release (default) or Debug. NOTE: this script deliberately does NOT expose a
    -Debug switch. -Debug is one of PowerShell's automatic common parameters
    (added by [CmdletBinding()], it drives $DebugPreference), so declaring our
    own -Debug would be a hard collision and PowerShell would refuse to bind it.
    Use "-Configuration Debug", or the short "-Dbg" switch below.

.PARAMETER Dbg
    Shorthand for -Configuration Debug. Named "Dbg" for the same reason.

.PARAMETER NoTests
    Skip running ctest after the build. The test targets are still compiled;
    this only skips executing them.

.PARAMETER Clean
    Delete the preset's build directory before configuring.

.PARAMETER Jobs
    Parallel compile jobs. Defaults to the number of logical processors.

.PARAMETER Pluginval
    Run scripts/run-pluginval.sh after a successful build (needs bash - Git for
    Windows ships one).

.PARAMETER Help
    Show usage and exit.

.EXAMPLE
    .\scripts\build.ps1
.EXAMPLE
    .\scripts\build.ps1 -Dbg -NoTests
.EXAMPLE
    .\scripts\build.ps1 -Clean -Pluginval -Jobs 16
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [switch]$Dbg,
    [switch]$NoTests,
    [switch]$Clean,

    [ValidateRange(0, 1024)]
    [int]$Jobs = 0,

    [switch]$Pluginval,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot

function Write-Step  { param([string]$Message) Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Warn  { param([string]$Message) Write-Host "==> warning: $Message" -ForegroundColor Yellow }
function Write-Fail  { param([string]$Message) Write-Host "==> error: $Message" -ForegroundColor Red; exit 1 }

function Show-Usage {
    @'
Ember build script (Windows)

usage: .\scripts\build.ps1 [options]

  -Configuration <Release|Debug>  Which preset to build (default: Release).
                                  -Debug is a PowerShell common parameter, so the
                                  debug build is selected with -Configuration Debug
                                  or the -Dbg switch.
  -Dbg                            Shorthand for -Configuration Debug.
  -NoTests                        Skip running ctest after the build.
  -Clean                          Delete the preset's build directory first.
  -Jobs N                         Parallel compile jobs (default: CPU count).
  -Pluginval                      Run scripts\run-pluginval.sh afterwards (needs bash).
  -Help                           Show this help and exit.

Presets (CMakePresets.json): windows-release (default) | windows-debug
  Generator: Visual Studio 17 2022, architecture x64.

Build directory: build\<preset>\
Artefacts:       build\<preset>\Ember_artefacts\<config>\{VST3,Standalone}\
'@ | Write-Host
}

if ($Help) { Show-Usage; exit 0 }

if ($Dbg) { $Configuration = 'Debug' }

$Preset = if ($Configuration -eq 'Debug') { 'windows-debug' } else { 'windows-release' }

$BuildDir    = Join-Path $Root "build\$Preset"
$ArtefactDir = Join-Path $BuildDir "Ember_artefacts\$Configuration"

# --------------------------------------------------------------- toolchain checks
# $ErrorActionPreference = 'Stop' does not turn a non-zero exit code from a native
# program into a terminating error, so every cmake/ctest call goes through this.
function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$What = 'command'
    )
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        Write-Fail "$What failed (exit code $LASTEXITCODE): $Exe $($Arguments -join ' ')"
    }
}

$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeCmd) {
    Write-Fail @'
cmake was not found on PATH.

Install it with one of:
    winget install --id Kitware.CMake
    choco install cmake --installargs 'ADD_CMAKE_TO_PATH=System'

or download the installer from https://cmake.org/download/ and tick
"Add CMake to the system PATH". Ember needs CMake 3.22 or newer.
Open a new terminal afterwards so the updated PATH is picked up.
'@
}

$ctestCmd = Get-Command ctest -ErrorAction SilentlyContinue
if (-not $ctestCmd -and -not $NoTests) {
    Write-Fail "ctest was not found on PATH even though cmake was ($($cmakeCmd.Source)). Re-install CMake, or pass -NoTests."
}

# No 2>&1 here on purpose: with $ErrorActionPreference = 'Stop', redirecting a
# native program's stderr into the success stream turns any stray stderr line
# into a terminating NativeCommandError on Windows PowerShell 5.1.
$versionLine = (& cmake --version | Select-Object -First 1)
$versionText = [string]$versionLine
$match = [regex]::Match($versionText, '(\d+)\.(\d+)(?:\.(\d+))?')
if (-not $match.Success) {
    Write-Fail "could not parse the version reported by $($cmakeCmd.Source): '$versionText'. Ember needs CMake 3.22 or newer."
}
$cmMajor = [int]$match.Groups[1].Value
$cmMinor = [int]$match.Groups[2].Value
$cmVersion = $match.Value

if ($cmMajor -lt 3 -or ($cmMajor -eq 3 -and $cmMinor -lt 22)) {
    Write-Fail @"
CMake $cmVersion is too old - Ember needs 3.22 or newer
(CMakePresets.json v3 and 'cmake --preset' require it).

Upgrade with:
    winget upgrade --id Kitware.CMake
    choco upgrade cmake

Found: $($cmakeCmd.Source)
"@
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Fail @'
git was not found on PATH. JUCE 8.0.15 is fetched at configure time with
FetchContent, which shells out to git.

Install it with:
    winget install --id Git.Git
'@
}

# Soft check only: the presets use the Visual Studio 17 2022 generator, and a
# missing toolchain otherwise shows up as a confusing CMake error much later.
$pf86 = ${env:ProgramFiles(x86)}
$vsWhere = if ($pf86) { Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe' } else { '' }
if (-not ($vsWhere -and (Test-Path $vsWhere)) -and -not $env:VSINSTALLDIR) {
    Write-Warn @'
Visual Studio 2022 was not detected. The windows-* presets use the
"Visual Studio 17 2022" generator, which needs VS 2022 (any edition) or the
Build Tools with the "Desktop development with C++" workload:
    winget install --id Microsoft.VisualStudio.2022.BuildTools
Continuing anyway - CMake will report the real error if it is genuinely absent.
'@
}

if ($Jobs -le 0) {
    $Jobs = [int]$env:NUMBER_OF_PROCESSORS
    if ($Jobs -le 0) { $Jobs = 4 }
}

# --------------------------------------------------------------- build
Write-Step "Ember 1.0.0 - preset $Preset ($Configuration), $Jobs job(s)"
Write-Host "    cmake $cmVersion, source $Root" -ForegroundColor DarkGray

Push-Location $Root
try {
    if ($Clean) {
        # Deliberately paranoid: only ever remove build\<preset> inside the repo.
        $expected = Join-Path $Root 'build'
        if (-not $BuildDir.StartsWith($expected, [System.StringComparison]::OrdinalIgnoreCase)) {
            Write-Fail "refusing to clean suspicious path: $BuildDir"
        }
        if (Test-Path $BuildDir) {
            Write-Step "cleaning $BuildDir"
            Remove-Item -LiteralPath $BuildDir -Recurse -Force
        }
        else {
            Write-Step "nothing to clean ($BuildDir does not exist)"
        }
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    Write-Step 'configuring'
    Invoke-Checked -Exe 'cmake' -Arguments @('--preset', $Preset) -What 'cmake configure'

    Write-Step 'building'
    Invoke-Checked -Exe 'cmake' `
        -Arguments @('--build', '--preset', $Preset, '--parallel', "$Jobs") `
        -What 'cmake build'

    $stopwatch.Stop()
    $elapsed = [int]$stopwatch.Elapsed.TotalSeconds

    # ----------------------------------------------------------- tests
    $testResult = 'skipped (-NoTests)'
    if (-not $NoTests) {
        Write-Step 'running tests'
        # windows-debug has no matching test preset, so fall back to running
        # ctest directly in the preset's build directory when there is none.
        $presetList = ''
        try { $presetList = (& ctest --list-presets 2>$null) -join "`n" } catch { $presetList = '' }

        if ($presetList -like ('*"' + $Preset + '"*')) {
            Invoke-Checked -Exe 'ctest' `
                -Arguments @('--preset', $Preset, '--parallel', "$Jobs") -What 'ctest'
        }
        else {
            Write-Warn "no ctest preset named '$Preset' in CMakePresets.json - running ctest directly in $BuildDir"
            Invoke-Checked -Exe 'ctest' `
                -Arguments @('--test-dir', $BuildDir, '-C', $Configuration,
                             '--output-on-failure', '--parallel', "$Jobs") -What 'ctest'
        }
        $testResult = 'passed'
    }

    # ----------------------------------------------------------- artefacts
    # If the configuration directory is not where we expect it, fall back to
    # whatever config dir actually exists so the paths printed are never a lie.
    if (-not (Test-Path $ArtefactDir)) {
        # NB: not $root - PowerShell variable names are case-insensitive, so that
        # would silently clobber $Root (the repo root) used further down.
        $artefactRoot = Join-Path $BuildDir 'Ember_artefacts'
        if (Test-Path $artefactRoot) {
            $candidate = Get-ChildItem -LiteralPath $artefactRoot -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -ne 'JuceLibraryCode' } |
                Select-Object -First 1
            if ($candidate) { $ArtefactDir = $candidate.FullName }
        }
    }

    Write-Host ''
    Write-Host "==> build finished in ${elapsed}s (tests: $testResult)" -ForegroundColor Green
    Write-Host 'Artefacts:'

    $artefacts = [ordered]@{
        'VST3'       = Join-Path $ArtefactDir 'VST3\Ember.vst3'
        'Standalone' = Join-Path $ArtefactDir 'Standalone\Ember.exe'
    }

    $foundAny = $false
    foreach ($name in $artefacts.Keys) {
        $path = $artefacts[$name]
        if (Test-Path $path) {
            $foundAny = $true
            Write-Host ("  {0,-12} {1}" -f $name, $path) -ForegroundColor Green
        }
        else {
            Write-Host ("  {0,-12} {1} (not built)" -f $name, $path) -ForegroundColor DarkGray
        }
    }

    if (-not $foundAny) {
        Write-Warn "no artefacts found under $ArtefactDir - check the build output above"
    }

    Write-Host ''
    Write-Host 'Install locally (optional):'
    Write-Host ('  Copy-Item -Recurse -Force "{0}" "$env:CommonProgramFiles\VST3\"' -f $artefacts['VST3'])
    Write-Host '  (needs an elevated shell)'

    # ----------------------------------------------------------- pluginval
    if ($Pluginval) {
        Write-Host ''
        Write-Step 'running pluginval'
        $pv = Join-Path $Root 'scripts\run-pluginval.sh'
        if (-not (Test-Path $pv)) { Write-Fail "scripts\run-pluginval.sh not found at $pv" }

        $bash = Get-Command bash -ErrorAction SilentlyContinue
        if (-not $bash) {
            Write-Fail @"
run-pluginval.sh is a bash script and no bash was found on PATH.
Install Git for Windows (winget install --id Git.Git) and re-run, or run it
by hand from a Git Bash prompt:
    bash scripts/run-pluginval.sh
"@
        }
        Invoke-Checked -Exe $bash.Source -Arguments @($pv) -What 'pluginval'
    }
}
finally {
    Pop-Location
}
