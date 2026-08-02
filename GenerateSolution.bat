@echo off
REM Run from repo root
"thirdParty\sharpmake\bin\release\Sharpmake.Application.exe" ^
  "/sources('build/main.sharpmake.cs')" ^
  /verbose
