@echo off
cd /d "%~dp0"
set PATH=C:\msys64\ucrt64\bin;%PATH%
gcc -O2 maak_icoon.c -o maak_icoon.exe -lgdi32 || exit /b 1
.\maak_icoon.exe || exit /b 1
windres gpu-venster.rc -O coff -o icoon.o || exit /b 1
gcc -O2 -Wall -municode -mwindows -static gpu-venster.c meter.c icoon.o -o GPUINT.exe -lgdiplus -ldwmapi -lgdi32 -luser32 -lshell32 -ladvapi32 -lpdh -lntdll -ldxgi -lpsapi -ldwmapi || exit /b 1
gcc -O2 -Wall -static meter.c meter_proef.c -o meter_proef.exe -lpdh -lntdll -ldxgi -lpsapi -ldwmapi || exit /b 1
del maak_icoon.exe icoon.o
