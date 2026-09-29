param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('asb-live-cli-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
& $Executable --help
if ($LASTEXITCODE -ne 0) { throw 'Live help failed.' }
& $Executable rehearse --output "$root\rehearsal"
if ($LASTEXITCODE -ne 0) { throw '60-second offline session failed.' }
$result = Get-Content -LiteralPath "$root\rehearsal\result.json" -Raw | ConvertFrom-Json
if (-not $result.complete -or -not $result.postflight_matches -or $result.restoration_verified -or $result.streamed_packets -ne 7499 -or $result.waveform_writes -ne 7502 -or $result.queries -ne 28 -or $result.mode_writes -ne 4) { throw 'Live rehearsal counts/qualification flags incorrect.' }
& $Executable rehearse --seconds 0 --output "$root\continuous"
if ($LASTEXITCODE -ne 0) { throw 'Continuous offline session failed.' }
$continuous = Get-Content -LiteralPath "$root\continuous\result.json" -Raw | ConvertFrom-Json
if (-not $continuous.complete -or -not $continuous.postflight_matches -or $continuous.stop_reason -ne 'operator_q' -or $continuous.streamed_packets -ne 37499 -or $continuous.waveform_writes -ne 37502 -or $continuous.queries -ne 28 -or $continuous.mode_writes -ne 4) { throw 'Continuous session stopped early or skipped orderly shutdown.' }
$manifest = Get-Content -LiteralPath "$root\rehearsal\manifest.json" -Raw | ConvertFrom-Json
foreach ($file in $manifest.files) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead((Join-Path "$root\rehearsal" $file.name))
    try { $actual = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
    if ($actual -ne $file.sha256) { throw 'Evidence hash mismatch.' }
}
& $Executable rehearse --output "$root\rehearsal"
if ($LASTEXITCODE -ne 1) { throw 'Existing evidence was overwritten.' }
& $Executable _worker-execute --output "$root\unsupervised"
if ($LASTEXITCODE -ne 1) { throw 'Unsupervised worker accepted.' }
& $Executable execute --output "$root\bad" --gain 12
if ($LASTEXITCODE -ne 1) { throw 'Unbound runtime gain accepted.' }
[IO.File]::WriteAllText("$root\old-token.asb", "ASB_APEX6_GRIP_PULSE_APPROVAL_V1`ngrip-left-pulse-v4`n")
& $Executable rehearse --manifest "$root\old-token.asb" --output "$root\wrong-scope"
if ($LASTEXITCODE -ne 1) { throw 'Old scope accepted as live review.' }
$process = Start-Process -FilePath $Executable -ArgumentList @('approve','--manifest',"`"$root\old-token.asb`"",'--rehearsal',"`"$root\rehearsal\rehearsal.asb`"",'--output',"`"$root\approval.asb`"") -RedirectStandardInput "$root\old-token.asb" -RedirectStandardOutput "$root\approval-stdout.txt" -RedirectStandardError "$root\approval-stderr.txt" -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 1 -or (Test-Path -LiteralPath "$root\approval.asb")) { throw 'Redirected approval accepted.' }
Write-Host 'Live CLI simulation, evidence, scope and supervision checks passed.'
