@echo off
setlocal
REM ---------------------------------------------------------------------------
REM build.cmd - build ocprobe.exe.
REM
REM   build.cmd                        build only
REM   build.cmd run [ip] [args] [sec]  build, push, run, pull the log
REM
REM Example: build.cmd run 192.168.1.5 "power 7"
REM
REM The 4th argument is the daemon-side run timeout in seconds (default 180).
REM Raise it for anything that holds a clock layout around a child process:
REM   build.cmd run 192.168.1.2 "patcher clocks mult=86 mpu=4 sec=300 child=ps1 args:gui play nosnd frames3000" 420
REM The daemon is single-threaded and answers nothing while RUN is waiting, so
REM keep the run itself as short as the measurement allows.
REM Probing TestPowerAPI is one index per run on purpose - index 6 never
REM completes its request, and a hung request dies only with its process.
REM
REM No SIS needed: the daemon drops the EXE straight into E:\sys\bin and
REM refreshes the SHA-1 in C:\sys\hash - that digest is what makes the loader
REM accept the binary with its declared capabilities.
REM
REM EPOCROOT carries no drive letter (Epoc32\tools\E32env.pm), so build from D:.
REM Perl must be < 5.22: E32Plat.pm uses defined(%hash).
REM
REM Comments here stay ASCII on purpose - cmd mangles UTF-8 in batch files.
REM ---------------------------------------------------------------------------

set PROJ=%~dp0..
set SDKROOT=D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1
set TCROOT=D:\VsCodeProjects\SymbianProjects\toolchain
set DAEMON=D:\VsCodeProjects\SymbianProjects\SymbianDeployDaemon

set EPOCROOT=\VsCodeProjects\SymbianProjects\S60_3rd_FP1\
path=%TCROOT%\perl\perl\bin;%SDKROOT%\Epoc32\tools;%TCROOT%\cppshim\gcc\bin;%TCROOT%\CSL_Arm_Toolchain\bin;%SDKROOT%\Epoc32\gcc\bin;%PATH%
D:

pushd "%PROJ%\group"
call bldmake bldfiles || (popd & echo [ocprobe] bldmake FAILED & exit /b 1)
call .\ABLD.BAT build gcce urel || (popd & echo [ocprobe] build FAILED & exit /b 1)
popd

set EXE=%SDKROOT%\Epoc32\release\gcce\urel\ocprobe.exe
if not exist "%EXE%" (echo [ocprobe] no binary at %EXE% & exit /b 1)
echo [ocprobe] built -^> %EXE%

if /i not "%~1"=="run" exit /b 0

set DIP=%~2
if "%DIP%"=="" set DIP=scan
set ARGS=%~3
set RUNSEC=%~4
if "%RUNSEC%"=="" set RUNSEC=180
set PATCHER=%PROJ%\out\rp\file0
set CMDLINE=E:\sys\bin\ocprobe.exe
if not "%ARGS%"=="" set CMDLINE=E:\sys\bin\ocprobe.exe %ARGS%

echo %ARGS% | findstr /i /c:"patcher" >nul
if not errorlevel 1 (
  if not exist "%PATCHER%" (echo [ocprobe] missing RomPatcher+ file: %PATCHER% & exit /b 1)
  powershell -NoProfile -ExecutionPolicy Bypass -File "%DAEMON%\tools\deploy_net.ps1" ^
    -Ip %DIP% ^
    -PutFile "%PATCHER%" -Remote "E:\sys\bin\patcher.ldd" -HashRemote "C:\sys\hash\patcher.ldd"
  if errorlevel 1 (echo [ocprobe] RomPatcher+ deploy FAILED & exit /b 1)
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%DAEMON%\tools\deploy_net.ps1" ^
  -Ip %DIP% -Cycle ^
  -PutFile "%EXE%" -Remote "E:\sys\bin\ocprobe.exe" -HashRemote "C:\sys\hash\ocprobe.exe" ^
  -Run "%CMDLINE%" -RunTimeoutSec %RUNSEC% ^
  -GetFile "E:\ocprobe.log" -OutFile "%PROJ%\out\ocprobe.log"
if errorlevel 1 (echo [ocprobe] deploy/run FAILED & exit /b 1)
echo [ocprobe] log -^> %PROJ%\out\ocprobe.log
exit /b 0
