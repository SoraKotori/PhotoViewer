#requires -Version 7.0

function Assert-PhotoViewerTestConfigFields {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Section,
        [Parameter(Mandatory = $true)]
        [string]$SectionName,
        [Parameter(Mandatory = $true)]
        [string[]]$RequiredFields
    )

    foreach ($field in $RequiredFields) {
        if ($Section.Keys -cnotcontains $field) {
            throw "Private test configuration $SectionName is missing $field"
        }
    }
    foreach ($field in $Section.Keys) {
        if ($RequiredFields -cnotcontains [string]$field) {
            throw "Private test configuration $SectionName contains an unsupported field"
        }
    }
}

function Read-PhotoViewerTestConfig {
    $configPath = Join-Path $PSScriptRoot 'test.local.psd1'
    try {
        $data = Import-PowerShellDataFile -LiteralPath (
            Resolve-Path -LiteralPath $configPath -ErrorAction Stop).Path
    } catch {
        throw 'Private test configuration could not be loaded'
    }
    if ($data -isnot [System.Collections.IDictionary]) {
        throw 'Private test configuration must be a data dictionary'
    }

    $allowedSections = @('Dataset', 'AppIntegration', 'Performance')
    foreach ($configuredSection in $data.Keys) {
        if ($allowedSections -cnotcontains [string]$configuredSection) {
            throw 'Private test configuration contains an unsupported section'
        }
    }
    foreach ($requiredSection in $allowedSections) {
        if ($data.Keys -cnotcontains $requiredSection) {
            throw "Private test configuration is missing $requiredSection"
        }
        if ($data[$requiredSection] -isnot [System.Collections.IDictionary]) {
            throw "Private test configuration $requiredSection must be a data dictionary"
        }
    }

    $dataset = $data.Dataset
    Assert-PhotoViewerTestConfigFields `
        -Section $dataset `
        -SectionName 'Dataset' `
        -RequiredFields @('InitialImagePath', 'InitialImageSha256')

    $initialImageValue = [string]$dataset.InitialImagePath
    if (-not [IO.Path]::IsPathFullyQualified($initialImageValue)) {
        throw 'Private test configuration Dataset.InitialImagePath must be an absolute path'
    }
    try {
        $initialImagePath = (Resolve-Path -LiteralPath $initialImageValue -ErrorAction Stop).Path
    } catch {
        throw 'Private test configuration Dataset.InitialImagePath is invalid'
    }
    if (-not (Test-Path -LiteralPath $initialImagePath -PathType Leaf) -or
        [IO.Path]::GetExtension($initialImagePath) -ine '.png') {
        throw 'Private test configuration Dataset.InitialImagePath must identify a PNG file'
    }

    $initialImageSha256 = [string]$dataset.InitialImageSha256
    if ($initialImageSha256 -notmatch '\A[0-9A-Fa-f]{64}\z') {
        throw 'Private test configuration Dataset.InitialImageSha256 is invalid'
    }

    $appIntegration = $data.AppIntegration
    Assert-PhotoViewerTestConfigFields `
        -Section $appIntegration `
        -SectionName 'AppIntegration' `
        -RequiredFields @('NavigationSequence')

    $navigationSequence = [string]$appIntegration.NavigationSequence
    if ($navigationSequence -notmatch '\A[LR]*\z') {
        throw 'Private test configuration AppIntegration.NavigationSequence is invalid'
    }

    $performance = $data.Performance
    Assert-PhotoViewerTestConfigFields `
        -Section $performance `
        -SectionName 'Performance' `
        -RequiredFields @('NavigationStepCount', 'NavigationDirection')

    $navigationStepCount = [int]$performance.NavigationStepCount
    if ($navigationStepCount -lt 1 -or $navigationStepCount -gt 1000) {
        throw 'Private test configuration Performance.NavigationStepCount must be between 1 and 1000'
    }
    $navigationDirection = [string]$performance.NavigationDirection
    if ($navigationDirection -cne 'L' -and $navigationDirection -cne 'R') {
        throw 'Private test configuration Performance.NavigationDirection must be L or R'
    }

    [pscustomobject]@{
        Dataset = [pscustomobject]@{
            InitialImagePath = $initialImagePath
            InitialImageSha256 = $initialImageSha256.ToUpperInvariant()
        }
        AppIntegration = [pscustomobject]@{
            NavigationSequence = $navigationSequence
        }
        Performance = [pscustomobject]@{
            NavigationStepCount = $navigationStepCount
            NavigationDirection = $navigationDirection
        }
    }
}
