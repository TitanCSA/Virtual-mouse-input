param(
    [string]$Version = '1.0.0',
    [Parameter(Mandatory = $true)][string]$ReleaseDirectory,
    [switch]$Publish
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$git = (Get-Command git.exe -ErrorAction Stop).Source
$owner = 'TitanCSA'; $name = 'Virtual-mouse-input'
$repositoryUrl = "https://github.com/$owner/$name"
$installerHash = 'A47DFC17536F350BDCB47CC786FB25EEEE74282CA6D282FD9719C775FFF6ED56'
if ($Version -cne '1.0.0') { throw 'Review the release metadata before changing versions.' }
$assets = @("Virtual-mouse-input-$Version-win64.zip", 'VdeviceSetup.exe', 'SHA256SUMS.txt')
$releaseRoot = (Resolve-Path -LiteralPath $ReleaseDirectory).Path
foreach ($asset in $assets) {
    $item = Get-Item -LiteralPath (Join-Path $releaseRoot $asset)
    if ($item.PSIsContainer -or !$item.Length -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw ('Invalid release asset: ' + $asset)
    }
}
if ((Get-FileHash -LiteralPath (Join-Path $releaseRoot 'VdeviceSetup.exe')).Hash -cne $installerHash) {
    throw 'The installer differs from the reviewed Vdevice 1.0.5.0 demo asset.'
}
$hashLines = @(Get-Content -LiteralPath (Join-Path $releaseRoot 'SHA256SUMS.txt') | Where-Object { $_.Trim() })
if ($hashLines.Count -ne 2) { throw 'SHA256SUMS.txt must describe the application ZIP and installer.' }
foreach ($asset in $assets[0..1]) {
    $expected = (Get-FileHash -LiteralPath (Join-Path $releaseRoot $asset)).Hash
    if (@($hashLines | Where-Object { $_ -match ('^(?i)' + $expected + '\s+\*?' + [regex]::Escape($asset) + '$') }).Count -ne 1) {
        throw ('Missing or incorrect checksum: ' + $asset)
    }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $releaseRoot $assets[0]))
try {
    $names = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    foreach ($required in @('VirtualMouseInput.exe', 'Driver/VdeviceSetup.exe', 'README.md', 'LICENSE', 'docs/BINARY-LICENSE.txt')) {
        if (@($names | Where-Object { $_ -ceq $required }).Count -ne 1) { throw ('Missing ZIP file: ' + $required) }
    }
    foreach ($path in $names) {
        if ($path -match '(^/|(^|/)\.\.(/|$)|(^|/)(DriverSource|TrialClock|Shared)(/|$)|\.(sys|pdb|pfx|pem|key|clixml)$)') {
            throw ('Unexpected application ZIP path: ' + $path)
        }
    }
    $stream = $zip.GetEntry('Driver/VdeviceSetup.exe').Open()
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $embeddedHash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
    if ($embeddedHash -cne $installerHash) { throw 'The bundled Vdevice installer differs from the reviewed demo.' }
} finally { $zip.Dispose() }
$sources = @('.gitignore', 'CMakeLists.txt', 'Build.ps1', 'README.md', 'LICENSE',
             'Vdevice/Vdevice.h', 'Vdevice/Vdevice.cpp', 'docs/BINARY-LICENSE.txt',
             'docs/images/app.png', 'Tools/Publish.ps1')
$sources += @(Get-ChildItem -LiteralPath (Join-Path $root 'App') -File |
    Where-Object { $_.Extension -in @('.cpp', '.h', '.rc', '.manifest', '.ico') } |
    ForEach-Object { 'App/' + $_.Name })
$sources += @(Get-ChildItem -LiteralPath (Join-Path $root 'Tests') -File |
    Where-Object { $_.Extension -in @('.cpp', '.h', '.ps1') } |
    ForEach-Object { 'Tests/' + $_.Name })
