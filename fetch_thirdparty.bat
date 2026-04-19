@echo off
REM ============================================================
REM  Fetch vendored third-party sources into .\thirdparty\.
REM
REM  PxrMaskProjection bundles (at build time, not in git) these
REM  single-header libraries:
REM
REM    tinyexr  -- BSD 3-Clause -- https://github.com/syoyo/tinyexr
REM    miniz    -- MIT          -- https://github.com/richgel999/miniz
REM
REM  Run this once before build.bat.  Re-run to update pinned
REM  versions after bumping the refs below.
REM
REM  Requires: curl and tar (both ship with Windows 10+).
REM ============================================================

setlocal

cd /d "%~dp0"
if not exist thirdparty mkdir thirdparty

REM Pinned refs -- bump and re-run to update.
set "TINYEXR_REF=release"
set "MINIZ_VER=3.0.2"

set "TINYEXR_BASE=https://raw.githubusercontent.com/syoyo/tinyexr/%TINYEXR_REF%"
set "MINIZ_ZIP_URL=https://github.com/richgel999/miniz/releases/download/%MINIZ_VER%/miniz-%MINIZ_VER%.zip"

echo.
echo Fetching tinyexr @ %TINYEXR_REF%
for %%F in (tinyexr.h exr_reader.hh streamreader.hh) do (
    curl -fL --progress-bar -o "thirdparty\%%F" "%TINYEXR_BASE%/%%F" || goto :fail
)

REM miniz's git tag ships the split source; the amalgamated single-file
REM form lives only in the GitHub release zip.
REM
REM Use the native Windows bsdtar (System32\tar.exe, shipping since
REM Windows 10 1803) so this works even if a different "tar" is on
REM PATH (e.g. Git Bash's GNU tar, which can't extract zips).
echo.
echo Fetching miniz @ %MINIZ_VER%
set "TMP_ZIP=thirdparty\_miniz.zip"
set "WIN_TAR=%SystemRoot%\System32\tar.exe"
curl -fL --progress-bar -o "%TMP_ZIP%" "%MINIZ_ZIP_URL%" || goto :fail
"%WIN_TAR%" -xf "%TMP_ZIP%" -C thirdparty miniz.h miniz.c || goto :fail
del "%TMP_ZIP%"

echo.
echo [OK] thirdparty\ populated:
dir /b thirdparty
endlocal
exit /b 0

:fail
echo.
echo [ERROR] Download or extraction failed.  Check your network,
echo         the refs above, and that curl + tar are available.
if exist "%TMP_ZIP%" del "%TMP_ZIP%"
endlocal
exit /b 1
