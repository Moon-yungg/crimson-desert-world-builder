param(
    [ValidateSet('Codec','ProjectLifecycle','AnchorTransform','PlacementLifecycle','Export','GroupingHistory','UiShell','InputOwnership','ProjectTable','OverlayBinding','All')][string]$Suite = 'All',
    [ValidateSet('Host','Live')][string]$Mode = 'Host',
    [string]$EvidenceRoot = '.omo/evidence/wb079-upstream/A/codec'
)
function Register-Suites {
    param([object[]]$Files)
    # Public suites are fixed; each independently compiled fixture contributes to one suite.
    $suiteOf = @{
        codec='Codec'
        project_lifecycle='ProjectLifecycle'
        anchor_transform='AnchorTransform'
        grounding='AnchorTransform'
        spawn_lifetime='PlacementLifecycle'
        placement_lifecycle='PlacementLifecycle'
        group_placement='PlacementLifecycle'
        export='Export'
        grouping_history='GroupingHistory'
        project_table='ProjectTable'
        ui_shell='UiShell'
        input_ownership='InputOwnership'
        overlay_binding='OverlayBinding'
    }
    $registered = @{}
    foreach ($file in $Files) {
        $stem = $file.BaseName -replace '_tests$',''
        if (!$suiteOf.ContainsKey($stem)) { throw "Unregistered test fixture: $($file.FullName)" }
        $key = $suiteOf[$stem]
        if (!$registered.ContainsKey($key)) { $registered[$key] = @() }
        $registered[$key] += $file.FullName
    }
    return $registered
}
function Assert-SuiteMapping {
    # In-memory planned filenames: no placeholder test files or fake executables.
    $planned = @('codec','project_lifecycle','anchor_transform','placement_lifecycle',
                 'export','grouping_history','project_table','spawn_lifetime','grounding','group_placement')
    foreach ($stage in @('C','D1')) {
        $stems = $planned
        if ($stage -eq 'D1') { $stems = $planned + @('ui_shell','input_ownership','overlay_binding') }
        $files = @($stems | ForEach-Object {
            [pscustomobject]@{ BaseName="${_}_tests"; FullName="${_}_tests.cpp" }
        })
        $registered = Register-Suites -Files $files
        $expected = if ($stage -eq 'C') { 7 } else { 10 }
        if ($registered.Count -ne $expected -or $registered.PlacementLifecycle.Count -ne 3 -or
            $registered.AnchorTransform.Count -ne 2) {
            throw "Public suite mapping mismatch at $stage"
        }
    }
}
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$evidence = [IO.Path]::GetFullPath((Join-Path $root $EvidenceRoot))
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$log = Join-Path $evidence 'run.log'
$result = Join-Path $evidence 'result.json'
$receipt = [ordered]@{ suite=$Suite; mode=$Mode; status='FAIL'; exitCode=1; assertions=0; suiteAssertions=@{}; fixtures=@(); sources=@(); sourceHashes=@(); binaryHash=$null; command='' }
try {
    Assert-SuiteMapping
    if ($Mode -eq 'Live') { throw 'BLOCKED: Live adapter unavailable' }
    $files = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*_tests.cpp' -File)
    $registered = Register-Suites -Files $files
    $selected = if ($Suite -eq 'All') { @($registered.Keys | Sort-Object) } else { @($Suite) }
    if ($selected.Count -eq 0) { throw 'No registered suites' }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (!(Test-Path $vswhere)) { throw 'MSVC vswhere unavailable' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$vs) { throw 'MSVC x64 compiler unavailable' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (!(Test-Path $vcvars)) { throw 'MSVC x64 environment unavailable' }
    $total = 0
    foreach ($name in $selected) {
        if (!$registered.ContainsKey($name)) { throw "Missing or zero tests for selected suite $name" }
        $receipt.suiteAssertions[$name] = 0
        foreach ($testFile in $registered[$name]) {
        $stem = ([IO.Path]::GetFileNameWithoutExtension($testFile) -replace '_tests$','')
        $sourceList = Join-Path $PSScriptRoot "$stem.sources"
        $includeArgs = ''
        if ($name -eq 'Codec') {
            $production = @((Join-Path $root 'asi/cdmodkit/proj_codec.cpp'))
        } elseif (Test-Path $sourceList) {
            $entries = @(Get-Content $sourceList | Where-Object { $_ -and !$_.StartsWith('#') })
            foreach ($entry in $entries | Where-Object { $_.StartsWith('@include ') }) {
                $directory = Join-Path $root $entry.Substring(9)
                if (!(Test-Path -LiteralPath $directory -PathType Container)) { throw "Missing include directory: $directory" }
                $includeArgs += ' /I"' + $directory + '"'
            }
            $production = @($entries | Where-Object { !$_.StartsWith('@include ') } | ForEach-Object { Join-Path $root $_ })
        } else {
            $production = @((Join-Path $root "asi/cdmodkit/$stem.cpp"))
        }
        $sources = @($testFile) + $production
        foreach ($src in $sources) { if (!(Test-Path $src)) { throw "Missing production/test source: $src" } }
        $receipt.sources += $sources
        $receipt.sourceHashes += @($sources | ForEach-Object { [ordered]@{ path=$_; sha256=(Get-FileHash $_ -Algorithm SHA256).Hash } })
        $binary = Join-Path $evidence "$stem.exe"
        $args = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $compile = 'call "' + $vcvars + '" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 ' + $includeArgs + ' ' + $args + ' /Fo:"' + $evidence + '\\" /Fd:"' + $evidence + '\\" /Fe:"' + $binary + '"'
        $receipt.command += "$compile`n"
        "COMMAND: $compile" | Out-File -Append -Encoding utf8 $log
        $output = & cmd.exe /d /c $compile 2>&1
        $output | Out-File -Append -Encoding utf8 $log
        if ($LASTEXITCODE -ne 0) { throw "Compiler failed for $name (exit $LASTEXITCODE)" }
        $receipt.binaryHash = (Get-FileHash $binary -Algorithm SHA256).Hash
        $output = & $binary (Join-Path $evidence (Join-Path 'fixtures' $stem)) 2>&1
        $exit = $LASTEXITCODE
        $output | Out-File -Append -Encoding utf8 $log
        if ($exit -ne 0) { throw "Suite $name failed (exit $exit)" }
        "EXIT: $exit" | Out-File -Append -Encoding utf8 $log
        $matches = @($output | Select-String -Pattern '^ASSERTIONS=([1-9][0-9]*)$')
        if ($matches.Count -ne 1) { throw "Suite $name returned zero or missing assertions" }
        $count = [int]$matches[0].Matches[0].Groups[1].Value
        $total += $count
        $receipt.suiteAssertions[$name] += $count
        $receipt.fixtures += [ordered]@{ suite=$name; test=$testFile; assertions=$count; exitCode=$exit; binaryHash=$receipt.binaryHash }
        }
    }
    $receipt.assertions = $total
    $receipt.exitCode = 0
    $receipt.status = 'PASS'
} catch {
    $receipt.error = $_.Exception.Message
    $_ | Out-String | Out-File -Append -Encoding utf8 $log
} finally {
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $result
}
Write-Output "${Suite}: $($receipt.status), assertions=$($receipt.assertions); $result"
exit $receipt.exitCode
