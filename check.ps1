Get-ChildItem -Path D:\Projetos\edopro -Name *build* -File
if(Test-Path D:\Projetos\edopro\build_result.txt){ Get-Content D:\Projetos\edopro\build_result.txt | Select-Object -Last 20 }
