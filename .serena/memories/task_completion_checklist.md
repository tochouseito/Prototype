# Task completion checklist
- Build the Visual C++ project with `msbuild Prototype\Prototype.vcxproj /p:Configuration=Debug /p:Platform=x64`.
- If behavior changed, run `.\Prototype\x64\Debug\Prototype.exe` and inspect the printed item/batch IDs and values.
- Review `git diff` to ensure only intended prototype files changed.
- There is currently no dedicated test or lint setup in the repo; verification is build + executable output.