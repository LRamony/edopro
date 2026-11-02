@echo off
set "VCPKG_ROOT=C:\vcpkg"

if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo vcpkg nao encontrado em "%VCPKG_ROOT%".
    exit /b 1
)

".\premake5.exe" vs2022 --no-direct3d --architecture=x86 --vcpkg-root="%VCPKG_ROOT%" --vcpkg-triplet=-windows-static
if errorlevel 1 exit /b %errorlevel%

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b %errorlevel%

"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" /noautoresponse "D:\Projetos\edopro\build\ygopro.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=Win32 /v:minimal
exit /b %errorlevel%
