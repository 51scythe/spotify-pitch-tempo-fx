@echo off
setlocal
cd /d "%~dp0\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
if not exist build\obj mkdir build\obj
cl /nologo /O2 /MT /c /Ivendor\minhook\include /Fobuild\obj\ vendor\minhook\src\buffer.c vendor\minhook\src\hook.c vendor\minhook\src\trampoline.c vendor\minhook\src\hde\hde64.c
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /LD /external:W0 /external:Ivendor\minhook\include /external:Ivendor\signalsmith-stretch /external:Ivendor\signalsmith-linear\include src\hook.cpp /Fobuild\obj\native.obj /Febuild\spotify-pitch-tempo.dll build\obj\buffer.obj build\obj\hook.obj build\obj\trampoline.obj build\obj\hde64.obj ole32.lib uuid.lib /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /DUNICODE /D_UNICODE src\controller.cpp /Fobuild\obj\controller.obj /Febuild\SpotifyPitchTempo.exe user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ws2_32.lib /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /external:W0 /external:Ivendor\signalsmith-stretch /external:Ivendor\signalsmith-linear\include src\dsp_test.cpp /Fobuild\obj\dsp_test.obj /Febuild\dsp-test.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 /external:W0 /external:Ivendor\signalsmith-stretch /external:Ivendor\signalsmith-linear\include src\fx_test.cpp /Fobuild\obj\fx_test.obj /Febuild\fx-test.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /W4 src\launch_user.cpp /Fobuild\obj\launch_user.obj /Febuild\launch-user.exe user32.lib advapi32.lib /link /INCREMENTAL:NO
exit /b %errorlevel%
