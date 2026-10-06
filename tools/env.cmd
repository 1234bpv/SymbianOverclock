@echo off
REM Called inside the build script's SETLOCAL. No global PATH changes.
for %%I in ("%~dp0..") do set "PROJ=%%~fI"
for %%I in ("%PROJ%\..") do set "SYMBIANROOT=%%~fI"
set "SDKROOT=%SYMBIANROOT%\S60_3rd_FP1"
set "TCROOT=%SYMBIANROOT%\toolchain"
set "DAEMON=%SYMBIANROOT%\SymbianDeployDaemon"
for %%I in ("%SDKROOT%") do set "EPOCROOT=%%~pnxI\"
for %%I in ("%TCROOT%\perl\perl\bin\perl.exe" "%TCROOT%\CSL_Arm_Toolchain\bin\arm-none-symbianelf-g++.exe" "%SDKROOT%\Epoc32\tools\bldmake.bat") do (
  if not exist "%%~I" (
    echo [build] Missing tool: %%~I
    exit /b 1
  )
)
set "PATH=%TCROOT%\perl\perl\bin;%SDKROOT%\Epoc32\tools;%TCROOT%\CSL_Arm_Toolchain\bin;%SDKROOT%\Epoc32\gcc\bin;%PATH%"
REM Portable Perl 5.20 does not understand the host C.UTF-8 locale.
set "LC_ALL=C"
REM Prefer an explicit override, then OpenSSL beside the installed Git.
if defined OPENSSL exit /b 0
for /f "delims=" %%I in ('where git.exe 2^>nul') do if not defined OPENSSL if exist "%%~dpI..\usr\bin\openssl.exe" set "OPENSSL=%%~dpI..\usr\bin\openssl.exe"
if not defined OPENSSL for /f "delims=" %%I in ('where openssl.exe 2^>nul') do if not defined OPENSSL set "OPENSSL=%%I"
exit /b 0
