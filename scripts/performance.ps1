param(
    [Parameter(Mandatory = $true)]
    [string]$SamplePng,
    [ValidateRange(1, 100)]
    [int]$Runs = 3,
    [ValidateRange(1, 1000)]
    [int]$Steps = 180,
    [ValidateRange(0, 60000)]
    [int]$WarmupMs = 0,
    [ValidateRange(1, 256)]
    [int]$Workers = 5,
    [ValidateRange(1, 4096)]
    [int]$StagingSlots = 8,
    [ValidateRange(1, 4096)]
    [int]$GpuTextureSlots = 3,
    [ValidateRange(1, 4096)]
    [int]$CompressedSlots = 8,
    [ValidateSet('L', 'R')]
    [string]$Direction = 'R',
    [switch]$ShortPresses,
    [ValidateRange(0, 1000)]
    [int]$NavigationIntervalMs = 0,
    [ValidateRange(0.01, 1000.0)]
    [double]$TargetFps = 30.0
)

$ErrorActionPreference = 'Stop'

if (-not ('PhotoViewer.ProcessMemoryCountersEx' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace PhotoViewer {
    [StructLayout(LayoutKind.Sequential)]
    public struct ProcessMemoryCountersEx {
        public uint Size;
        public uint PageFaultCount;
        public UIntPtr PeakWorkingSetSize;
        public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage;
        public UIntPtr QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage;
        public UIntPtr QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage;
        public UIntPtr PeakPagefileUsage;
        public UIntPtr PrivateUsage;
    }

    public static class ProcessMemory {
        [DllImport("psapi.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetProcessMemoryInfo(
            IntPtr process,
            ref ProcessMemoryCountersEx counters,
            uint size);
    }
}
'@
}

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
$commitMeasurements = @()
$workingSetMeasurements = @()
$managedMemoryMeasurements = @()
$coldStartMeasurements = @()
$coldTimelineMeasurements = @()
$maximumGapMeasurements = @()
$maximumLatenessMeasurements = @()
$maximumReadyLatenessMeasurements = @()
$lateReadyCountMeasurements = @()
for ($run = 1; $run -le $Runs; ++$run) {
    $report = Join-Path $resultDirectory "performance-report-$Direction-$inputMode-$run.txt"
    $arguments = @(
        ('"' + $sample + '"'),
        "--workers=$Workers",
        "--staging-slot-count=$StagingSlots",
        "--gpu-texture-slot-count=$GpuTextureSlots",
        "--compressed-slot-count=$CompressedSlots",
        "--validation-file-list=$fileList",
        "--validation-navigation=$navigation",
        "--validation-report=$report",
        '--validation-elapsed-exit-code',
        '--validation-timeout-ms=60000'
    )
    if ($WarmupMs -gt 0) {
        $arguments += "--validation-warmup-ms=$WarmupMs"
    }
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
        -PassThru
    $processHandle = $process.SafeHandle
    $process.WaitForExit()
    $memory = [PhotoViewer.ProcessMemoryCountersEx]::new()
    $memory.Size = [Runtime.InteropServices.Marshal]::SizeOf($memory)
    if (-not [PhotoViewer.ProcessMemory]::GetProcessMemoryInfo(
        $processHandle.DangerousGetHandle(), [ref]$memory, $memory.Size)) {
        throw "GetProcessMemoryInfo failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    [int64]$peakCommitBytes = $memory.PeakPagefileUsage.ToUInt64()
    [int64]$peakWorkingSetBytes = $memory.PeakWorkingSetSize.ToUInt64()
    [GC]::KeepAlive($processHandle)
    $commitMeasurements += $peakCommitBytes
    $workingSetMeasurements += $peakWorkingSetBytes
    if ($process.ExitCode -ge 100 -and $process.ExitCode -le 109) {
        throw "Run $run timed out at pipeline stage $($process.ExitCode - 100)"
    }
    if ($process.ExitCode -le 0) {
        throw "Run $run did not return a valid elapsed time: $($process.ExitCode)"
    }
    $fps = $Steps * 1000.0 / $process.ExitCode
    $measurements += $fps
    $finalReport = @{}
    $managedPeakBytes = 0L
    $managedBytes = @{
        compressed_committed_bytes = 0L
        staging_committed_bytes = 0L
        gpu_bytes = 0L
    }
    $inFinalPhase = $false
    foreach ($line in Get-Content -LiteralPath $report) {
        if ($line -like 'phase=*') {
            $inFinalPhase = $line -eq 'phase=navigation-complete'
            continue
        }
        if ($inFinalPhase -and $line -match '^([^=]+)=(.*)$') {
            $finalReport[$matches[1]] = $matches[2]
        }
        if ($line -match '^(compressed_committed_bytes|staging_committed_bytes|gpu_bytes)=([0-9]+)$') {
            $managedBytes[$matches[1]] = [int64]$matches[2]
            $managedTotal = $managedBytes.compressed_committed_bytes +
                            $managedBytes.staging_committed_bytes +
                            $managedBytes.gpu_bytes
            $managedPeakBytes = [math]::Max($managedPeakBytes, $managedTotal)
        }
    }
    $managedMemoryMeasurements += $managedPeakBytes
    $presentedIndices = @($finalReport.validation_presented_indices -split ',' |
        ForEach-Object { [int64]$_ })
    $presentedNanoseconds = @($finalReport.validation_presented_nanoseconds -split ',' |
        ForEach-Object { [int64]$_ })
    $readyIndices = @($finalReport.validation_ready_indices -split ',' |
        Where-Object { $_ -ne '' } |
        ForEach-Object { [int64]$_ })
    $readyNanoseconds = @($finalReport.validation_ready_nanoseconds -split ',' |
        Where-Object { $_ -ne '' } |
        ForEach-Object { [int64]$_ })
    if ($presentedIndices.Count -ne $Steps + 1 -or
        $presentedNanoseconds.Count -ne $Steps + 1) {
        throw "Run $run reported $($presentedIndices.Count) presentations; $($Steps + 1) required"
    }
    if ($readyIndices.Count -ne $readyNanoseconds.Count) {
        throw "Run $run reported mismatched ready indices and timestamps"
    }
    $readyByIndex = @{}
    for ($index = 0; $index -lt $readyIndices.Count; ++$index) {
        $readyByIndex[$readyIndices[$index]] = $readyNanoseconds[$index]
    }
    $expectedDelta = if ($Direction -eq 'R') { 1 } else { -1 }
    [double]$maximumGapMs = 0
    [double]$maximumLatenessMs = 0
    [double]$maximumReadyLatenessMs = [double]::NegativeInfinity
    [int]$lateReadyCount = 0
    $targetPeriodNanoseconds = 1.0e9 / $TargetFps
    for ($ordinal = 0; $ordinal -lt $presentedIndices.Count; ++$ordinal) {
        if ($ordinal -gt 0 -and
            $presentedIndices[$ordinal] - $presentedIndices[$ordinal - 1] -ne
                $expectedDelta) {
            throw "Run $run presentation order is not contiguous at position $ordinal"
        }
        if ($ordinal -gt 0) {
            $gapMs = ($presentedNanoseconds[$ordinal] -
                      $presentedNanoseconds[$ordinal - 1]) / 1.0e6
            $maximumGapMs = [math]::Max($maximumGapMs, $gapMs)
        }
        $deadline = $presentedNanoseconds[0] +
                    $ordinal * $targetPeriodNanoseconds
        $latenessMs = ($presentedNanoseconds[$ordinal] - $deadline) / 1.0e6
        $maximumLatenessMs = [math]::Max($maximumLatenessMs, $latenessMs)
        $frameIndex = $presentedIndices[$ordinal]
        if (-not $readyByIndex.ContainsKey($frameIndex)) {
            throw "Run $run has no ready timestamp for image index $frameIndex"
        }
        $readyLatenessMs = ([int64]$readyByIndex[$frameIndex] - $deadline) / 1.0e6
        $maximumReadyLatenessMs = [math]::Max(
            $maximumReadyLatenessMs, $readyLatenessMs)
        if ($readyLatenessMs -gt 0) {
            ++$lateReadyCount
        }
    }
    $coldStartMs = $presentedNanoseconds[0] / 1.0e6
    $coldTimelineElapsedMs = ($presentedNanoseconds[-1] -
                              $presentedNanoseconds[0]) / 1.0e6
    $coldTimelineFps = $Steps * 1000.0 / $coldTimelineElapsedMs
    $coldStartMeasurements += $coldStartMs
    $coldTimelineMeasurements += $coldTimelineFps
    $maximumGapMeasurements += $maximumGapMs
    $maximumLatenessMeasurements += $maximumLatenessMs
    $maximumReadyLatenessMeasurements += $maximumReadyLatenessMs
    $lateReadyCountMeasurements += $lateReadyCount
    $injectionMs = [double]$finalReport.navigation_injection_nanoseconds / 1.0e6
    $tailMs = [double]$finalReport.navigation_pipeline_tail_nanoseconds / 1.0e6
    $injectionMeasurements += $injectionMs
    $tailMeasurements += $tailMs
    [pscustomobject]@{
        Run = $run
        ElapsedMs = $process.ExitCode
        ImagesPerSecond = [math]::Round($fps, 2)
        ColdFirstPresentMs = [math]::Round($coldStartMs, 2)
        ColdTimelineImagesPerSecond = [math]::Round($coldTimelineFps, 2)
        MaximumPresentationGapMs = [math]::Round($maximumGapMs, 2)
        MaximumDeadlineLatenessMs = [math]::Round($maximumLatenessMs, 2)
        MaximumReadyDeadlineLatenessMs = [math]::Round($maximumReadyLatenessMs, 2)
        LateReadyImages = $lateReadyCount
        InputInjectionMs = [math]::Round($injectionMs, 2)
        PipelineTailMs = [math]::Round($tailMs, 2)
        PeakCommitMiB = [math]::Round($peakCommitBytes / 1MB, 1)
        PeakWorkingSetMiB = [math]::Round($peakWorkingSetBytes / 1MB, 1)
        PeakManagedStorageMiB = [math]::Round($managedPeakBytes / 1MB, 1)
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
    StagingSlots = $StagingSlots
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
    MaximumColdFirstPresentMs = [math]::Round(
        ($coldStartMeasurements | Measure-Object -Maximum).Maximum, 2)
    MinimumColdTimelineImagesPerSecond = [math]::Round(
        ($coldTimelineMeasurements | Measure-Object -Minimum).Minimum, 2)
    AverageColdTimelineImagesPerSecond = [math]::Round(
        ($coldTimelineMeasurements | Measure-Object -Average).Average, 2)
    MaximumPresentationGapMs = [math]::Round(
        ($maximumGapMeasurements | Measure-Object -Maximum).Maximum, 2)
    MaximumDeadlineLatenessMs = [math]::Round(
        ($maximumLatenessMeasurements | Measure-Object -Maximum).Maximum, 2)
    MaximumReadyDeadlineLatenessMs = [math]::Round(
        ($maximumReadyLatenessMeasurements | Measure-Object -Maximum).Maximum, 2)
    MaximumLateReadyImages =
        ($lateReadyCountMeasurements | Measure-Object -Maximum).Maximum
    MaximumInputInjectionMs = [math]::Round(
        ($injectionMeasurements | Measure-Object -Maximum).Maximum, 2)
    MaximumPipelineTailMs = [math]::Round(
        ($tailMeasurements | Measure-Object -Maximum).Maximum, 2)
    MaximumCommitMiB = [math]::Round(
        (($commitMeasurements | Measure-Object -Maximum).Maximum / 1MB), 1)
    MaximumWorkingSetMiB = [math]::Round(
        (($workingSetMeasurements | Measure-Object -Maximum).Maximum / 1MB), 1)
    MaximumManagedStorageMiB = [math]::Round(
        (($managedMemoryMeasurements | Measure-Object -Maximum).Maximum / 1MB), 1)
    SourceFilesUnchanged = $true
}
if ($minimum -lt $TargetFps) {
    throw "Performance target missed: minimum $([math]::Round($minimum, 2)) < $TargetFps images/s"
}
if (($lateReadyCountMeasurements | Measure-Object -Maximum).Maximum -ne 0) {
    throw "Ready deadline missed: at least one image became presentable after its 30 images/s deadline"
}

Write-Host 'PASS: performance and ready-deadline targets met in every run.'
