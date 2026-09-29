param([Parameter(Mandatory=$true)][string]$StatePath)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\Apex6HidHide.ps1"
$state=[IO.File]::ReadAllText($StatePath) | ConvertFrom-Json
$root=Split-Path -Parent $StatePath
try {
    $owner=[Diagnostics.Process]::GetProcessById($state.OwnerPid)
    if ($owner.StartTime.ToUniversalTime().Ticks -ne $state.OwnerStartTicks) { throw 'Isolation owner identity changed.' }
    [IO.File]::WriteAllText("$root\watchdog-ready.json",(@{pid=$PID} | ConvertTo-Json))
    $leaseFailed=$false
    while (-not $owner.WaitForExit(500)) {
        if (Test-Path -LiteralPath "$root\restored.json") { exit 0 }
        if(Test-Path -LiteralPath "$root\recovery-requested"){break}
        if($state.Plan.Strict -and -not $leaseFailed -and (Test-Path -LiteralPath "$root\active.json")){
            try {
                Assert-Apex6IsolationState (Get-Apex6HidHideState $state.Cli) $state.Plan
                Assert-Apex6WritersStopped
                $roots=@(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.InstanceId -like 'ROOT\GENITECH_VIRTUAL_GAMEPAD_DEVICE\*' })
                foreach($proxy in $roots){if($state.Plan.Devices -notcontains $proxy.InstanceId){throw 'A new Flydigi virtual controller appeared during isolation.'}}
                & "$PSScriptRoot\Test-Apex6XInputIsolation.ps1" -Output "$root\watchdog-xinput.json" -RequireNoControllers
                $stamp=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
                [IO.File]::WriteAllText("$root\lease.tmp",$state.Token+"`n"+$stamp+"`n")
                Move-Item -LiteralPath "$root\lease.tmp" -Destination "$root\lease.txt" -Force
            } catch {
                $leaseFailed=$true
                [IO.File]::WriteAllText("$root\isolation-failure.txt",$_.Exception.ToString())
                # Keep hiding in place until the worker/owner stops. Do not
                # expose physical input while output might still be active.
            }
        }
    }
    if (Test-Path -LiteralPath "$root\restored.json") { exit 0 }
    $lock=[Threading.Mutex]::new($false,'Local\ApexSenseBridge-Apex6-HidHide-session')
    $locked=$false
    try {
        try { $locked=$lock.WaitOne(10000) } catch [Threading.AbandonedMutexException] { $locked=$true }
        if (-not $locked) { throw 'Another isolation owner is active; recovery deferred.' }
        $after=Restore-Apex6HidHidePlan $state.Cli $state.Plan
        [IO.File]::WriteAllText("$root\restored.json",(@{by='watchdog';after=$after;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 8))
    } finally { if($locked){$lock.ReleaseMutex()};$lock.Dispose() }
} catch {
    [IO.File]::WriteAllText("$root\watchdog-error.txt",$_.Exception.ToString())
    exit 1
}
