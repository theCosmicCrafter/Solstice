param([Parameter(Mandatory=$true)][string]$Exe)
$env:PATH = 'C:\solstice-ci\llvm-mingw-20251118-ucrt-x86_64\bin;C:\solstice-ci\deps\bin;C:\solstice-ci\b\bin;' + $env:PATH
& "C:\solstice-ci\b\bin\$Exe" @args
exit $LASTEXITCODE
