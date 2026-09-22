@echo off
rem Builds psxe_embedded from the command line. Options go to build.ps1, e.g.:
rem   build.bat -Flash      build.bat -Clean      build.bat -Sound 0 -Flash
rem   powershell Get-Help .\build.ps1 -Detailed   lists them all
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
