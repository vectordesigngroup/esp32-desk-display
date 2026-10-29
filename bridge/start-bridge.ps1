$root = $PSScriptRoot
Set-Location $root
$py = Join-Path $env:LocalAppData "Programs\Python\Python312\python.exe"
if (-not (Test-Path $py)) { $py = "python" }
$script = Join-Path $root "status_bridge.py"
while ($true) {
    & $py $script
    if ($LASTEXITCODE -eq 0) { break }
    Start-Sleep -Seconds 5
}
