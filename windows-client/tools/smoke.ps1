# Windows loopback smoke test (spec §62) — no Mac needed:
# fake-host (stand-in for `mac-host serve --mode hp`) → windows-client on 127.0.0.1.
# Exit code 0 = PASS (handshake + >= 30 fps presented), non-zero = FAIL.
#
#   .\tools\smoke.ps1                 # 8 s, clean network
#   .\tools\smoke.ps1 -Loss 1         # simulate 1 % video packet loss
param(
    [string]$Build = "$PSScriptRoot\..\build\Release",
    [int]$Seconds = 8,
    [double]$Loss = 0
)
$ErrorActionPreference = 'Stop'

$hostLog = Join-Path ([IO.Path]::GetTempPath()) 'fake-host-smoke.log'
$hostArgs = @('--bind', '127.0.0.1', '--seconds', [string]($Seconds + 5))
if ($Loss -gt 0) { $hostArgs += @('--loss', [string]$Loss) }

$fakeHost = Start-Process -FilePath "$Build\fake-host.exe" -ArgumentList $hostArgs `
    -RedirectStandardOutput $hostLog -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2   # clip encode; the client re-handshakes anyway

& "$Build\windows-client.exe" --host 127.0.0.1 --seconds $Seconds --expect-video
$code = $LASTEXITCODE

$fakeHost.WaitForExit(15000) | Out-Null
Write-Host "fake-host log: $hostLog"
exit $code
