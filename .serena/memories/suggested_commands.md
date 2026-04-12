# Suggested commands
- List repo root: `Get-ChildItem -Name`
- Search text: `rg -n "pattern" Prototype`
- Build project: `msbuild Prototype\Prototype.vcxproj /p:Configuration=Debug /p:Platform=x64`
- Run executable: `.\Prototype\x64\Debug\Prototype.exe`
- Show git status: `git status --short`
- Show diff for files: `git diff -- Prototype\BatchedNumberRegistry.h Prototype\Prototype.cpp`
- Read files in PowerShell: `Get-Content Prototype\BatchedNumberRegistry.h`