<#
.SYNOPSIS
    Builds psxe_embedded from the command line, with the toolchain of an installed MCUXpresso IDE.

.DESCRIPTION
    The makefiles in Release\ are not part of the repository: MCUXpresso IDE generates them from
    .cproject. So this script
      - lets the IDE itself generate them and build - headless, in a workspace of its own under
        %TEMP%, which works while the IDE is open - when they are missing or out of date (.cproject
        is newer, a source file was added or removed) or when -Clean or -Regenerate is given;
      - otherwise runs make on them, as the IDE does for an incremental build.

    -Sound (PSXE\sound_switch.h) and -Define override the build switches, which are all guarded
    with #ifndef: PSX_PROFILE=1, PSXE_AUTOTEST=1, PSXE_FRAME_LIMIT=0, ... Such a variant is always
    built from scratch and kept as Release\psxe_embedded_<variant>.axf / .bin / .map; its objects
    and makefiles are removed afterwards, so that the next ordinary build - here or in the IDE -
    cannot pick them up.

    The full output goes to Release\build.log; the console shows the files being built, warnings,
    errors and the memory use.

.EXAMPLE
    build.bat                         incremental build of Release
.EXAMPLE
    build.bat -Flash                  the same, then flashed to the board (the IDE must not be debugging)
.EXAMPLE
    build.bat -Clean                  everything from scratch
.EXAMPLE
    build.bat -Sound 0 -Flash         without the SPU voices (see PSXE\sound_switch.h), flashed
.EXAMPLE
    build.bat -Define PSX_PROFILE=1   the profiling build
#>
[CmdletBinding()]
param(
    # PSXE_SOUND for this build: 0, 1 or 2 (PSXE\sound_switch.h); by default what the header says
    [ValidateRange(-1, 2)] [int] $Sound = -1,

    # further NAME=VALUE switches for this build
    [string[]] $Define = @(),

    # build everything from scratch
    [switch] $Clean,

    # let the IDE write the makefiles again even though they look up to date
    [switch] $Regenerate,

    # flash the result with LinkServer
    [switch] $Flash,

    [int] $Jobs = [Environment]::ProcessorCount,

    # the MCUXpresso IDE (the folder with mcuxpressoidec.exe, or the one above it); by default the newest
    # one in C:\nxp
    [string] $Ide = $env:MCUXPRESSO_IDE,

    [string] $Probe = "MIMXRT1052B:EVKB-IMXRT1050"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2

$Project = $PSScriptRoot
$Config = "Release" # the Debug configuration in .cproject is out of date (no PSXE sources)
$BuildDir = Join-Path $Project $Config
$ProjectName = ([xml](Get-Content -Raw (Join-Path $Project ".project"))).projectDescription.name
$script:LogWriter = $null
$script:LastErrors = -1

# written next to the makefiles when this script had them generated: the hash of the .cproject they come from
$Stamp = Join-Path $BuildDir ".cproject.sha256"

function Get-ProjectHash
{
    return (Get-FileHash -Algorithm SHA256 (Join-Path $Project ".cproject")).Hash
}

function Find-Ide
{
    if ($Ide)
    {
        foreach ($dir in @($Ide, (Join-Path $Ide "ide")))
        {
            if (Test-Path (Join-Path $dir "mcuxpressoidec.exe"))
            {
                return (Resolve-Path $dir).Path
            }
        }

        throw "No MCUXpresso IDE at '$Ide' (mcuxpressoidec.exe not found)"
    }

    $found = Get-ChildItem "C:\nxp" -Directory -Filter "MCUXpressoIDE_*" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "ide\mcuxpressoidec.exe") } |
        Sort-Object -Descending -Property {
            $v = [version]"0.0"
            [void][version]::TryParse(($_.Name -replace '^MCUXpressoIDE_', ''), [ref]$v)
            $v
        } |
        Select-Object -First 1

    if (-not $found)
    {
        throw "MCUXpresso IDE not found in C:\nxp: pass -Ide <folder> or set MCUXPRESSO_IDE"
    }

    return (Join-Path $found.FullName "ide")
}

