@echo off
rem Builds wkVolumeControl.dll (32-bit, required by WA.exe).
rem Requires Visual Studio Build Tools; adjust the path below if needed.

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1

cd /d "%~dp0"
cl /nologo /O2 /W3 /MT /EHsc /LD wkVolumeControl.cpp /Fe:wkVolumeControl.dll ^
   /link user32.lib gdi32.lib ole32.lib
