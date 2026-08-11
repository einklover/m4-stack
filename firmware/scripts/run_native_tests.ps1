# Run PlatformIO native tests with MinGW in PATH.
# Usage: powershell -File scripts/run_native_tests.ps1

$mingwBin = Join-Path $env:USERPROFILE ".platformio\packages\toolchain-gccmingw32\bin"

if (-not (Test-Path $mingwBin)) {
    Write-Host "MinGW not found. Installing via PlatformIO..."
    python -m platformio pkg install -g -t toolchain-gccmingw32
}

$env:PATH = "$mingwBin;$env:PATH"
python -m platformio test -e native @args
