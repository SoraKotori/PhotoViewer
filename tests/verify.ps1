#requires -Version 7.0

param(
    [switch]$AppIntegration,
    [string]$MSBuildPath
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$appProject = Join-Path $projectRoot 'PhotoViewer.vcxproj'
$coreTestsProject = Join-Path $projectRoot 'PhotoViewer.CoreTests.vcxproj'
$runtimeTestsProject = Join-Path $projectRoot 'PhotoViewer.RuntimeTests.vcxproj'
$releaseOutput = Join-Path $projectRoot 'out\build\x64\Release'
$appOutput = Join-Path $releaseOutput 'app'
$coreTestsOutput = Join-Path $releaseOutput 'tests\core'
$runtimeTestsOutput = Join-Path $releaseOutput 'tests\runtime'

function Normalize-ProcessPathEnvironment {
    $environment = [Environment]::GetEnvironmentVariables(
        [EnvironmentVariableTarget]::Process)
    $pathNames = @($environment.Keys | Where-Object { $_ -ieq 'Path' })
    if ($pathNames.Count -le 1) {
        return
    }

    $pathSegments = foreach ($pathName in $pathNames) {
        ([string]$environment[$pathName]) -split ';'
    }
    $normalizedPath = @($pathSegments |
        Where-Object { $_ } |
        Select-Object -Unique) -join ';'
    foreach ($pathName in $pathNames) {
        Remove-Item -LiteralPath "Env:$pathName" -ErrorAction Stop
    }
    $env:Path = $normalizedPath
}

function Resolve-MSBuildExecutable {
    param(
        [string]$ConfiguredPath
    )

    if ($ConfiguredPath) {
        try {
            $resolvedPath = (
                Resolve-Path -LiteralPath $ConfiguredPath -ErrorAction Stop).Path
            if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
                throw 'not a file'
            }
            return $resolvedPath
        } catch {
            throw 'MSBuildPath must identify an existing executable'
        }
    }

    $command = Get-Command 'MSBuild.exe' -CommandType Application `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command.Source
    }

    $visualStudioInstaller = Join-Path `
        ([Environment]::GetFolderPath('ProgramFilesX86')) `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $visualStudioInstaller -PathType Leaf) {
        $discoveredPath = & $visualStudioInstaller `
            -latest `
            -products '*' `
            -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\amd64\MSBuild.exe' |
            Select-Object -First 1
        if ($discoveredPath -and
            (Test-Path -LiteralPath $discoveredPath -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $discoveredPath).Path
        }
    }

    throw 'MSBuild executable was not found; pass -MSBuildPath explicitly'
}

Normalize-ProcessPathEnvironment
$msbuildPath = Resolve-MSBuildExecutable -ConfiguredPath $MSBuildPath

