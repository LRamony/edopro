@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1

set "VCPKG_ROOT=C:\vcpkg"

if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo vcpkg nao encontrado em "%VCPKG_ROOT%".
    goto :failed
)

".\premake5.exe" vs2022 --no-direct3d --architecture=x86 --sound=miniaudio --vcpkg-root="%VCPKG_ROOT%" --vcpkg-triplet=-windows-static
if errorlevel 1 goto :failed

findstr /c:"YGOPRO_USE_MINIAUDIO" "build\ygopro.vcxproj" >nul
if errorlevel 1 (
    echo O projeto gerado nao habilitou o backend miniaudio.
    goto :failed
)

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 goto :failed

"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" /noautoresponse "D:\Projetos\edopro\build\ygopro.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=Win32 /v:minimal
set "BUILD_RESULT=%ERRORLEVEL%"
popd
exit /b %BUILD_RESULT%

:failed
set "BUILD_RESULT=%ERRORLEVEL%"
if "%BUILD_RESULT%"=="0" set "BUILD_RESULT=1"
popd
exit /b %BUILD_RESULT%
