@echo off
rem Called by build.sh. %~dp0 = agent dir (UNC path when run from WSL).
setlocal
set "A=%~dp0"
set "TMP=%A%build\tmp"
set "TEMP=%A%build\tmp"
set VSCMD_SKIP_SENDTELEMETRY=1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /EHsc /std:c++20 /MT /utf-8 /W4 /DUNICODE /D_UNICODE "%A%src\*.cpp" /Fo"%A%build\\" /Fe"%A%bin\btop-agent.exe" ^
	/link pdh.lib iphlpapi.lib ntdll.lib powrprof.lib advapi32.lib
