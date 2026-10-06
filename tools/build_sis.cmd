@echo off
setlocal
REM ---------------------------------------------------------------------------
REM build_sis.cmd - build and sign the on-device overclock application.
REM
REM   build_sis.cmd            build + sign -> group\SymbianOverclock_sha1.sis
REM   build_sis.cmd push [ip]  the above, then copy the SIS onto the phone over
REM                            WiFi into E:\Downloads, to be installed by hand.
REM
REM Three traps this script exists to close - each one already cost a day
REM somewhere in this tree:
REM   1. signsis WITHOUT -s returns 0 and produces an UNSIGNED file. The only
REM      reliable check is "signsis -o" showing an issuer.
REM   2. Symbian 9.2 cannot parse a SHA-256 certificate at all, and blames the
REM      file instead ("file is corrupted"). RSA + SHA-1 is mandatory.
REM   3. makesis misparses a .pkg that is UTF-8 or has LF endings - the .pkg
REM      must stay ASCII with CRLF.
REM
REM EPOCROOT carries no drive letter (Epoc32\tools\E32env.pm), so build on the SDK drive.
REM Perl must be < 5.22: E32Plat.pm uses defined(%hash).
REM
REM Comments here stay ASCII on purpose - cmd mangles UTF-8 in batch files.
REM ---------------------------------------------------------------------------

call "%~dp0env.cmd" || exit /b 1

set "SIGNDIR=%PROJ%\sign"
if not defined OPENSSL (echo [ocgui] OpenSSL not found; set OPENSSL to openssl.exe & exit /b 1)
pushd "%PROJ%\group"
call bldmake bldfiles || (popd & echo [ocgui] bldmake FAILED & exit /b 1)
call .\ABLD.BAT build gcce urel || (popd & echo [ocgui] build FAILED & exit /b 1)

if not exist "%SDKROOT%\Epoc32\release\gcce\urel\ocgui.exe" (
  popd & echo [ocgui] no binary produced & exit /b 1
)

if not exist "%SIGNDIR%" mkdir "%SIGNDIR%"
if not exist "%SIGNDIR%\sha1.key" (
  echo [ocgui] generating a self-signed RSA/SHA-1 certificate
  "%OPENSSL%" genrsa -out "%SIGNDIR%\sha1.key" 1024 || (popd & exit /b 1)
  "%OPENSSL%" req -new -x509 -sha1 -key "%SIGNDIR%\sha1.key" -out "%SIGNDIR%\sha1.cer" -days 3650 -subj "/CN=SymbianOverclock" || (popd & exit /b 1)
)

del /q SymbianOverclock.sis SymbianOverclock_sha1.sis 2>nul
call makesis ocgui.pkg SymbianOverclock.sis || (popd & echo [ocgui] makesis FAILED & exit /b 1)
call signsis -s SymbianOverclock.sis SymbianOverclock_sha1.sis "%SIGNDIR%\sha1.cer" "%SIGNDIR%\sha1.key"
if not exist SymbianOverclock_sha1.sis (popd & echo [ocgui] signsis produced nothing & exit /b 1)
call signsis -o SymbianOverclock_sha1.sis | findstr /i "Issued" >nul
if errorlevel 1 (popd & echo [ocgui] UNSIGNED: signsis -o shows no issuer & exit /b 1)
echo [ocgui] signed ok -^> %PROJ%\group\SymbianOverclock_sha1.sis
popd

if /i not "%~1"=="push" exit /b 0

set DIP=%~2
if "%DIP%"=="" set DIP=scan
powershell -NoProfile -ExecutionPolicy Bypass -File "%DAEMON%\tools\deploy_net.ps1" ^
  -Ip %DIP% ^
  -PutFile "%PROJ%\group\SymbianOverclock_sha1.sis" -Remote "E:\Downloads\SymbianOverclock_sha1.sis"
if errorlevel 1 (echo [ocgui] push FAILED & exit /b 1)
echo [ocgui] SIS is in E:\Downloads on the phone - install it from there.
exit /b 0
