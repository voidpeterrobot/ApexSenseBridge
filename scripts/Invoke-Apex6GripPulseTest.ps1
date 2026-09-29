param(
    [Parameter(Mandatory=$true)][string]$Baseline,
    [Parameter(Mandatory=$true)][string]$Confirmation,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Executable = ''
)
$ErrorActionPreference = 'Stop'
if ([Console]::IsInputRedirected) { throw 'Run in an interactive console; redirected consent is not allowed.' }
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $PSScriptRoot '..\build-win\Release\ApexSenseBridgeApex6NeutralExperiment.exe'
}
$toolPath = (Resolve-Path -LiteralPath $Executable).Path
$baselinePath = (Resolve-Path -LiteralPath $Baseline).Path
$confirmationPath = (Resolve-Path -LiteralPath $Confirmation).Path
if ($baselinePath -eq $confirmationPath) { throw 'Provide two independent fresh baseline acquisitions.' }
foreach ($path in @($baselinePath, $confirmationPath)) {
    $age = [DateTime]::UtcNow - (Get-Item -LiteralPath $path).LastWriteTimeUtc
    if ($age.TotalMinutes -gt 5 -or $age.TotalSeconds -lt 0) { throw 'Baselines must be fresh (within five minutes). Reacquire after any configuration change.' }
}
$resultPath = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $resultPath) { throw 'Output must be a new directory.' }
Write-Host 'ONE LEFT GRIP PULSE: 64 ms, 125 Hz; gain 12 on original 1/16 base; software peak/limit 0.75, edge peak 0.375.'
Write-Host '11 waveform writes, four mode writes, 28 queries. Right grip enabled with neutral samples; triggers disabled.'
Write-Host 'Confirm recovery after the silent lifecycle and current power-cycle recovery, ordinary input and vibration.'
Write-Host 'Direct USB, receiver unplugged; close official software and other known controller writers.'
Write-Host 'Observe the controller and be ready to power off. Ctrl+C stops traffic without cleanup; host timing cannot guarantee physical stopping.'
& $toolPath compare-grip --baseline $baselinePath --confirmation $confirmationPath
if ($LASTEXITCODE -ne 0) { throw 'Fresh baselines do not match.' }
[IO.Directory]::CreateDirectory($resultPath) | Out-Null
$review = Join-Path $resultPath 'review.json'
$approval = Join-Path $resultPath 'approval.asb'
$run = Join-Path $resultPath 'run'
& $toolPath prepare-grip-pulse --baseline $baselinePath --output $review
if ($LASTEXITCODE -ne 0) { throw 'Pulse preparation failed.' }
& $toolPath rehearse-grip-pulse --baseline $baselinePath --manifest $review --output (Join-Path $resultPath 'rehearsal')
if ($LASTEXITCODE -ne 2) { throw 'Offline pulse rehearsal failed.' }
Write-Host 'Three prompts follow: setup/recovery, exact pulse/risks, exact review SHA256.'
& $toolPath approve-grip-pulse --manifest $review --output $approval
if ($LASTEXITCODE -ne 0) { throw 'Approval not recorded. No physical run started.' }
& $toolPath execute-grip-pulse --manifest $review --approval $approval --output $run
$runCode = $LASTEXITCODE
if ($runCode -eq 2) {
    Write-Host 'Pulse diagnostic complete (exit 2); operator qualification pending; physical recovery UNVERIFIED.' -ForegroundColor Yellow
} else {
    Write-Host 'Pulse FAILED. Disconnect/power off. No retry or speculative cleanup.' -ForegroundColor Red
}
try { & (Join-Path $PSScriptRoot 'Show-Apex6ExperimentDiagnostics.ps1') -Run $run }
catch { Write-Warning "Offline diagnostics unavailable: $($_.Exception.Message)" }
Write-Host 'Report whether the left pulse was felt, any right/trigger or unexpected movement, and ordinary input/vibration afterward.'
Write-Host 'A faint or unclear pulse is inconclusive: no automatic retry or gain increase. Any unexpected behavior ends physical testing.'
exit $runCode
