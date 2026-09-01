# Build Release, deploy to C:\dev\AltTaberPlus, restart the app.
# The app runs elevated, so it can only be stopped/started through its scheduled task.
$ErrorActionPreference = 'Stop'

$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$build = "$PSScriptRoot\build"
$exe = "$build\Release\AltTaberPlus.exe"
$target = "C:\dev\AltTaberPlus"
$task = "AltTaber Startup"

& $cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

schtasks /end /tn $task | Out-Null
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Process AltTaberPlus -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
}
if (Get-Process AltTaberPlus -ErrorAction SilentlyContinue) { throw "AltTaberPlus still running, cannot replace it" }

Copy-Item $exe $target -Force
schtasks /run /tn $task | Out-Null
"deployed $((Get-Item "$target\AltTaberPlus.exe").LastWriteTime) and restarted"
