[CmdletBinding()]
param([ValidateSet('All','Build','Test','Package')][string]$Target='All',[switch]$Render,[switch]$AddressSanitizer,[string]$DriverInstaller)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Install Visual Studio 2022 Desktop development with C++.'}
$cmake=(Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if(!$cmake){$cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'}
$build=Join-Path $root $(if($AddressSanitizer){'obj\Asan'}else{'obj\Build'})
$originalPath=$env:PATH
try{
 $asan=if($AddressSanitizer){'ON'}else{'OFF'}
 if($AddressSanitizer){
  $toolset=Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory|Where-Object {[version]$_.Name -ge [version]'14.38.0' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'))}|Sort-Object {[version]$_.Name} -Descending|Select-Object -First 1
  if(!$toolset){throw 'Install the current Visual Studio C++ AddressSanitizer component.'}
  $env:PATH=(Join-Path $toolset.FullName 'bin\Hostx64\x64')+';'+$env:PATH
 }
 if($Target -ne 'Test'){
  $toolsetArgs=@();if($AddressSanitizer){$toolsetArgs=@('-T',('version='+$toolset.Name))}
  & $cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 @toolsetArgs ('-DVMI_ASAN='+$asan)
  if($LASTEXITCODE){throw 'CMake configuration failed.'}
  $buildArgs=@();if($AddressSanitizer){$buildArgs=@('--',('/p:VCToolsVersion='+$toolset.Name),'/p:PreferredToolArchitecture=x64')}
  & $cmake --build $build --config Release --parallel @buildArgs
  if($LASTEXITCODE){throw 'C++ build failed.'}
 }
 function OfflineApp([string[]]$Arguments){
  $process=Start-Process -FilePath (Join-Path $build 'bin\VirtualMouseInput.exe') -ArgumentList $Arguments -WindowStyle Hidden -PassThru
  $heldHandle=$process.Handle
  try{if(!$process.WaitForExit(30000)){throw 'Offline UI test timed out.'};if($process.ExitCode){throw ('Offline UI test failed: '+$process.ExitCode)}}finally{$process.Dispose()}
 }
 if($Target -ne 'Build'){
  & (Join-Path (Split-Path $cmake -Parent) 'ctest.exe') --test-dir $build -C Release --output-on-failure
  if($LASTEXITCODE){throw 'Protocol or session tests failed.'}
  OfflineApp @('--ui-self-test',('"'+(Join-Path $build 'ui-tests.json')+'"'))
  $ui=Get-Content -LiteralPath (Join-Path $build 'ui-tests.json') -Raw|ConvertFrom-Json
  if(!$ui.passed -or $ui.driverOpened -or $ui.inputsSent){throw 'Offline UI proof is invalid.'}
 }
 if($Render){OfflineApp @('--screenshot',('"'+(Join-Path $root 'docs\images\app.png')+'"'))}
 if($Target -eq 'Package'){
  if($AddressSanitizer){throw 'Package the ordinary Release build, not sanitizer binaries.'}
  $installer=$DriverInstaller
  if(!$installer){$installer=Join-Path $root 'obj\Dependencies\VdeviceSetup.exe'}
  $expected='A47DFC17536F350BDCB47CC786FB25EEEE74282CA6D282FD9719C775FFF6ED56'
  if(!(Test-Path -LiteralPath $installer)){
   New-Item -ItemType Directory -Force (Split-Path $installer -Parent)|Out-Null
   Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/TitanCSA/Vdevice/releases/download/v1.0.5.0-demo/VdeviceSetup.exe' -OutFile $installer
  }
  if(!(Test-Path -LiteralPath $installer) -or (Get-FileHash -LiteralPath $installer).Hash -ne $expected){throw 'The verified current Vdevice demo installer is missing. Use the Vdevice v1.0.5.0-demo release.'}
  $assets=Join-Path $root 'dist\v1.0.0';$stage=Join-Path $root ('obj\Package-'+[guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force $assets,(Join-Path $stage 'Driver'),(Join-Path $stage 'docs')|Out-Null
  Copy-Item -LiteralPath (Join-Path $build 'bin\VirtualMouseInput.exe') -Destination $stage
  Copy-Item -LiteralPath $installer -Destination (Join-Path $stage 'Driver\VdeviceSetup.exe')
  Copy-Item -LiteralPath (Join-Path $root 'README.md'),(Join-Path $root 'LICENSE') -Destination $stage
  Copy-Item -LiteralPath (Join-Path $root 'docs\BINARY-LICENSE.txt') -Destination (Join-Path $stage 'docs')
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $zip=Join-Path $assets 'Virtual-mouse-input-1.0.0-win64.zip'
  # .NET Framework CreateFromDirectory can emit Windows backslashes. Store
  # portable ZIP paths explicitly and keep the application at the archive root.
  $temporaryZip=$stage+'.zip'
  $stream=[IO.File]::Open($temporaryZip,[IO.FileMode]::CreateNew)
  try{
   $archive=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$true)
   try{foreach($file in Get-ChildItem -LiteralPath $stage -File -Recurse){
    $name=$file.FullName.Substring($stage.Length+1).Replace('\','/')
    $entry=$archive.CreateEntry($name,[IO.Compression.CompressionLevel]::Optimal)
    $entry.LastWriteTime=[DateTimeOffset]::new(2000,1,1,0,0,0,[TimeSpan]::Zero)
    $input=[IO.File]::OpenRead($file.FullName)
    try{$output=$entry.Open();try{$input.CopyTo($output)}finally{$output.Dispose()}}finally{$input.Dispose()}
   }}finally{$archive.Dispose()}
  }finally{$stream.Dispose()}
  $check=[IO.Compression.ZipFile]::OpenRead($temporaryZip)
  try{if($check.Entries.Count -ne 5 -or !$check.GetEntry('Driver/VdeviceSetup.exe') -or !$check.GetEntry('VirtualMouseInput.exe')){throw 'Unexpected application package layout.'}}finally{$check.Dispose()}
  if(Test-Path -LiteralPath $zip){Move-Item -LiteralPath $zip -Destination (Join-Path $root ('obj\Previous-Release-'+[guid]::NewGuid().ToString('N')+'.zip'))}
  Move-Item -LiteralPath $temporaryZip -Destination $zip
  Copy-Item -LiteralPath $installer -Destination (Join-Path $assets 'VdeviceSetup.exe') -Force
  $sum=@();foreach($name in 'Virtual-mouse-input-1.0.0-win64.zip','VdeviceSetup.exe'){$sum+=((Get-FileHash -LiteralPath (Join-Path $assets $name)).Hash.ToLowerInvariant()+'  '+$name)}
  [IO.File]::WriteAllText((Join-Path $assets 'SHA256SUMS.txt'),($sum -join "`n")+"`n",[Text.Encoding]::ASCII)
  Write-Host ('Release assets: '+$assets)
 }
 Write-Host 'Complete. No real virtual mouse was created and no input was sent by the tests.'
}finally{$env:PATH=$originalPath}
