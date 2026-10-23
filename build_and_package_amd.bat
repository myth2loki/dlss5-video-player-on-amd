@echo off
rem Builds the AMD player from source and packages it under dist\ in one step.
rem See tools\build_and_package_amd.ps1 for what the step does and why it checks
rem that the build staged NeuralWorker.exe before packaging.
rem
rem Extra arguments are forwarded, e.g.:
rem   build_and_package_amd.bat -IncludeFfmpeg -WithoutNvidiaBackend
rem   build_and_package_amd.bat -SkipBuild -NoZip -Force
rem   build_and_package_amd.bat -Test -Configure
setlocal EnableExtensions
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build_and_package_amd.ps1 %*
exit /b %ERRORLEVEL%