foreach ($source in $sources) {
    $item = Get-Item -LiteralPath (Join-Path $root $source)
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw ('Invalid source: ' + $source) }
}
Write-Host ('Reviewed source files: ' + $sources.Count)
Write-Host ('Release assets: ' + ($assets -join ', '))
if (!$Publish) { Write-Host 'Preflight passed. No GitHub changes made.'; exit 0 }

$env:GIT_TERMINAL_PROMPT = '0'; $env:GCM_INTERACTIVE = 'Never'
$lines = "protocol=https`nhost=github.com`nusername=$owner`n`n" | & $git credential fill
if ($LASTEXITCODE) { throw 'GitHub authentication is unavailable.' }
$credential = @{}
foreach ($line in $lines) { $split = $line.IndexOf('='); if ($split -gt 0) { $credential[$line.Substring(0, $split)] = $line.Substring($split + 1) } }
if (!$credential.password) { throw 'GitHub authentication did not return a credential.' }
$headers = @{ Authorization = ('Bearer ' + $credential.password); Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28'; 'User-Agent' = 'VirtualMouseInput-Publication' }
function Api([string]$Method, [string]$Path, $Body = $null) {
    $arguments = @{ Method = $Method; Uri = ('https://api.github.com' + $Path); Headers = $headers }
    if ($null -ne $Body) { $arguments.ContentType = 'application/json; charset=utf-8'; $arguments.Body = [Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Depth 8 -Compress)) }
    Invoke-RestMethod @arguments
}
function Git([string[]]$Arguments) {
    & $git -C $root @Arguments
    if ($LASTEXITCODE) { throw ('Git operation failed: ' + $Arguments[0]) }
}
try {
    $user = Api GET '/user'
    if ($user.login -cne $owner) { throw 'Unexpected authenticated GitHub account.' }
    $repo = $null
    try { $repo = Api GET ("/repos/$owner/$name") }
    catch { if (!$_.Exception.Response -or [int]$_.Exception.Response.StatusCode -ne 404) { throw } }
    $description = 'C++ virtual USB HID mouse example using the Microsoft-signed Vdevice kernel driver: buttons, relative X/Y movement and scrolling. Windows 10/11 x64.'
    if (!$repo) { $repo = Api POST '/user/repos' @{ name = $name; description = $description; private = $false; auto_init = $false; has_wiki = $false } }
    if ($repo.full_name -cne "$owner/$name" -or $repo.private) { throw 'Unexpected repository destination.' }
    if (!(Test-Path -LiteralPath (Join-Path $root '.git'))) { Git -Arguments @('init', '-b', 'main') }
    Git -Arguments @('config', 'user.name', $owner)
    Git -Arguments @('config', 'user.email', ($user.id.ToString() + '+' + $owner + '@users.noreply.github.com'))
    $remotes = @(& $git -C $root remote)
    if ($remotes -contains 'origin') {
        $origin = (& $git -C $root remote get-url origin).Trim()
        if ($origin -cne "$repositoryUrl.git") { throw 'Refusing an unexpected Git remote.' }
    } else { Git -Arguments @('remote', 'add', 'origin', "$repositoryUrl.git") }
    if ((& $git -C $root branch --show-current).Trim() -cne 'main') { throw 'The publication branch must be main.' }
    Git -Arguments (@('add', '--') + $sources)
    $tracked = @(& $git -C $root ls-files)
    foreach ($path in $tracked) { if ($sources -cnotcontains $path) { throw ('Unreviewed tracked source: ' + $path) } }
    $changes = @(& $git -C $root diff --cached --name-only)
    if ($changes.Count) { Git -Arguments @('commit', '-m', 'Add Virtual Mouse Input C++ Vdevice example') }
    $commit = (& $git -C $root rev-parse HEAD).Trim()
    Git -Arguments @('push', '--set-upstream', 'origin', 'main')
    $null = Api PATCH ("/repos/$owner/$name") @{ description = $description; homepage = "$repositoryUrl#readme"; default_branch = 'main'; has_wiki = $false }
    $null = Api PUT ("/repos/$owner/$name/topics") @{ names = @('windows', 'cpp', 'virtual-mouse', 'mouse', 'hid', 'usb', 'virtual-device', 'kernel-driver', 'vdevice', 'input', 'accessibility', 'example', 'win32', 'direct2d') }
    $release = $null
    try { $release = Api GET ("/repos/$owner/$name/releases/tags/v$Version") }
    catch { if (!$_.Exception.Response -or [int]$_.Exception.Response.StatusCode -ne 404) { throw } }
    if ($release) { throw 'This release already exists. Refusing to replace reviewed downloads automatically.' }
    $body = @"
A compact C++ example of [Vdevice](https://github.com/TitanCSA/Vdevice): the virtual HID mouse connects on opening and disconnects on closing. Send five mouse buttons, relative X/Y movement and vertical/horizontal scrolling.

**Windows 10/11 x64.** Install **VdeviceSetup.exe**, restart if requested, then extract **$($assets[0])** and open **VirtualMouseInput.exe** as administrator. The same driver installer is also included inside the ZIP under **Driver/**.

The Vdevice demo provides **one hour per Windows kernel boot**, starting at first successful device creation and shared across Vdevice applications. Windows **Restart** resets the allowance. Microsoft signs the driver package; application and installer EXEs are separate binaries.

Application and two-file SDK source are included in this repository under MIT; kernel driver source is not included. See the README for controls, building and binary distribution terms. Offline tests validate protocol/encoding; this release does not claim universal application compatibility or measured hardware latency.

Check **SHA256SUMS.txt** against downloaded files.
"@
    $release = Api POST ("/repos/$owner/$name/releases") @{ tag_name = "v$Version"; target_commitish = $commit; name = "Virtual Mouse Input $Version - C++ Vdevice Example"; body = $body; draft = $true; prerelease = $false }
    foreach ($asset in $assets) {
        $uploadUrl = $release.upload_url.Split('{')[0] + '?name=' + [Uri]::EscapeDataString($asset)
        $uploaded = Invoke-RestMethod -Method Post -Uri $uploadUrl -Headers $headers -ContentType 'application/octet-stream' -InFile (Join-Path $releaseRoot $asset)
        $local = Get-Item -LiteralPath (Join-Path $releaseRoot $asset)
        $digest = 'sha256:' + (Get-FileHash -LiteralPath $local.FullName).Hash.ToLowerInvariant()
        if ($uploaded.name -cne $asset -or $uploaded.size -ne $local.Length -or $uploaded.digest -cne $digest) { throw ('Uploaded asset failed verification: ' + $asset) }
    }
    $release = Api PATCH ("/repos/$owner/$name/releases/" + $release.id) @{ draft = $false; make_latest = 'true' }
    $verificationRoot = Join-Path $root ('obj\Published-' + $Version)
    New-Item -ItemType Directory -Path $verificationRoot -Force | Out-Null
    foreach ($asset in $release.assets) {
        if ($assets -cnotcontains $asset.name) { throw 'Unexpected published asset.' }
        $download = Join-Path $verificationRoot $asset.name
        Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $download
        if ((Get-FileHash -LiteralPath $download).Hash -cne (Get-FileHash -LiteralPath (Join-Path $releaseRoot $asset.name)).Hash) { throw ('Published download failed verification: ' + $asset.name) }
    }
    [ordered]@{ Repository = $repositoryUrl; Commit = $commit; Release = $release.html_url; Assets = @($release.assets | Select-Object name, size, digest); VerifiedUtc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $verificationRoot 'Publication.json') -Encoding UTF8
    Write-Host ('Published and download-verified: ' + $release.html_url)
} finally { $headers = $null; $credential.Clear(); $lines = $null }