function Invoke-ProjectBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectPath,
        [Parameter(Mandatory = $true)]
        [string]$DisplayName
    )

    & $msbuildPath $ProjectPath /m:1 /t:Build /p:Configuration=Release `
        /p:Platform=x64 /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "$DisplayName build failed: $LASTEXITCODE"
    }
}

Invoke-ProjectBuild -ProjectPath $coreTestsProject -DisplayName 'Core tests'
Invoke-ProjectBuild -ProjectPath $runtimeTestsProject -DisplayName 'Runtime tests'

& (Join-Path $coreTestsOutput 'PhotoViewer.CoreTests.exe')
if ($LASTEXITCODE -ne 0) { throw "Core tests failed: $LASTEXITCODE" }

& (Join-Path $runtimeTestsOutput 'PhotoViewer.RuntimeTests.exe')
if ($LASTEXITCODE -ne 0) { throw "Runtime tests failed: $LASTEXITCODE" }

if ($AppIntegration) {
    Invoke-ProjectBuild -ProjectPath $appProject -DisplayName 'PhotoViewer'
    . (Join-Path $projectRoot 'config\read-test-config.ps1')
    $testConfig = Read-PhotoViewerTestConfig
    $initialImagePath = $testConfig.Dataset.InitialImagePath
    $navigationSequence = $testConfig.AppIntegration.NavigationSequence
    $initialImageHashBefore = (
        Get-FileHash -LiteralPath $initialImagePath -Algorithm SHA256).Hash
    if ($initialImageHashBefore -cne $testConfig.Dataset.InitialImageSha256) {
        throw 'Configured initial image does not match its SHA-256'
    }
    $fullscreenTestProcess = Start-Process `
        -FilePath (Join-Path $appOutput 'PhotoViewer.exe') `
        -ArgumentList @(
            '--validation-fullscreen',
            '--workers=4',
            ('"' + $initialImagePath + '"')
        ) `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($fullscreenTestProcess.ExitCode -ne 0) {
        throw "F11 fullscreen app integration test failed: $($fullscreenTestProcess.ExitCode)"
    }
    $initialCompressedDataBudgetRejectionStatus = 'Skipped'
    $initialCompressedDataBudgetRejectionExitCode = $null
    if ((Get-Item -LiteralPath $initialImagePath).Length -gt 1MB) {
        $initialCompressedDataBudgetRejectionProcess = Start-Process `
            -FilePath (Join-Path $appOutput 'PhotoViewer.exe') `
            -ArgumentList @(
                '--validation-exit-after-present',
                '--validation-timeout-ms=500',
                '--compressed-budget-mib=1',
                '--workers=4',
                ('"' + $initialImagePath + '"')
            ) `
            -WindowStyle Hidden `
            -PassThru
        if (-not $initialCompressedDataBudgetRejectionProcess.WaitForExit(5000)) {
            Stop-Process -Id $initialCompressedDataBudgetRejectionProcess.Id -Force
            throw 'Initial compressed-data budget rejection left startup waiting indefinitely'
        }
        $initialCompressedDataBudgetRejectionExitCode =
            $initialCompressedDataBudgetRejectionProcess.ExitCode
        if ($initialCompressedDataBudgetRejectionExitCode -ne 109) {
            throw "Initial compressed-data budget rejection returned unexpected exit code: $initialCompressedDataBudgetRejectionExitCode"
        }
        $initialCompressedDataBudgetRejectionStatus = 'Passed'
    }
    $validationExitArgument = if ($navigationSequence) {
        "--validation-navigation=$navigationSequence"
    } else {
        '--validation-exit-after-present'
    }
    $runId = '{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'),
        [Guid]::NewGuid().ToString('N')
    $appIntegrationRunDirectory = Join-Path $projectRoot (
        Join-Path 'out\tests\app-integration' $runId)
    [IO.Directory]::CreateDirectory($appIntegrationRunDirectory) | Out-Null
    $reportPath = Join-Path $appIntegrationRunDirectory 'report.txt'
    $appIntegrationArguments = @(
        $validationExitArgument,
        "--validation-report=$reportPath",
        '--workers=4',
        ('"' + $initialImagePath + '"')
    )
    if ($navigationSequence) {
        $appIntegrationArguments += '--validation-navigation-start-delay-ms=1'
    }
    $appIntegrationProcess = Start-Process `
        -FilePath (Join-Path $appOutput 'PhotoViewer.exe') `
        -ArgumentList $appIntegrationArguments `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    $initialImageHashAfter = (
        Get-FileHash -LiteralPath $initialImagePath -Algorithm SHA256).Hash
    if ($appIntegrationProcess.ExitCode -ne 0) {
        throw "PhotoViewer app integration test failed: $($appIntegrationProcess.ExitCode)"
    }
    if ($initialImageHashBefore -cne $initialImageHashAfter -or
        $initialImageHashAfter -cne $testConfig.Dataset.InitialImageSha256) {
        throw 'Configured initial image changed during app integration test'
    }
    $reportLines = [IO.File]::ReadAllLines($reportPath)
    if ($navigationSequence -and
        $reportLines -notcontains 'phase=navigation-started') {
        throw 'App integration report did not record the navigation start'
    }
    $reportValues = @{}
    foreach ($line in $reportLines) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            $reportValues[$line.Substring(0, $separator)] =
                $line.Substring($separator + 1)
        }
    }
    if ($reportValues['title_matches_current'] -ne '1') {
        throw 'Window title did not match the presented image filename'
    }
    [pscustomobject]@{
        FullscreenExitCode = $fullscreenTestProcess.ExitCode
        InitialCompressedDataBudgetRejectionStatus =
            $initialCompressedDataBudgetRejectionStatus
        InitialCompressedDataBudgetRejectionExitCode =
            $initialCompressedDataBudgetRejectionExitCode
        AppIntegrationExitCode = $appIntegrationProcess.ExitCode
        InitialImageUnchanged = $true
    }
}

Write-Host 'PASS: requested builds and automated tests completed.'
