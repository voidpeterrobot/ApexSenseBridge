param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Executable = (Join-Path $PSScriptRoot '..\build-win\Release\ApexSenseBridgeApex6NeutralExperiment.exe')
)
$ErrorActionPreference = 'Stop'

# Operator-facing wrapper only: no automatic consent, hardware discovery,
# settings changes, retry, or cleanup traffic. The native supervisor owns I/O.
if ([Console]::IsInputRedirected) { throw 'Run in an interactive console; redirected consent is not allowed.' }
$toolPath = (Resolve-Path -LiteralPath $Executable).Path
$reviewPath = (Resolve-Path -LiteralPath $Manifest).Path
$resultPath = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $resultPath) { throw 'Output must be a new directory; previous evidence is never overwritten.' }

Write-Host 'Apex6Pro GRIP-ONLY NEUTRAL test. This is NOT a vibration pulse test.'
Write-Host 'It enters streaming mode, sends neutral samples, exits, and restores current grip settings.'
Write-Host 'Mode changes can still affect motors. Keep the controller observable and be ready to disconnect/power it off.'
Write-Host 'Any unexpected failure stops all traffic, including cleanup. Do not retry after a failure.'
Write-Host "Review: $reviewPath"
Write-Host 'The executable will display the exact review and ask for your confirmations and its SHA256.'

[IO.Directory]::CreateDirectory($resultPath) | Out-Null
$approvalPath = Join-Path $resultPath 'approval.asb'
$runPath = Join-Path $resultPath 'run'
& $toolPath approve-grip --manifest $reviewPath --output $approvalPath
if ($LASTEXITCODE -ne 0) { throw 'Approval was not recorded. No physical test was started.' }
Write-Host 'Starting the single approved neutral lifecycle now. No automatic retry.'
& $toolPath execute-grip --manifest $reviewPath --approval $approvalPath --output $runPath
if ($LASTEXITCODE -ne 0) {
    Write-Host 'STOP: test incomplete. Disconnect/power off the controller now; do not retry.' -ForegroundColor Red
    Write-Host "Retain evidence at: $runPath"
    throw 'Physical recovery is not established. Report the result before continuing.'
}
Write-Host 'Protocol completed. This does NOT prove normal motor recovery or haptic playback.'
Write-Host 'Independently check ordinary input and normal vibration, then report what you observed.'
Write-Host "Evidence: $runPath"
