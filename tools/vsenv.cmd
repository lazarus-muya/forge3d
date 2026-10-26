@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Runs a command inside the x64 Visual Studio developer environment.
rem Usage: tools\vsenv.cmd cmake --preset win-msvc
setlocal
set "F3D_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%F3D_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "F3D_VS=%%i"
if not defined F3D_VS (
    echo Visual Studio with C++ tools not found
    exit /b 1
)
rem VsDevCmd looks for vswhere on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
rem VsDevCmd points VCPKG_ROOT at Visual Studio's bundled vcpkg; keep the caller's.
set "F3D_VCPKG_ROOT=%VCPKG_ROOT%"
call "%F3D_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if defined F3D_VCPKG_ROOT set "VCPKG_ROOT=%F3D_VCPKG_ROOT%"
%*
