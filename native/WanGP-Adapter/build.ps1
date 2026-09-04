param(
    [string]$NgxSdk = "C:\temp\NVIDIA-DLSS",
    [string]$Msvc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.35.32215",
    [string]$WindowsSdk = "C:\Program Files (x86)\Windows Kits\10",
    [string]$WindowsSdkVersion = "10.0.22000.0",
    [ValidateSet("all", "nr", "dlssg")][string]$Target = "all"
)

$ErrorActionPreference = "Stop"
$native = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $native)
$hostDir = Join-Path $root "bin\runtime\host"
$output = Join-Path $hostDir "nr-depth-worker.exe"
$object = Join-Path $hostDir "nr-depth-worker.obj"
$dlssgDir = Join-Path $root "bin\runtime\dlssg"
$dlssgOutput = Join-Path $dlssgDir "dlssg-worker.exe"
$dlssgObject = Join-Path $dlssgDir "dlssg-worker.obj"
New-Item -ItemType Directory -Force -Path $hostDir | Out-Null
New-Item -ItemType Directory -Force -Path $dlssgDir | Out-Null
$env:Path = "$Msvc\bin\Hostx64\x64;$WindowsSdk\bin\$WindowsSdkVersion\x64;$env:Path"
$env:INCLUDE = "$Msvc\include;$WindowsSdk\Include\$WindowsSdkVersion\ucrt;$WindowsSdk\Include\$WindowsSdkVersion\shared;$WindowsSdk\Include\$WindowsSdkVersion\um;$WindowsSdk\Include\$WindowsSdkVersion\winrt;$NgxSdk\include"
$env:LIB = "$Msvc\lib\x64;$WindowsSdk\Lib\$WindowsSdkVersion\ucrt\x64;$WindowsSdk\Lib\$WindowsSdkVersion\um\x64"

if ($Target -in @("all", "nr")) {
    & "$Msvc\bin\Hostx64\x64\cl.exe" /nologo /O2 /EHsc /W3 /MD /I"$NgxSdk\include" /Fo:"$object" "$native\nr_depth_worker.cpp" /Fe:"$output" /link "$NgxSdk\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib" version.lib winmm.lib kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    Remove-Item -LiteralPath $object
}

if ($Target -in @("all", "dlssg")) {
    & "$Msvc\bin\Hostx64\x64\cl.exe" /nologo /O2 /EHsc /W3 /MD /I"$NgxSdk\include" /Fo:"$dlssgObject" "$native\dlssg_worker.cpp" /Fe:"$dlssgOutput" /link "$NgxSdk\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib" d3d12.lib dxgi.lib version.lib winmm.lib kernel32.lib user32.lib advapi32.lib ole32.lib
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    Remove-Item -LiteralPath $dlssgObject
}
