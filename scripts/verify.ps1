param(
    [string]$SamplePng,
    [ValidatePattern('^[LR]*$')]
    [string]$NavigationScript = 'RRRRRL'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'
$solution = Join-Path $projectRoot 'PhotoViewer.sln'
$output = Join-Path $projectRoot 'x64\Release'

& $msbuild $solution /m /t:Build /p:Configuration=Release /p:Platform=x64 /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Release build failed: $LASTEXITCODE" }

& (Join-Path $output 'PhotoViewer.CoreTests.exe')
if ($LASTEXITCODE -ne 0) { throw "Core tests failed: $LASTEXITCODE" }

& (Join-Path $output 'PhotoViewer.RuntimeTests.exe')
if ($LASTEXITCODE -ne 0) { throw "Runtime tests failed: $LASTEXITCODE" }

if ($SamplePng) {
    $sample = (Resolve-Path -LiteralPath $SamplePng).Path
    $before = (Get-FileHash -LiteralPath $sample -Algorithm SHA256).Hash
    $fullscreenProcess = Start-Process `
        -FilePath (Join-Path $output 'PhotoViewer.exe') `
        -ArgumentList @(
            '--validation-fullscreen',
            '--validation-navigation=R',
            '--workers=7',
            ('"' + $sample + '"')
        ) `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($fullscreenProcess.ExitCode -ne 0) {
        throw "F11 fullscreen integration test failed: $($fullscreenProcess.ExitCode)"
    }
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $validationMode = if ($NavigationScript) {
        "--validation-navigation=$NavigationScript"
    } else {
        '--validation-exit-after-present'
    }
    $process = Start-Process `
        -FilePath (Join-Path $output 'PhotoViewer.exe') `
        -ArgumentList @(
            $validationMode,
            '--staging-cache-mib=512',
            '--gpu-cache-mib=256',
            '--compressed-budget-mib=256',
            '--workers=7',
            ('"' + $sample + '"')
        ) `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    $timer.Stop()
    $after = (Get-FileHash -LiteralPath $sample -Algorithm SHA256).Hash
    if ($process.ExitCode -ne 0) { throw "PhotoViewer integration test failed: $($process.ExitCode)" }
    if ($before -ne $after) { throw 'Source PNG changed during integration test' }
    [pscustomobject]@{
        FullscreenExitCode = $fullscreenProcess.ExitCode
        IntegrationExitCode = $process.ExitCode
        IntegrationElapsedMs = $timer.ElapsedMilliseconds
        NavigationScript = $NavigationScript
        SourceSha256 = $after
        SourceUnchanged = $true
    }
}

Write-Host 'PASS: build and automated verification completed.'
