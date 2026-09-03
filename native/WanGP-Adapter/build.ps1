param(
    [string]$NgxSdk = "C:\temp\NVIDIA-DLSS",
    [string]$Msvc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.35.32215",
    [string]$WindowsSdk = "C:\Program Files (x86)\Windows Kits\10",
    [string]$WindowsSdkVersion = "10.0.22000.0"
)

$ErrorActionPreference = "Stop"
$native = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $native)
$hostDir = Join-Path $root "bin\runtime\host"
$output = Join-Path $hostDir "nr-depth-worker.exe"
$object = Join-Path $hostDir "nr-depth-worker.obj"
New-Item -ItemType Directory -Force -Path $hostDir | Out-Null
$env:Path = "$Msvc\bin\Hostx64\x64;$WindowsSdk\bin\$WindowsSdkVersion\x64;$env:Path"
$env:INCLUDE = "$Msvc\include;$WindowsSdk\Include\$WindowsSdkVersion\ucrt;$WindowsSdk\Include\$WindowsSdkVersion\shared;$WindowsSdk\Include\$WindowsSdkVersion\um;$WindowsSdk\Include\$WindowsSdkVersion\winrt;$NgxSdk\include"
$env:LIB = "$Msvc\lib\x64;$WindowsSdk\Lib\$WindowsSdkVersion\ucrt\x64;$WindowsSdk\Lib\$WindowsSdkVersion\um\x64"

& "$Msvc\bin\Hostx64\x64\cl.exe" /nologo /O2 /EHsc /W3 /MD /I"$NgxSdk\include" /Fo:"$object" "$native\nr_depth_worker.cpp" /Fe:"$output" /link "$NgxSdk\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib" version.lib winmm.lib kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Remove-Item -LiteralPath $object
