param(
    [Parameter(Mandatory = $true)]
    [string]$SamplePng,
    [ValidateRange(1, 100)]
    [int]$Runs = 3,
    [ValidateRange(1, 1000)]
    [int]$Steps = 120,
    [ValidateRange(1, 60000)]
    [int]$WarmupMs = 1250,
    [ValidateRange(1, 256)]
    [int]$Workers = 10,
    [ValidateRange(1, 4096)]
    [int]$CpuSurfaceSlots = 32,
    [ValidateRange(1, 4096)]
    [int]$GpuTextureSlots = 16,
    [ValidateRange(1, 4096)]
    [int]$CompressedSlots = 64,
    [ValidateSet('L', 'R')]
    [string]$Direction = 'R',
    [switch]$ShortPresses,
    [ValidateRange(0, 1000)]
    [int]$NavigationIntervalMs = 0,
    [ValidateRange(0.01, 1000.0)]
    [double]$TargetFps = 30.0
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$viewer = Join-Path $projectRoot 'x64\Release\PhotoViewer.exe'
if (-not (Test-Path -LiteralPath $viewer -PathType Leaf)) {
    throw "Release executable not found: $viewer"
}

$sample = (Resolve-Path -LiteralPath $SamplePng).Path
$files = @(Get-ChildItem -LiteralPath (Split-Path -Parent $sample) -Filter '*.png' |
    Sort-Object Name)
$compatibleFiles = @()
foreach ($file in $files) {
    $stream = [System.IO.File]::Open(
        $file.FullName,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    try {
        $header = [byte[]]::new(29)
        $read = $stream.Read($header, 0, $header.Length)
    } finally {
        $stream.Dispose()
    }
    if ($read -ne 29) { continue }
    $width = ([uint32]$header[16] -shl 24) -bor
             ([uint32]$header[17] -shl 16) -bor
             ([uint32]$header[18] -shl 8) -bor [uint32]$header[19]
    $height = ([uint32]$header[20] -shl 24) -bor
              ([uint32]$header[21] -shl 16) -bor
              ([uint32]$header[22] -shl 8) -bor [uint32]$header[23]
    if ($width -eq 7680 -and $height -eq 4320 -and
        $header[24] -eq 8 -and $header[25] -eq 6 -and $header[28] -eq 0) {
        $compatibleFiles += $file
    }
}
$start = -1
for ($index = 0; $index -lt $compatibleFiles.Count; ++$index) {
    if ($compatibleFiles[$index].FullName -eq $sample) {
        $start = $index
        break
    }
}
if ($start -lt 0) {
    throw 'The sample PNG is not a compatible 8K RGBA image'
}
if ($Direction -eq 'R') {
    if ($start + $Steps -ge $compatibleFiles.Count) {
        throw "Only $($compatibleFiles.Count - $start - 1) compatible images follow the sample; $Steps required"
    }
    $usedFiles = @($compatibleFiles[$start..($start + $Steps)])
} else {
    if ($start -lt $Steps) {
        throw "Only $start compatible images precede the sample; $Steps required"
    }
    $usedFiles = @($compatibleFiles[($start - $Steps)..$start])
}

$resultDirectory = Join-Path $projectRoot 'test-results'
[System.IO.Directory]::CreateDirectory($resultDirectory) | Out-Null
$fileList = Join-Path $resultDirectory 'performance-files.txt'
$usedFiles.FullName | Set-Content -LiteralPath $fileList -Encoding utf8NoBOM
$before = @{}
foreach ($file in $usedFiles) {
    $before[$file.FullName] = "$($file.Length):$($file.LastWriteTimeUtc.Ticks)"
}

$navigation = $Direction * $Steps
$inputMode = if ($ShortPresses) { 'short' } else { 'hold' }
$measurements = @()
$injectionMeasurements = @()
$tailMeasurements = @()
for ($run = 1; $run -le $Runs; ++$run) {
    $report = Join-Path $resultDirectory "performance-report-$Direction-$inputMode-$run.txt"
    $arguments = @(
        ('"' + $sample + '"'),
        "--workers=$Workers",
        "--cpu-surface-slot-count=$CpuSurfaceSlots",
        "--gpu-texture-slot-count=$GpuTextureSlots",
        "--compressed-slot-count=$CompressedSlots",
        "--validation-file-list=$fileList",
        "--validation-navigation=$navigation",
        "--validation-warmup-ms=$WarmupMs",
        "--validation-report=$report",
        '--validation-elapsed-exit-code',
        '--validation-timeout-ms=60000'
    )
    if ($NavigationIntervalMs -gt 0) {
        $arguments += "--validation-navigation-interval-ms=$NavigationIntervalMs"
    }
    if ($ShortPresses) {
        $arguments += '--validation-short-presses'
    }
    $process = Start-Process `
        -FilePath $viewer `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($process.ExitCode -ge 100 -and $process.ExitCode -le 109) {
        throw "Run $run timed out at pipeline stage $($process.ExitCode - 100)"
    }
    if ($process.ExitCode -le 0) {
        throw "Run $run did not return a valid elapsed time: $($process.ExitCode)"
    }
    $fps = $Steps * 1000.0 / $process.ExitCode
    $measurements += $fps
    $finalReport = @{}
    $inFinalPhase = $false
    foreach ($line in Get-Content -LiteralPath $report) {
        if ($line -like 'phase=*') {
            $inFinalPhase = $line -eq 'phase=navigation-complete'
            continue
        }
        if ($inFinalPhase -and $line -match '^([^=]+)=(.*)$') {
            $finalReport[$matches[1]] = $matches[2]
        }
    }
    $injectionMs = [double]$finalReport.navigation_injection_nanoseconds / 1.0e6
    $tailMs = [double]$finalReport.navigation_pipeline_tail_nanoseconds / 1.0e6
    $injectionMeasurements += $injectionMs
    $tailMeasurements += $tailMs
    [pscustomobject]@{
        Run = $run
        ElapsedMs = $process.ExitCode
        ImagesPerSecond = [math]::Round($fps, 2)
        InputInjectionMs = [math]::Round($injectionMs, 2)
        PipelineTailMs = [math]::Round($tailMs, 2)
    }
}

foreach ($file in $usedFiles) {
    $current = Get-Item -LiteralPath $file.FullName
    $after = "$($current.Length):$($current.LastWriteTimeUtc.Ticks)"
    if ($after -ne $before[$file.FullName]) {
        throw "Source PNG metadata changed during performance test: $($file.FullName)"
    }
}

$minimum = ($measurements | Measure-Object -Minimum).Minimum
$average = ($measurements | Measure-Object -Average).Average
[pscustomobject]@{
    Runs = $Runs
    Workers = $Workers
    CpuSurfaceSlots = $CpuSurfaceSlots
    GpuTextureSlots = $GpuTextureSlots
    CompressedSlots = $CompressedSlots
    Direction = $Direction
    InputMode = $inputMode
    StepsPerRun = $Steps
    WarmupMs = $WarmupMs
    NavigationIntervalMs = $NavigationIntervalMs
    TargetImagesPerSecond = $TargetFps
    MinimumImagesPerSecond = [math]::Round($minimum, 2)
    AverageImagesPerSecond = [math]::Round($average, 2)
    MaximumInputInjectionMs = [math]::Round(
        ($injectionMeasurements | Measure-Object -Maximum).Maximum, 2)
    MaximumPipelineTailMs = [math]::Round(
        ($tailMeasurements | Measure-Object -Maximum).Maximum, 2)
    SourceFilesUnchanged = $true
}
if ($minimum -lt $TargetFps) {
    throw "Performance target missed: minimum $([math]::Round($minimum, 2)) < $TargetFps images/s"
}

Write-Host 'PASS: performance target met in every run.'
