@echo off
rem Packages the AMD build: player + neural helper + FFX DLLs + the
rem amd-neural-runtime\LmxxfNrRuntime pack, then zips it under dist\.
rem
rem Extra arguments are forwarded, e.g.:
rem   package_amd_release.bat -IncludeFfmpeg -WithoutNvidiaBackend
rem   package_amd_release.bat -IncludeSubtitleEngine
rem   package_amd_release.bat -IncludeSpeechEngine
rem   package_amd_release.bat -NoZip -Force
setlocal EnableExtensions
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\package_amd_release.ps1 %*
exit /b %ERRORLEVEL%
