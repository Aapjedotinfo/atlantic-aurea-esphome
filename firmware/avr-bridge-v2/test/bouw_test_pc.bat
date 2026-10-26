@echo off
rem Bouwt en draait de kerntests native op de PC met MSVC (cl.exe).
rem mingw-gcc is op deze pc niet aanwezig; de kern is gewone C99 en compileert ook met gcc.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set OUT=%TEMP%
if not "%1"=="" set OUT=%1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\\" /Fe"%OUT%\test_kern_pc.exe" "%~dp0test_kern.c" "%~dp0sim.c" "%~dp0..\proto.c" "%~dp0..\tabel.c" "%~dp0..\brug.c"
if errorlevel 1 exit /b 1
"%OUT%\test_kern_pc.exe"
exit /b %errorlevel%
