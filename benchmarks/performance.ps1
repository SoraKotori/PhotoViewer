#requires -Version 7.0

param(
    [ValidateRange(1, 100)]
    [int]$Runs = 3,
    [ValidateRange(0, 60000)]
    [int]$NavigationStartDelayMs = 0,
    [ValidateRange(1, 256)]
    [int]$Workers = 4,
    [ValidateRange(1, 4096)]
    [int]$StagingSlots = 8,
    [ValidateRange(1, 4096)]
    [int]$GpuForwardSlots = 2,
    [ValidateRange(0, 4096)]
    [int]$GpuReverseSlots = 1,
    [ValidateRange(1, 4096)]
    [int]$CompressedSlots = 8,
    [switch]$ShortNavigationPresses,
    [ValidateRange(0, 1000)]
    [int]$NavigationStepIntervalMs = 0,
    [ValidateRange(0.01, 1000.0)]
    [double]$TargetImagesPerSecond = 30.0,
    [ValidateSet('all', 'critical', 'non-idat', 'none')]
    [string]$PngChunkCrc = 'all',
    [ValidateSet('on', 'off')]
    [string]$PngAdler32 = 'on'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $projectRoot 'config\read-test-config.ps1')

$testConfig = Read-PhotoViewerTestConfig
$initialImagePath = $testConfig.Dataset.InitialImagePath
$navigationStepCount = $testConfig.Performance.NavigationStepCount
$navigationDirection = $testConfig.Performance.NavigationDirection
$initialImageBefore = Get-Item -LiteralPath $initialImagePath
$initialImageLengthBefore = $initialImageBefore.Length
$initialImageWriteTimeBefore = $initialImageBefore.LastWriteTimeUtc

if ($GpuForwardSlots + $GpuReverseSlots -gt 4096) {
    throw 'Combined GPU slot count exceeds 4096'
}

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

$viewerPath = Join-Path $projectRoot 'out\build\x64\Release\app\PhotoViewer.exe'
if (-not (Test-Path -LiteralPath $viewerPath -PathType Leaf)) {
    throw "Release executable not found: $viewerPath"
}

function Get-TrackedPipelineResourceSnapshotTotal {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Snapshot,
        [Parameter(Mandatory = $true)]
        [string]$Phase,
        [Parameter(Mandatory = $true)]
        [int]$Run
    )

    foreach ($field in @(
        'compressed_committed_bytes',
        'staging_committed_bytes',
        'gpu_bytes')) {
        if (-not $Snapshot.Contains($field)) {
            throw "Run $Run report phase $Phase did not contain a complete tracked-resource snapshot"
        }
    }

    return [int64]$Snapshot.compressed_committed_bytes +
        [int64]$Snapshot.staging_committed_bytes +
        [int64]$Snapshot.gpu_bytes
}

$runId = '{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'),
    [Guid]::NewGuid().ToString('N')
$benchmarkRunDirectory = Join-Path $projectRoot (Join-Path 'out\benchmarks' $runId)
[System.IO.Directory]::CreateDirectory($benchmarkRunDirectory) | Out-Null

