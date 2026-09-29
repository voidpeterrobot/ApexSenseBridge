param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Executable = (Join-Path $PSScriptRoot '..\build-win\Release\ApexSenseBridgeApex6NeutralExperiment.exe')
)
$ErrorActionPreference = 'Stop'
if ([Console]::IsInputRedirected) { throw 'Run in an interactive console; redirected consent is not allowed.' }
$toolPath = (Resolve-Path -LiteralPath $Executable).Path
$reviewPath = (Resolve-Path -LiteralPath $Manifest).Path
$resultPath = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $resultPath) { throw 'Output must be a new directory; evidence is never overwritten.' }
Write-Host 'Apex6Pro ONE LEFT-GRIP RESTORE test after a power cycle.'
Write-Host 'No streaming setup, waveform, combined exit or right restore. One baseline-derived mode command only.'
Write-Host 'The same left-restore command previously received error 1 after streaming; this isolates the preceding sequence.'
Write-Host 'Keep the controller observable. On any error disconnect/power off; do not retry.'
Write-Host "Review: $reviewPath"
Write-Host 'Three prompts only: setup/readiness, combined review/risks, then the exact review SHA256.'
[IO.Directory]::CreateDirectory($resultPath) | Out-Null
$approvalPath = Join-Path $resultPath 'approval.asb'
$runPath = Join-Path $resultPath 'run'
& $toolPath approve-grip-restore --manifest $reviewPath --output $approvalPath
if ($LASTEXITCODE -ne 0) { throw 'Approval not recorded. No physical test was started.' }
Write-Host 'Starting the single approved restore-only test. No automatic retry.'
& $toolPath execute-grip-restore --manifest $reviewPath --approval $approvalPath --output $runPath
if ($LASTEXITCODE -ne 0) {
    Write-Host 'STOP: test incomplete. Disconnect/power off now; do not retry.' -ForegroundColor Red
    Write-Host "Retain evidence: $runPath"
    try { & (Join-Path $PSScriptRoot 'Show-Apex6ExperimentDiagnostics.ps1') -Run $runPath }
    catch { Write-Warning "Offline diagnostics unavailable: $($_.Exception.Message). Retain the original evidence." }
    throw 'Physical recovery is not established. Report this result before continuing.'
}
Write-Host 'Restore-only protocol completed; this does not qualify streaming or prove motor recovery.'
Write-Host 'Independently check ordinary input and normal vibration and report your observations.'
Write-Host "Evidence: $runPath"
