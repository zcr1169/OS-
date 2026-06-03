@echo off
chcp 65001 >/dev/null 2>&1
set "VCDIR=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.52.36328"
set "SDKDIR=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.28000.0"
set "INCLUDE=%VCDIR%\include;%SDKDIR%\Include\%SDKVER%\ucrt;%SDKDIR%\Include\%SDKVER%\um;%SDKDIR%\Include\%SDKVER%\shared"
set "LIB=%VCDIR%\lib\x64;%SDKDIR%\Lib\%SDKVER%\ucrt\x64;%SDKDIR%\Lib\%SDKVER%\um\x64"
set "PATH=%VCDIR%\bin\Hostx64\x64;%PATH%"
cl.exe /EHsc /std:c++17 /utf-8 /I 源码 /Fe:生成\os_simulator.exe 源码\*.cpp
pause
