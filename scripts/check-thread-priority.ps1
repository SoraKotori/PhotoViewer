param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,
    [string]$Binary,
    [string]$Dumpbin
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $ProjectRoot).Path
$self = $MyInvocation.MyCommand.Path
$extensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp',
                '.ps1', '.props', '.targets', '.vcxproj')
$excludedDirectories = @('.git', 'x64', 'build', 'benchmark-results')
$excludedPrefixes = $excludedDirectories | ForEach-Object {
    Join-Path $root ($_ + [IO.Path]::DirectorySeparatorChar)
}

$sourceFiles = Get-ChildItem -LiteralPath $root -File -Recurse | Where-Object {
    $path = $_.FullName
    $extensions -contains $_.Extension.ToLowerInvariant() -and
    $path -ne $self -and
    -not ($excludedPrefixes | Where-Object {
        $path.StartsWith($_, [StringComparison]::OrdinalIgnoreCase)
    })
}

$forbiddenSourcePatterns = @(
    '\bSetThreadPriority(?:Boost)?\s*\(',
    '\bSetPriorityClass\s*\(',
    '\bSetProcessPriorityBoost\s*\(',
    '\bSetThreadInformation\s*\(',
    '\bSetProcessInformation\s*\(',
    '\bAvSetMmThread(?:Characteristics(?:A|W)?|Priority)\s*\(',
    '\b(?:Nt|Zw)SetInformationThread\s*\(',
    '\bRtlSetThreadPriority\s*\(',
    '\bTHREAD_PRIORITY_(?!ERROR_RETURN\b)[A-Z_]+\b',
    '\b(?:REALTIME|HIGH|ABOVE_NORMAL|BELOW_NORMAL|IDLE)_PRIORITY_CLASS\b'
)

$violations = foreach ($file in $sourceFiles) {
    Select-String -LiteralPath $file.FullName -Pattern $forbiddenSourcePatterns
}
if ($violations) {
    $details = $violations | ForEach-Object {
        '{0}:{1}: {2}' -f $_.Path, $_.LineNumber, $_.Line.Trim()
    }
    throw "Thread/process priority mutation is forbidden:`n$($details -join "`n")"
}

if ($Binary) {
    $binaryPath = (Resolve-Path -LiteralPath $Binary).Path
    if (-not $Dumpbin) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} `
            'Microsoft Visual Studio\Installer\vswhere.exe'
        $installationPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if (-not $installationPath) { throw 'Visual C++ tools were not found' }
        $versionFile = Join-Path $installationPath `
            'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
        $toolsVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
        $Dumpbin = Join-Path $installationPath `
            "VC\Tools\MSVC\$toolsVersion\bin\Hostx64\x64\dumpbin.exe"
    }
    if (-not (Test-Path -LiteralPath $Dumpbin)) {
        throw "dumpbin.exe was not found: $Dumpbin"
    }

    $imports = & $Dumpbin /nologo /imports $binaryPath
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect executable imports: $LASTEXITCODE"
    }
    $forbiddenImports = @(
        'SetThreadPriority',
        'SetThreadPriorityBoost',
        'SetPriorityClass',
        'SetProcessPriorityBoost',
        'SetThreadInformation',
        'SetProcessInformation',
        'AvSetMmThreadCharacteristics',
        'AvSetMmThreadPriority',
        'NtSetInformationThread',
        'ZwSetInformationThread',
        'RtlSetThreadPriority'
    )
    $importViolations = $imports | Select-String -SimpleMatch `
        -Pattern $forbiddenImports
    if ($importViolations) {
        throw "Executable imports a priority-mutation API:`n$($importViolations -join "`n")"
    }
}

Write-Host 'Thread priority policy: PASS (OS scheduler defaults only)'
