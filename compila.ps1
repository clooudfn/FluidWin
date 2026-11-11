# Compila FluidWin.exe da questi sorgenti.
# Serve MSYS2 (https://www.msys2.org) con il compilatore UCRT64:  pacman -S mingw-w64-ucrt-x86_64-gcc
# Uso:  powershell -ExecutionPolicy Bypass -File compila.ps1  [-msys C:\msys64]
param([string]$msys = 'C:\msys64')
$bin = Join-Path $msys 'ucrt64\bin'
if (-not (Test-Path "$bin\g++.exe")) { Write-Error "g++ non trovato in ${bin}: installa MSYS2 e il pacchetto mingw-w64-ucrt-x86_64-gcc"; exit 1 }
$env:Path = "$bin;$env:Path"
Set-Location $PSScriptRoot
& "$bin\windres.exe" icona.rc -O coff -o icona.o
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$bin\g++.exe" -O2 -std=gnu++20 -Wall -municode -mwindows fluidwin.cpp icona.o -o FluidWin.exe `
    -ld3d11 -ldxgi -ldcomp -ld3dcompiler -ld2d1 -ldwrite -ldwmapi -luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -lversion `
    -lpropsys -luuid -liphlpapi -lwindowscodecs -lruntimeobject -lpowrprof -lshlwapi -lshcore -lsetupapi -lwinmm -ldxva2 -lwinhttp -static
if ($LASTEXITCODE) { exit $LASTEXITCODE }
"FluidWin.exe pronto"