# the value PSXE\sound_switch.h gives PSXE_SOUND when nothing else does
function Get-SoundDefault
{
    $header = Join-Path $Project "PSXE\sound_switch.h"

    foreach ($line in [IO.File]::ReadAllLines($header))
    {
        if ($line -match '^\s*#define\s+PSXE_SOUND\s+(\d+)')
        {
            return [int]$Matches[1]
        }
    }

    throw "no '#define PSXE_SOUND' in $header"
}

# the lines worth showing while the full output goes to the log
function Test-Interesting([string] $line)
{
    if ($line -match '^\s*arm-none-eabi-')
    {
        return $false # the command lines themselves: long, and all in the log
    }

    return ($line -match '^Building (file|target):' -or
            $line -match ':\s*(fatal )?error\b|:\s*warning:|undefined reference|multiple definition|region .* overflowed' -or
            $line -match '^make(\[\d+\])?: \*\*\*|terminated with exit code|Build Finished' -or
            $line -match '^\s*Memory region|^\s+(SRAM_ITC|SRAM_DTC|SRAM_OC|BOARD_SDRAM|BOARD_FLASH|NCACHE_REGION)[^:]*:' -or
            $line -match '^\s+text\s+data\s+bss|^\s*\d+\s+\d+\s+\d+\s+\d+\s+[0-9a-f]+\s')
}

# runs a program, the whole output into the log, the interesting part on the console; returns the exit code
function Invoke-Logged([string] $exe, [string[]] $arguments, [switch] $ShowAll)
{
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue" # stderr of a native program must not stop the script

    try
    {
        & $exe @arguments 2>&1 | ForEach-Object {
            $line = "$_"
            $script:LogWriter.WriteLine($line)

            if ($line -match 'Build Finished\. (\d+) errors')
            {
                $script:LastErrors = [int]$Matches[1]
            }

            if ($ShowAll -or (Test-Interesting $line))
            {
                if ($line -match 'error|undefined reference|overflowed|\*\*\*')
                {
                    Write-Host $line -ForegroundColor Red
                }
                elseif ($line -match 'warning')
                {
                    Write-Host $line -ForegroundColor Yellow
                }
                else
                {
                    Write-Host $line
                }
            }
        }

        return $LASTEXITCODE
    }
    finally
    {
        $ErrorActionPreference = $previous
        $script:LogWriter.Flush()
    }
}

# the source folders of the configuration, as .cproject has them, with what each excludes
function Get-SourceFolders
{
    $xml = [xml](Get-Content -Raw (Join-Path $Project ".cproject"))
    $path = "//cconfiguration[storageModule[@moduleId='org.eclipse.cdt.core.settings' and @name='$Config']]" +
            "//sourceEntries/entry[@kind='sourcePath']"

    foreach ($entry in $xml.SelectNodes($path))
    {
        $excluding = @()

        if ($entry.GetAttribute("excluding"))
        {
            $excluding = $entry.GetAttribute("excluding") -split '\|'
        }

        [pscustomobject]@{ Name = $entry.GetAttribute("name"); Excluding = $excluding }
    }
}

