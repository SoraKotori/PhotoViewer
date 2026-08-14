param(
    [string]$SamplePng,
    [ValidatePattern('^[LR]*$')]
    [string]$NavigationScript = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'
$solution = Join-Path $projectRoot 'PhotoViewer.sln'
$output = Join-Path $projectRoot 'x64\Release'

& $msbuild $solution /m /t:Build /p:Configuration=Release /p:Platform=x64 /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Release build failed: $LASTEXITCODE" }

& (Join-Path $PSScriptRoot 'check-thread-priority.ps1') `
    -ProjectRoot $projectRoot `
    -Binary (Join-Path $output 'PhotoViewer.exe')

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
            '--workers=4',
            ('"' + $sample + '"')
        ) `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($fullscreenProcess.ExitCode -ne 0) {
        throw "F11 fullscreen integration test failed: $($fullscreenProcess.ExitCode)"
    }
    $startupFailureExitCode = $null
    if ((Get-Item -LiteralPath $sample).Length -gt 1MB) {
        $startupFailure = Start-Process `
            -FilePath (Join-Path $output 'PhotoViewer.exe') `
            -ArgumentList @(
                '--validation-exit-after-present',
                '--validation-timeout-ms=500',
                '--compressed-budget-mib=1',
                '--workers=4',
                ('"' + $sample + '"')
            ) `
            -WindowStyle Hidden `
            -PassThru
        if (-not $startupFailure.WaitForExit(5000)) {
            Stop-Process -Id $startupFailure.Id -Force
            throw 'Initial image allocation failure left startup waiting indefinitely'
        }
        $startupFailureExitCode = $startupFailure.ExitCode
        if ($startupFailureExitCode -ne 109) {
            throw "Initial image failure returned unexpected exit code: $startupFailureExitCode"
        }
    }
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $validationMode = if ($NavigationScript) {
        "--validation-navigation=$NavigationScript"
    } else {
        '--validation-exit-after-present'
    }
    $report = Join-Path ([IO.Path]::GetTempPath()) `
        ("PhotoViewer-verify-{0}.txt" -f [Guid]::NewGuid().ToString('N'))
    $process = Start-Process `
        -FilePath (Join-Path $output 'PhotoViewer.exe') `
        -ArgumentList @(
            $validationMode,
            "--validation-report=$report",
            '--workers=4',
            ('"' + $sample + '"')
        ) `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    $timer.Stop()
    $after = (Get-FileHash -LiteralPath $sample -Algorithm SHA256).Hash
    if ($process.ExitCode -ne 0) { throw "PhotoViewer integration test failed: $($process.ExitCode)" }
    if ($before -ne $after) { throw 'Source PNG changed during integration test' }
    $reportValues = @{}
    foreach ($line in [IO.File]::ReadAllLines($report)) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            $reportValues[$line.Substring(0, $separator)] =
                $line.Substring($separator + 1)
        }
    }
    [IO.File]::Delete($report)
    if ($reportValues['title_matches_current'] -ne '1') {
        throw 'Window title did not match the presented image filename'
    }
    [pscustomobject]@{
        FullscreenExitCode = $fullscreenProcess.ExitCode
        StartupFailureExitCode = $startupFailureExitCode
        IntegrationExitCode = $process.ExitCode
        IntegrationElapsedMs = $timer.ElapsedMilliseconds
        NavigationScript = $NavigationScript
        SourceSha256 = $after
        SourceUnchanged = $true
    }
}

Write-Host 'PASS: build and automated verification completed.'
