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
Write-Host 'Apex6Pro ONE LEFT-GRIP RESTORE + BOUNDED RAW LISTEN diagnostic.'
Write-Host 'After its first reply: no further writes; listen at most 500 ms / 32 additional reports.'
Write-Host 'No streaming, waveform, right restore, postflight or cleanup. A later ACK does NOT establish recovery.'
Write-Host 'Disconnect immediately on unexpected behavior. Power off/disconnect after capture; do not retry.'
Write-Host "Review: $reviewPath"
Write-Host 'Three approval prompts: setup/readiness, combined review/risks, exact review SHA256.'
[IO.Directory]::CreateDirectory($resultPath) | Out-Null
$approvalPath = Join-Path $resultPath 'approval.asb'
$runPath = Join-Path $resultPath 'run'
& $toolPath approve-grip-observe --manifest $reviewPath --output $approvalPath
if ($LASTEXITCODE -ne 0) { throw 'Approval not recorded. No physical test was started.' }
Write-Host 'Starting one approved diagnostic. Capture completion never means restoration success.'
& $toolPath execute-grip-observe --manifest $reviewPath --approval $approvalPath --output $runPath
Write-Host 'STOP: Power off/disconnect now. Do not retry; physical recovery is not established.' -ForegroundColor Yellow
try { & (Join-Path $PSScriptRoot 'Show-Apex6ExperimentDiagnostics.ps1') -Run $runPath }
catch { Write-Warning "Offline diagnostics unavailable: $($_.Exception.Message)" }
Write-Host "Retain evidence: $runPath"
Write-Host 'Report the capture result before continuing. An unresolved exit is expected even if the bounded capture completed.'