# why the makefiles have to be written again, or nothing when they are up to date
function Get-StaleReason
{
    $makefile = Join-Path $BuildDir "makefile"

    if (-not (Test-Path $makefile))
    {
        return "there are none yet"
    }

    if (Test-Path $Stamp)
    {
        if ((Get-Content -Raw $Stamp).Trim() -ne (Get-ProjectHash))
        {
            return ".cproject has changed"
        }
    }
    elseif ((Get-Item (Join-Path $Project ".cproject")).LastWriteTimeUtc -gt (Get-Item $makefile).LastWriteTimeUtc)
    {
        # written by the IDE, not by this script: all there is to go by is the time
        return ".cproject is newer than the makefiles"
    }

    # every source file of the source folders has to be in the makefiles, and nothing else
    $listed = @{}

    foreach ($mk in Get-ChildItem $BuildDir -Recurse -File -Filter "subdir.mk")
    {
        foreach ($line in [IO.File]::ReadAllLines($mk.FullName))
        {
            if ($line -match '^\.\./(\S+\.(c|cpp|cc|cxx|S|s))\s*\\?\s*$')
            {
                $listed[$Matches[1].ToLowerInvariant()] = $true
            }
        }
    }

    foreach ($file in @($listed.Keys))
    {
        if (-not (Test-Path (Join-Path $Project $file)))
        {
            return "$file is gone"
        }
    }

    foreach ($folder in Get-SourceFolders)
    {
        $dir = Join-Path $Project $folder.Name

        if (-not (Test-Path $dir))
        {
            continue
        }

        foreach ($file in Get-ChildItem $dir -Recurse -File -Include *.c, *.cpp, *.cc, *.cxx, *.S, *.s)
        {
            $relative = $file.FullName.Substring($Project.Length + 1).Replace('\', '/')
            $inFolder = $relative.Substring($folder.Name.Length + 1)
            $excluded = $false

            foreach ($ex in $folder.Excluding)
            {
                if ($ex -and (($inFolder -eq $ex) -or $inFolder.StartsWith("$ex/", [StringComparison]::OrdinalIgnoreCase)))
                {
                    $excluded = $true
                }
            }

            if (-not $excluded -and -not $listed.ContainsKey($relative.ToLowerInvariant()))
            {
                return "$relative is new"
            }
        }
    }

    return $null
}

# MCUXpresso IDE writes the makefiles from .cproject and builds, in a throwaway workspace
function Invoke-Headless([string] $action, [string[]] $defines)
{
    $workspace = Join-Path ([IO.Path]::GetTempPath()) ("psxe_build_ws_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    $arguments = @("-nosplash", "--launcher.suppressErrors",
                   "-application", "org.eclipse.cdt.managedbuilder.core.headlessbuild",
                   "-data", $workspace, "-import", $Project, "-no-indexer", "-markerType", "cdt",
                   $action, "$ProjectName/$Config")

    foreach ($d in $defines)
    {
        $arguments += @("-D", $d)
    }

    # the headless build rewrites some of the project's settings files (language.settings.xml): the
    # repository's stay as they are
    $kept = @{}

    foreach ($f in @(".cproject", ".project", ".settings\language.settings.xml", ".settings\org.eclipse.cdt.core.prefs",
                     ".settings\org.eclipse.core.resources.prefs"))
    {
        $p = Join-Path $Project $f

        if (Test-Path $p)
        {
            $kept[$p] = [IO.File]::ReadAllBytes($p)
        }
    }

    Write-Host "MCUXpresso IDE writes the makefiles and builds (headless, the IDE takes a minute to start) ..."

    try
    {
        return (Invoke-Logged (Join-Path $IdeDir "mcuxpressoidec.exe") $arguments)
    }
    finally
    {
        foreach ($p in $kept.Keys)
        {
            $now = $null

            if (Test-Path $p)
            {
                $now = [IO.File]::ReadAllBytes($p)
            }

            if (($null -eq $now) -or ([Convert]::ToBase64String($now) -ne [Convert]::ToBase64String($kept[$p])))
            {
                [IO.File]::WriteAllBytes($p, $kept[$p])
                Write-Host "(the IDE had rewritten $($p.Substring($Project.Length + 1)); it is back as it was)"
            }
        }

        Remove-Item -Recurse -Force $workspace -ErrorAction SilentlyContinue
    }
}

function Invoke-Make
{
    Write-Host "make -j$Jobs in $Config ..."

    return (Invoke-Logged (Join-Path $IdeDir "buildtools\bin\make.exe") @("-r", "-j$Jobs", "-C", $BuildDir, "all"))
}

# ------------------------------------------------------------------------------------------------------------
try
{
    $IdeDir = Find-Ide
    $env:PATH = (Join-Path $IdeDir "tools\bin") + ";" + (Join-Path $IdeDir "buildtools\bin") + ";" + $env:PATH

    $defines = New-Object System.Collections.Generic.List[string]

    if (($Sound -ge 0) -and ($Sound -ne (Get-SoundDefault)))
    {
        $defines.Add("PSXE_SOUND=$Sound")
    }

    foreach ($d in $Define)
    {
        if ($d)
        {
            $defines.Add($d)
        }
    }

    $variant = ($defines | ForEach-Object { $_ -replace '[^A-Za-z0-9_]+', '-' }) -join "_"

    [void](New-Item -ItemType Directory -Force $BuildDir)

    $logPath = Join-Path $BuildDir "build.log"
    $script:LogWriter = New-Object System.IO.StreamWriter($logPath, $false)

    $started = (Get-Date).AddSeconds(-2)

    Write-Host "psxe_embedded $Config$(if ($variant) { " ($variant)" }) with $IdeDir"

    $usedIde = $true

    if ($variant)
    {
        # compiled with other switches: nothing of an earlier build can be used, nor may anything of this one be
        $code = Invoke-Headless "-cleanBuild" $defines.ToArray()
    }
    elseif ($Clean)
    {
        $code = Invoke-Headless "-cleanBuild" @()
    }
    else
    {
        $reason = $null

        if ($Regenerate)
        {
            $reason = "-Regenerate"
        }
        else
        {
            $reason = Get-StaleReason
        }

        if ($reason)
        {
            Write-Host "The makefiles have to be written again: $reason"
            $code = Invoke-Headless "-build" @()
        }
        else
        {
            $usedIde = $false
            $code = Invoke-Make
        }
    }

    $axf = Join-Path $BuildDir "$ProjectName.axf"

    if (($code -ne 0) -and ($script:LastErrors -eq 0) -and (Test-Path $axf) -and ((Get-Item $axf).LastWriteTime -ge $started))
    {
        Write-Host "(the IDE reported problems outside the build, see $logPath)" -ForegroundColor Yellow
        $code = 0
    }

    if ($code -ne 0)
    {
        throw "the build failed (exit code $code), see $logPath"
    }

    if (-not (Test-Path $axf))
    {
        throw "the build did not produce $axf, see $logPath"
    }

    if ($usedIde -and -not $variant)
    {
        Set-Content -NoNewline -Encoding ascii $Stamp (Get-ProjectHash)
    }

    if ($variant)
    {
        foreach ($ext in @("axf", "bin", "map"))
        {
            $out = Join-Path $BuildDir "$ProjectName.$ext"

            if (Test-Path $out)
            {
                Move-Item -Force $out (Join-Path $BuildDir "${ProjectName}_$variant.$ext")
            }
        }

        $axf = Join-Path $BuildDir "${ProjectName}_$variant.axf"

        # the objects and the makefiles carry the switches: gone, so that the next ordinary build starts over
        Get-ChildItem $BuildDir -Recurse -File -Include *.o, *.d, *.su, *.mk | Remove-Item -Force
        Remove-Item -Force (Join-Path $BuildDir "makefile"), $Stamp -ErrorAction SilentlyContinue
    }

    Write-Host ""
    Write-Host "Built: $axf" -ForegroundColor Green

    if ($Flash)
    {
        Write-Host "Flashing with LinkServer ($Probe) ..."

        $code = Invoke-Logged (Join-Path $IdeDir "LinkServer\LinkServer.exe") @("flash", $Probe, "load", $axf) -ShowAll

        if ($code -ne 0)
        {
            throw "flashing failed (exit code $code) - is the probe in use by a debug session?"
        }

        Write-Host "Flashed and started." -ForegroundColor Green
    }

    exit 0
}
catch
{
    Write-Host "build.ps1: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally
{
    if ($script:LogWriter)
    {
        $script:LogWriter.Close()
    }
}