$navigationSequence = $navigationDirection * $navigationStepCount
$navigationInputMode = if ($ShortNavigationPresses) { 'short' } else { 'hold' }
$pngValidationMode = "$PngChunkCrc-$PngAdler32"
$navigationRateMeasurements = @()
$navigationInputInjectionMeasurements = @()
$navigationPipelineTailMeasurements = @()
$peakCommitMeasurements = @()
$peakWorkingSetMeasurements = @()
$trackedPipelineResourceMeasurements = @()
$processStartToFirstPresentMeasurements = @()
$firstToLastPresentationRateMeasurements = @()
$maximumPresentationGapMeasurements = @()
$maximumPresentationDeadlineLatenessMeasurements = @()
$maximumReadyDeadlineLatenessMeasurements = @()
$lateReadyImageCountMeasurements = @()
for ($run = 1; $run -le $Runs; ++$run) {
    $reportPath = Join-Path $benchmarkRunDirectory `
        ("navigation-performance-report-$navigationDirection-$navigationInputMode-$pngValidationMode-$run-{0}.txt" -f
            [Guid]::NewGuid().ToString('N'))
    $viewerArguments = @(
        ('"' + $initialImagePath + '"'),
        "--workers=$Workers",
        "--staging-slot-count=$StagingSlots",
        "--gpu-forward-slot-count=$GpuForwardSlots",
        "--gpu-reverse-slot-count=$GpuReverseSlots",
        "--compressed-slot-count=$CompressedSlots",
        "--png-chunk-crc=$PngChunkCrc",
        "--png-adler32=$PngAdler32",
        "--validation-navigation=$navigationSequence",
        ('--validation-report="' + $reportPath + '"'),
        '--validation-timeout-ms=60000'
    )
    if ($NavigationStartDelayMs -gt 0) {
        $viewerArguments += "--validation-navigation-start-delay-ms=$NavigationStartDelayMs"
    }
    if ($NavigationStepIntervalMs -gt 0) {
        $viewerArguments += "--validation-navigation-interval-ms=$NavigationStepIntervalMs"
    }
    if ($ShortNavigationPresses) {
        $viewerArguments += '--validation-short-presses'
    }
    $viewerProcess = Start-Process `
        -FilePath $viewerPath `
        -ArgumentList $viewerArguments `
        -WindowStyle Hidden `
        -PassThru
    $processHandle = $viewerProcess.SafeHandle
    $viewerProcess.WaitForExit()
    $processMemory = [PhotoViewer.ProcessMemoryCountersEx]::new()
    $processMemory.Size = [Runtime.InteropServices.Marshal]::SizeOf($processMemory)
    if (-not [PhotoViewer.ProcessMemory]::GetProcessMemoryInfo(
        $processHandle.DangerousGetHandle(), [ref]$processMemory,
        $processMemory.Size)) {
        throw "GetProcessMemoryInfo failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    [int64]$peakCommitBytes = $processMemory.PeakPagefileUsage.ToUInt64()
    [int64]$peakWorkingSetBytes = $processMemory.PeakWorkingSetSize.ToUInt64()
    [GC]::KeepAlive($processHandle)
    $peakCommitMeasurements += $peakCommitBytes
    $peakWorkingSetMeasurements += $peakWorkingSetBytes
    if ($viewerProcess.ExitCode -ge 100 -and $viewerProcess.ExitCode -le 109) {
        throw "Run $run timed out at pipeline stage $($viewerProcess.ExitCode - 100)"
    }
    if ($viewerProcess.ExitCode -ne 0) {
        throw "Run $run failed with exit code $($viewerProcess.ExitCode)"
    }
    $finalReport = @{}
    $peakTrackedPipelineResourceBytes = 0L
    $trackedResourceSnapshot = @{}
    $currentReportPhase = $null
    $inFinalPhase = $false
    $sawNavigationComplete = $false
    foreach ($line in Get-Content -LiteralPath $reportPath) {
        if ($line -like 'phase=*') {
            if ($null -ne $currentReportPhase) {
                $trackedResourceTotal = Get-TrackedPipelineResourceSnapshotTotal `
                    -Snapshot $trackedResourceSnapshot `
                    -Phase $currentReportPhase `
                    -Run $run
                $peakTrackedPipelineResourceBytes = [math]::Max(
                    $peakTrackedPipelineResourceBytes, $trackedResourceTotal)
            }
            $currentReportPhase = $line.Substring('phase='.Length)
            $trackedResourceSnapshot = @{}
            $inFinalPhase = $line -eq 'phase=navigation-complete'
            if ($inFinalPhase) {
                $sawNavigationComplete = $true
            }
            continue
        }
        if ($inFinalPhase -and $line -match '^([^=]+)=(.*)$') {
            $finalReport[$matches[1]] = $matches[2]
        }
        if ($line -match '^(compressed_committed_bytes|staging_committed_bytes|gpu_bytes)=([0-9]+)$') {
            if ($null -eq $currentReportPhase) {
                throw "Run $run report contained tracked resources before its first phase"
            }
            if ($trackedResourceSnapshot.ContainsKey($matches[1])) {
                throw "Run $run report phase $currentReportPhase contained a duplicate tracked-resource value"
            }
            $trackedResourceSnapshot[$matches[1]] = [int64]$matches[2]
        }
    }
    if ($null -eq $currentReportPhase) {
        throw "Run $run report did not contain a phase"
    }
    $trackedResourceTotal = Get-TrackedPipelineResourceSnapshotTotal `
        -Snapshot $trackedResourceSnapshot `
        -Phase $currentReportPhase `
        -Run $run
    $peakTrackedPipelineResourceBytes = [math]::Max(
        $peakTrackedPipelineResourceBytes, $trackedResourceTotal)
    if (-not $sawNavigationComplete) {
        throw "Run $run report did not contain a completed navigation phase"
    }
    foreach ($field in @(
        'navigation_completion_nanoseconds',
        'navigation_injection_nanoseconds',
        'navigation_pipeline_tail_nanoseconds',
        'validation_presented_indices',
        'validation_presented_nanoseconds',
        'validation_ready_indices',
        'validation_ready_nanoseconds')) {
        if (-not $finalReport.ContainsKey($field)) {
            throw "Run $run completed report did not contain required field $field"
        }
    }
    if (-not $finalReport.ContainsKey('navigation_completion_nanoseconds') -or
        [int64]$finalReport.navigation_completion_nanoseconds -le 0) {
        throw "Run $run report did not contain a valid navigation duration"
    }
    $navigationElapsedMs =
        [double]$finalReport.navigation_completion_nanoseconds / 1.0e6
    $navigationImagesPerSecond =
        $navigationStepCount * 1000.0 / $navigationElapsedMs
    $navigationRateMeasurements += $navigationImagesPerSecond
    $trackedPipelineResourceMeasurements += $peakTrackedPipelineResourceBytes
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
    if ($presentedIndices.Count -ne $navigationStepCount + 1 -or
        $presentedNanoseconds.Count -ne $navigationStepCount + 1) {
        throw "Run $run reported $($presentedIndices.Count) presentations; $($navigationStepCount + 1) required"
    }
    if ($readyIndices.Count -ne $readyNanoseconds.Count) {
        throw "Run $run reported mismatched ready indices and timestamps"
    }
    $readyByIndex = @{}
    for ($index = 0; $index -lt $readyIndices.Count; ++$index) {
        $readyByIndex[$readyIndices[$index]] = $readyNanoseconds[$index]
    }
    $expectedDelta = if ($navigationDirection -eq 'R') { 1 } else { -1 }
    [double]$maximumPresentationGapMs = 0
    [double]$maximumPresentationDeadlineLatenessMs = 0
    [double]$maximumReadyDeadlineLatenessMs = [double]::NegativeInfinity
    [int]$lateReadyImageCount = 0
    $targetPeriodNanoseconds = 1.0e9 / $TargetImagesPerSecond
    $navigationTargetStartNanoseconds =
        $presentedNanoseconds[0] + $NavigationStartDelayMs * 1.0e6
    for ($ordinal = 0; $ordinal -lt $presentedIndices.Count; ++$ordinal) {
        if ($ordinal -gt 0 -and
            $presentedIndices[$ordinal] - $presentedIndices[$ordinal - 1] -ne
                $expectedDelta) {
            throw "Run $run presentation order is not contiguous at position $ordinal"
        }
        if ($ordinal -gt 0) {
            $gapMs = ($presentedNanoseconds[$ordinal] -
                      $presentedNanoseconds[$ordinal - 1]) / 1.0e6
            $maximumPresentationGapMs = [math]::Max(
                $maximumPresentationGapMs, $gapMs)
        }
        $deadline = if ($ordinal -eq 0) {
            $presentedNanoseconds[0]
        } else {
            $navigationTargetStartNanoseconds +
                $ordinal * $targetPeriodNanoseconds
        }
        $latenessMs = ($presentedNanoseconds[$ordinal] - $deadline) / 1.0e6
        $maximumPresentationDeadlineLatenessMs = [math]::Max(
            $maximumPresentationDeadlineLatenessMs, $latenessMs)
        $imageIndex = $presentedIndices[$ordinal]
        if (-not $readyByIndex.ContainsKey($imageIndex)) {
            throw "Run $run has no ready timestamp for image index $imageIndex"
        }
        $readyLatenessMs = ([int64]$readyByIndex[$imageIndex] - $deadline) / 1.0e6
        $maximumReadyDeadlineLatenessMs = [math]::Max(
            $maximumReadyDeadlineLatenessMs, $readyLatenessMs)
        if ($readyLatenessMs -gt 0) {
            ++$lateReadyImageCount
        }
    }
    $processStartToFirstPresentMs = $presentedNanoseconds[0] / 1.0e6
    $firstToLastPresentationElapsedMs = ($presentedNanoseconds[-1] -
                                         $presentedNanoseconds[0]) / 1.0e6
    $firstToLastPresentationImagesPerSecond =
        $navigationStepCount * 1000.0 / $firstToLastPresentationElapsedMs
    $processStartToFirstPresentMeasurements += $processStartToFirstPresentMs
    $firstToLastPresentationRateMeasurements +=
        $firstToLastPresentationImagesPerSecond
    $maximumPresentationGapMeasurements += $maximumPresentationGapMs
    $maximumPresentationDeadlineLatenessMeasurements +=
        $maximumPresentationDeadlineLatenessMs
    $maximumReadyDeadlineLatenessMeasurements +=
        $maximumReadyDeadlineLatenessMs
    $lateReadyImageCountMeasurements += $lateReadyImageCount
    $navigationInputInjectionMs =
        [double]$finalReport.navigation_injection_nanoseconds / 1.0e6
    $navigationPipelineTailMs =
        [double]$finalReport.navigation_pipeline_tail_nanoseconds / 1.0e6
    $navigationInputInjectionMeasurements += $navigationInputInjectionMs
    $navigationPipelineTailMeasurements += $navigationPipelineTailMs
    [pscustomobject]@{
        Run = $run
        NavigationElapsedMs = [math]::Round($navigationElapsedMs, 2)
        NavigationImagesPerSecond =
            [math]::Round($navigationImagesPerSecond, 2)
        ProcessStartToFirstPresentMs =
            [math]::Round($processStartToFirstPresentMs, 2)
        FirstToLastPresentationImagesPerSecond =
            [math]::Round($firstToLastPresentationImagesPerSecond, 2)
        MaximumPresentationGapMs =
            [math]::Round($maximumPresentationGapMs, 2)
        MaximumPresentationDeadlineLatenessMs =
            [math]::Round($maximumPresentationDeadlineLatenessMs, 2)
        MaximumReadyDeadlineLatenessMs =
            [math]::Round($maximumReadyDeadlineLatenessMs, 2)
        LateReadyImageCount = $lateReadyImageCount
        NavigationInputInjectionMs =
            [math]::Round($navigationInputInjectionMs, 2)
        NavigationPipelineTailMs =
            [math]::Round($navigationPipelineTailMs, 2)
        PeakCommitMiB = [math]::Round($peakCommitBytes / 1MB, 1)
        PeakWorkingSetMiB = [math]::Round($peakWorkingSetBytes / 1MB, 1)
        PeakTrackedPipelineResourceMiB =
            [math]::Round($peakTrackedPipelineResourceBytes / 1MB, 1)
    }
}

$initialImageAfter = Get-Item -LiteralPath $initialImagePath
$initialImageHashAfter = (
    Get-FileHash -LiteralPath $initialImagePath -Algorithm SHA256).Hash
if ($initialImageAfter.Length -ne $initialImageLengthBefore -or
    $initialImageAfter.LastWriteTimeUtc -ne $initialImageWriteTimeBefore -or
    $initialImageHashAfter -cne $testConfig.Dataset.InitialImageSha256) {
    throw 'Configured initial image changed or no longer matches its SHA-256'
}

$minimumNavigationRate =
    ($navigationRateMeasurements | Measure-Object -Minimum).Minimum
$averageNavigationRate =
    ($navigationRateMeasurements | Measure-Object -Average).Average
[pscustomobject]@{
    Runs = $Runs
    Workers = $Workers
    StagingSlots = $StagingSlots
    GpuForwardSlots = $GpuForwardSlots
    GpuReverseSlots = $GpuReverseSlots
    CompressedSlots = $CompressedSlots
    NavigationDirection = $navigationDirection
    NavigationInputMode = $navigationInputMode
    PngChunkCrc = $PngChunkCrc
    PngAdler32 = $PngAdler32
    NavigationStepsPerRun = $navigationStepCount
    NavigationStartDelayMs = $NavigationStartDelayMs
    NavigationStepIntervalMs = $NavigationStepIntervalMs
    TargetImagesPerSecond = $TargetImagesPerSecond
    MinimumNavigationImagesPerSecond =
        [math]::Round($minimumNavigationRate, 2)
    AverageNavigationImagesPerSecond =
        [math]::Round($averageNavigationRate, 2)
    MaximumProcessStartToFirstPresentMs = [math]::Round(
        ($processStartToFirstPresentMeasurements |
            Measure-Object -Maximum).Maximum, 2)
    MinimumFirstToLastPresentationImagesPerSecond = [math]::Round(
        ($firstToLastPresentationRateMeasurements |
            Measure-Object -Minimum).Minimum, 2)
    AverageFirstToLastPresentationImagesPerSecond = [math]::Round(
        ($firstToLastPresentationRateMeasurements |
            Measure-Object -Average).Average, 2)
    MaximumPresentationGapMs = [math]::Round(
        ($maximumPresentationGapMeasurements |
            Measure-Object -Maximum).Maximum, 2)
    MaximumPresentationDeadlineLatenessMs = [math]::Round(
        ($maximumPresentationDeadlineLatenessMeasurements |
            Measure-Object -Maximum).Maximum, 2)
    MaximumReadyDeadlineLatenessMs = [math]::Round(
        ($maximumReadyDeadlineLatenessMeasurements |
            Measure-Object -Maximum).Maximum, 2)
    MaximumLateReadyImageCount =
        ($lateReadyImageCountMeasurements | Measure-Object -Maximum).Maximum
    MaximumNavigationInputInjectionMs = [math]::Round(
        ($navigationInputInjectionMeasurements |
            Measure-Object -Maximum).Maximum, 2)
    MaximumNavigationPipelineTailMs = [math]::Round(
        ($navigationPipelineTailMeasurements |
            Measure-Object -Maximum).Maximum, 2)
    MaximumCommitMiB = [math]::Round(
        (($peakCommitMeasurements | Measure-Object -Maximum).Maximum / 1MB), 1)
    MaximumWorkingSetMiB = [math]::Round(
        (($peakWorkingSetMeasurements | Measure-Object -Maximum).Maximum / 1MB), 1)
    MaximumTrackedPipelineResourceMiB = [math]::Round(
        (($trackedPipelineResourceMeasurements |
            Measure-Object -Maximum).Maximum / 1MB), 1)
    InitialImageUnchanged = $true
}
if ($minimumNavigationRate -lt $TargetImagesPerSecond) {
    throw "Navigation performance target missed: minimum $([math]::Round($minimumNavigationRate, 2)) < $TargetImagesPerSecond images/s"
}
if (($lateReadyImageCountMeasurements | Measure-Object -Maximum).Maximum -ne 0) {
    throw "Ready deadline missed: at least one image became presentable after its $TargetImagesPerSecond images/s deadline"
}

Write-Host 'PASS: navigation performance and ready-deadline targets met in every run.'
