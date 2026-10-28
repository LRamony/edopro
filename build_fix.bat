@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" /noautoresponse "D:\Projetos\edopro\build\ygopro.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=Win32 /v:minimal
