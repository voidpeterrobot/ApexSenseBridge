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
Write-Host 'SILENT GRIP MODE-ONLY LIFECYCLE: four mode writes, 28 queries, zero waveform writes.'
Write-Host 'Recovery after the official capture is unverified. Confirm power-cycle recovery and ordinary input/vibration before this run.'
Write-Host 'Direct USB, receiver unplugged; close official software and other known controller writers.'
& $toolPath compare-grip --baseline $baselinePath --confirmation $confirmationPath
if ($LASTEXITCODE -ne 0) { throw 'Fresh baselines do not match.' }
[IO.Directory]::CreateDirectory($resultPath) | Out-Null
$review = Join-Path $resultPath 'review.json'
$approval = Join-Path $resultPath 'approval.asb'
$run = Join-Path $resultPath 'run'
& $toolPath prepare-grip-lifecycle --baseline $baselinePath --output $review
if ($LASTEXITCODE -ne 0) { throw 'Lifecycle preparation failed.' }
& $toolPath rehearse-grip-lifecycle --baseline $baselinePath --manifest $review --output (Join-Path $resultPath 'rehearsal')
if ($LASTEXITCODE -ne 2) { throw 'Offline lifecycle rehearsal failed.' }
Write-Host 'Three prompts follow: setup/recovery, exact review/risks, exact review SHA256.'
& $toolPath approve-grip-lifecycle --manifest $review --output $approval
if ($LASTEXITCODE -ne 0) { throw 'Approval not recorded. No physical run started.' }
& $toolPath execute-grip-lifecycle --manifest $review --approval $approval --output $run
$runCode = $LASTEXITCODE
if ($runCode -eq 2) {
    Write-Host 'Diagnostic sequence complete; physical recovery UNVERIFIED (exit 2).' -ForegroundColor Yellow
} else {
    Write-Host 'Diagnostic FAILED. Disconnect/power off. No retry or speculative cleanup.' -ForegroundColor Red
}
try { & (Join-Path $PSScriptRoot 'Show-Apex6ExperimentDiagnostics.ps1') -Run $run }
catch { Write-Warning "Offline diagnostics unavailable: $($_.Exception.Message)" }
Write-Host 'Report ordinary input, normal vibration, and any unexpected movement. Matching RAM does not verify recovery.'
Write-Host 'Any unexpected behavior ends physical testing. Retain all evidence; no waveform run is authorized.'
exit $runCode
