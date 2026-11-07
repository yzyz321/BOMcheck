@echo off
rem Build the C/C++ version (C0-C9) with MinGW + Ninja, then verify.
rem Usage: build_c.bat            configure if needed, build, verify
rem        build_c.bat clean      wipe build\c first
rem NOTE: keep this file ASCII-only with CRLF endings -- cmd.exe misparses
rem       UTF-8 / LF-only batch files and silently drops half of each line.
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
rem forward slashes here: -DCMAKE_*_COMPILER values get copied verbatim into a
rem cmake script string, where a backslash starts an escape sequence ('\A' fails)
rem toolchain paths can be overridden:  set MINGW=D:/.../mingw64/bin
if not defined MINGW set "MINGW=D:/AICS/mingw64/bin"
set "BUILD=%ROOT%build\c"
rem tool scripts need openpyxl: prefer the interpreter that already has it,
rem the first python on PATH may be a bare one
rem override with:  set PY=<python.exe with openpyxl>
if not defined PY (
  set "PY=python"
  if exist "D:\miniconda3\python.exe" set "PY=D:\miniconda3\python.exe"
)

if /i "%~1"=="clean" (
  if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

echo [0/15] layout header from pyref/layout_spec.py
"%PY%" "%ROOT%tools\gen_layout_c.py"
if errorlevel 1 exit /b 1

if not exist "%BUILD%\build.ninja" (
  echo [1/15] configure
  cmake -S "%ROOT%." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_C_COMPILER="%MINGW%/gcc.exe" ^
        -DCMAKE_CXX_COMPILER="%MINGW%/g++.exe" ^
        -DCMAKE_RC_COMPILER="%MINGW%/windres.exe"
  if errorlevel 1 exit /b 1
)

echo [2/15] build
cmake --build "%BUILD%"
if errorlevel 1 exit /b 1

echo [3/15] verify exe
set "EXE="
for %%f in ("%BUILD%\bin\BOMCheck_V*.exe") do set "EXE=%%~f"
if not defined EXE (
  echo no exe produced
  exit /b 1
)
"%PY%" "%ROOT%tools\check_exe.py" "%EXE%"
if errorlevel 1 exit /b 1

echo [4/15] verify layout table
"%PY%" "%ROOT%tools\check_layout.py" "%EXE%"
if errorlevel 1 exit /b 1

echo [5/15] verify xlsx reader vs python oracle
"%PY%" "%ROOT%tools\check_xlsx.py" --exe "%EXE%"
if errorlevel 1 exit /b 1

echo [6/15] verify pn/ref/key functions vs python oracle
"%PY%" "%ROOT%tools\check_pn.py"
if errorlevel 1 exit /b 1

echo [7/15] verify header/column recognition vs python oracle
"%PY%" "%ROOT%tools\check_sheet.py"
if errorlevel 1 exit /b 1

echo [8/15] verify pairing / 12-column diff vs python oracle
"%PY%" "%ROOT%tools\check_pair.py"
if errorlevel 1 exit /b 1

echo [9/15] verify xlsx writer vs python oracle
"%PY%" "%ROOT%tools\check_write.py"
if errorlevel 1 exit /b 1

echo [10/15] verify mark / annotate vs python oracle
"%PY%" "%ROOT%tools\check_mark.py"
if errorlevel 1 exit /b 1

echo [11/15] verify malformed / extreme inputs do not crash
"%PY%" "%ROOT%tools\check_fuzz.py" --exe "%EXE%"
if errorlevel 1 exit /b 1

echo [12/15] verify ui wiring (table / write / tabs / click)
"%PY%" "%ROOT%tools\check_ui.py" --exe "%EXE%"
if errorlevel 1 exit /b 1

echo [13/15] pack + clean-dir check
"%PY%" "%ROOT%tools\pack.py"
if errorlevel 1 exit /b 1
"%PY%" "%ROOT%tools\check_pack.py"
if errorlevel 1 exit /b 1

echo [14/15] ctest
pushd "%BUILD%"
ctest --output-on-failure
set "CT=%errorlevel%"
popd
if not "%CT%"=="0" exit /b 1

echo.
echo OK  %EXE%
