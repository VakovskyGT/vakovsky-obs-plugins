[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json

    # Inno Setup is on the GitHub windows-2022 image; locally a missing one only skips the installers.
    # winget from a non-admin terminal installs it per user, into LOCALAPPDATA\Programs.
    $Iscc = (Get-Command -Name ISCC.exe -ErrorAction SilentlyContinue).Source
    if ( ! $Iscc ) {
        $Iscc = @("${env:ProgramFiles(x86)}", "${env:ProgramFiles}", "${env:LOCALAPPDATA}/Programs") |
            Where-Object { $_ } | ForEach-Object { "$_/Inno Setup 6/ISCC.exe" } |
            Where-Object { Test-Path -Path $_ } | Select-Object -First 1
    }
    if ( ! $Iscc ) {
        if ( $env:GITHUB_ACTIONS -ne $null ) {
            throw 'Inno Setup (ISCC.exe) not found'
        }
        Write-Warning 'Inno Setup (ISCC.exe) not found: the installers are skipped'
    }

    # Two archives per plugin, each with the plugin's folder for C:\ProgramData\obs-studio\plugins:
    # OBS 33 and newer take the module next to data, OBS 31 and 32 take it from bin\64bit ("legacy").
    # Then an installer per plugin, for the installed OBS and portable ones.
    foreach ( $Plugin in $BuildSpec.plugins.PSObject.Properties ) {
        $ProductName = $Plugin.Name
        $ProductVersion = $Plugin.Value.version
        $Installed = "${ProjectRoot}/release/${Configuration}/${ProductName}"
        $Stage = "${ProjectRoot}/release/stage/${ProductName}"

        Remove-Item -ErrorAction SilentlyContinue -Path "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        Remove-Item -ErrorAction SilentlyContinue -Path "${ProjectRoot}/release/${ProductName}-*-windows-*.exe"

        Log-Group "Archiving ${ProductName}..."
        # the CMake install has the legacy layout
        $CompressArgs = @{
            Path = $Installed
            CompressionLevel = 'Optimal'
            DestinationPath = "${ProjectRoot}/release/${ProductName}-${ProductVersion}-windows-legacy-${Target}.zip"
            Verbose = ($Env:CI -ne $null)
        }
        Compress-Archive -Force @CompressArgs

        Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path "${ProjectRoot}/release/stage"
        New-Item -ItemType Directory -Force -Path "${ProjectRoot}/release/stage" | Out-Null
        Copy-Item -Recurse -Path $Installed -Destination $Stage
        Move-Item -Path "${Stage}/bin/64bit/*" -Destination $Stage
        Remove-Item -Recurse -Force -Path "${Stage}/bin"
        $CompressArgs.Path = $Stage
        $CompressArgs.DestinationPath = "${ProjectRoot}/release/${ProductName}-${ProductVersion}-windows-${Target}.zip"
        Compress-Archive -Force @CompressArgs
        Remove-Item -Recurse -Force -Path "${ProjectRoot}/release/stage"
        Log-Group

        if ( $Iscc ) {
            # one installer per plugin, it picks the layout by the OBS version
            Log-Group "Building the ${ProductName} installer..."
            $IsccArgs = @(
                '/Q'
                "/DModule=${ProductName}"
                "/DVersion=${ProductVersion}"
                "/DDisplayName=$($Plugin.Value.displayName)"
                "/DSourceDir=$([System.IO.Path]::GetFullPath($Installed))"
                "/DRepoRoot=$([System.IO.Path]::GetFullPath($ProjectRoot))"
                "/O$([System.IO.Path]::GetFullPath("${ProjectRoot}/release"))"
                "${ScriptHome}/installer.iss"
            )
            & $Iscc @IsccArgs
            if ( $LASTEXITCODE -ne 0 ) {
                throw "ISCC failed for ${ProductName} (exit code ${LASTEXITCODE})"
            }
            Log-Group
        }
    }
}

Package
