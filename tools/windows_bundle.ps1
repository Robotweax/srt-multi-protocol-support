$ErrorActionPreference = 'Stop'
cmake --install build --prefix build/package --component SrtMux
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$bin = (Resolve-Path build/package/bin).Path
$redist = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
$ssl = 'C:/Program Files/OpenSSL'
$search = @($redist, "$ssl/bin", "$ssl", "$env:WINDIR/System32")
$queue = [Collections.Generic.Queue[string]]::new()
Get-ChildItem build/package -Recurse -Include *.exe,*.dll | ForEach-Object { $queue.Enqueue($_.FullName) }
while ($queue.Count) {
    $binary = $queue.Dequeue()
    $imports = dumpbin /nologo /dependents $binary
    if ($LASTEXITCODE) { throw "Cannot inspect $binary" }
    foreach ($line in $imports) {
        if ($line -notmatch '^\s+([\w.\-]+\.dll)\s*$') { continue }
        $dll = $Matches[1]
        if ($dll -match '^(api-ms-|ext-ms-)' -or (Test-Path "$bin/$dll")) { continue }
        $source = $search | ForEach-Object { Join-Path $_ $dll } | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (!$source) { throw "Missing dependency $dll of $binary" }
        if ($source.StartsWith("$env:WINDIR/System32", [StringComparison]::OrdinalIgnoreCase) -or $source -like "$env:WINDIR\System32\*") {
            if ($dll -match '^(msvcp1|vcruntime|libcrypto|libssl)') { throw "Missing redistributable $dll" }
            continue
        }
        Copy-Item $source "$bin/$dll"
        $queue.Enqueue("$bin/$dll")
    }
}
Copy-Item tools/WINDOWS-DE.txt build/package
Copy-Item tools/OpenSSL-LICENSE.txt build/package/share/srtmux/licenses/
python tools/write_bundle_manifest.py build/package
if ($LASTEXITCODE) { exit $LASTEXITCODE }
New-Item dist -ItemType Directory -Force | Out-Null
$zip = "dist/srt-live-transmit-windows-x64.zip"
Compress-Archive -Path build/package/* -DestinationPath $zip
Expand-Archive $zip "$env:RUNNER_TEMP/Relay Test"
$python = (Get-Command python).Source
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
& $python tests/windows_bundle_transfer.py "$env:RUNNER_TEMP/Relay Test"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Get-FileHash $zip
