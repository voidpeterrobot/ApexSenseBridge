$ErrorActionPreference='Stop'
. "$PSScriptRoot\..\scripts\Apex6HidHide.ps1"
function Assert($condition,[string]$reason){if(-not $condition){throw $reason}}
function Reject([scriptblock]$body){$failed=$false;try{& $body}catch{$failed=$true};Assert $failed 'Expected rejection.'}
$root=Join-Path ([IO.Path]::GetTempPath()) ('asb-hidhide-'+[Guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $root
$game=Join-Path $root ('asb-test-game-'+[Guid]::NewGuid().ToString('N')+'.exe')
[IO.File]::WriteAllText($game,'unused game-path fixture')
Assert (Assert-Apex6GameClosed $game).closed 'Closed game guard failed.'
Reject { Assert-Apex6GameClosed $game @($game) }
Reject { Assert-Apex6GameClosed (Join-Path $PSHOME 'powershell.exe') }
Reject { Assert-Apex6GameClosed (Join-Path $root 'missing.exe') }
$cli=Join-Path $root 'fake-cli.ps1'
Copy-Item -LiteralPath "$PSScriptRoot\fake_hidhide_cli.ps1" -Destination $cli
$stateFile=Join-Path $root 'fake-state.json'
function Reset-State { [IO.File]::WriteAllText($stateFile,(@{Active=$false;Inverse=$false;Apps=@('C:\Original App.exe');Devices=@();FailOn='';Malformed=$false}|ConvertTo-Json)) }
function Read-State { [IO.File]::ReadAllText($stateFile)|ConvertFrom-Json }
function Write-State($state){[IO.File]::WriteAllText($stateFile,($state|ConvertTo-Json -Depth 6))}
$exe='C:\Bridge Folder\ApexSenseBridgeApex6LiveBridge.exe'
$device='HID\VID_37D7&PID_2502&IG_01\EXACT_INSTANCE'
$xinputDevice='USB\VID_37D7&PID_2502&MI_00\EXACT_XUSB_INSTANCE'
Reset-State
$before=Get-Apex6HidHideState $cli
$plan=New-Apex6HidHidePlan $before $exe $device $xinputDevice
$active=Enable-Apex6HidHidePlan $cli $plan
Assert ($active.Active -and $active.Apps -contains $exe -and $active.Devices -contains $device -and $active.Devices -contains $xinputDevice -and $active.Devices.Count -eq 2) 'HID/XUSB isolation not applied.'
$restored=Restore-Apex6HidHidePlan $cli $plan
Assert (($restored|ConvertTo-Json -Compress) -eq ($before|ConvertTo-Json -Compress)) 'Original state not restored.'
$null=Restore-Apex6HidHidePlan $cli $plan
Reject { New-Apex6HidHidePlan $before $exe 'HID\VID_054C&PID_0CE6\VIRTUAL' $xinputDevice }
Reject { New-Apex6HidHidePlan $before $exe $device 'USB\VID_37D7&PID_2502\COMPOSITE_ROOT' }
foreach($field in @('Active','Inverse')){$state=Get-Apex6HidHideState $cli;$state.$field=$true;Reject {New-Apex6HidHidePlan $state $exe $device}}
$state=Get-Apex6HidHideState $cli;$state.Devices=@('OTHER');Reject {New-Apex6HidHidePlan $state $exe $device}
$state=Read-State;$state.Apps+= 'C:\Concurrent App.exe';Write-State $state
Reject { Enable-Apex6HidHidePlan $cli $plan }
Reset-State
$state=Read-State;$state.FailOn='--cloak-on';Write-State $state
Reject { Enable-Apex6HidHidePlan $cli $plan }
$state=Read-State;$state.FailOn='';Write-State $state
$restored=Restore-Apex6HidHidePlan $cli $plan
Assert (-not $restored.Active -and -not $restored.Devices.Count -and $restored.Apps -notcontains $exe) 'Partial activation leaked configuration.'
$null=Enable-Apex6HidHidePlan $cli $plan
$state=Read-State;$state.Apps+='C:\Concurrent App.exe';$state.Devices=@($state.Devices)+ 'OTHER';Write-State $state
$restored=Restore-Apex6HidHidePlan $cli $plan
Assert ($restored.Active -and $restored.Devices -contains 'OTHER' -and $restored.Apps -contains 'C:\Concurrent App.exe' -and $restored.Devices -notcontains $device) 'Rollback clobbered concurrent unrelated settings.'
Reset-State
$state=Read-State;$state.Malformed=$true;Write-State $state
Reject {Get-Apex6HidHideState $cli}
Reset-State
$null=Enable-Apex6HidHidePlan $cli $plan
$owner=Start-Process powershell.exe -WindowStyle Hidden -ArgumentList '-NoProfile -NonInteractive -Command "Start-Sleep -Seconds 30"' -PassThru
$statePath=Join-Path $root 'snapshot.json'
@{OwnerPid=$owner.Id;OwnerStartTicks=$owner.StartTime.ToUniversalTime().Ticks;Cli=$cli;Plan=$plan}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $statePath -Encoding UTF8
$watchScript=(Resolve-Path -LiteralPath "$PSScriptRoot\..\scripts\Watch-Apex6HidHide.ps1").Path
$watcher=Start-Process powershell.exe -WindowStyle Hidden -ArgumentList "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$watchScript`" -StatePath `"$statePath`"" -PassThru
try {
    $end=[DateTime]::UtcNow.AddSeconds(10)
    while(-not(Test-Path -LiteralPath "$root\watchdog-ready.json")){if([DateTime]::UtcNow -gt $end){throw 'Watchdog failed to become ready.'};Start-Sleep -Milliseconds 50}
    # Terminate only the dummy owner created above to simulate console failure.
    $owner.Kill();$owner.WaitForExit()
    Assert ($watcher.WaitForExit(15000)) 'Crash recovery watchdog did not exit.'
    Assert ($watcher.ExitCode -eq 0 -and (Test-Path -LiteralPath "$root\restored.json")) 'Crash recovery failed.'
    $restored=Get-Apex6HidHideState $cli
    Assert (($restored|ConvertTo-Json -Compress) -eq ($before|ConvertTo-Json -Compress)) 'Crash recovery did not restore original state.'
} finally {if(-not $owner.HasExited){$owner.Kill()};if(-not $watcher.HasExited){$watcher.Kill()}}

# Ported ancestry checks must never select the VIIPER bus or another controller.
$vendor='HID\VID_37D7&PID_2502&MI_02\VENDOR'
$proxyRoot='ROOT\GENITECH_VIRTUAL_GAMEPAD_DEVICE\0000'
$nodes=@(
    @{Id=$vendor;Container='APEX';Parent='USB\APEX';Service='hidusb'},
    @{Id=$device;Container='APEX';Parent=$xinputDevice;Service='xinputhid'},
    @{Id=$xinputDevice;Container='APEX';Parent='USB\APEX';Service='xusb22'},
    @{Id='USB\APEX';Container='APEX';Parent='USB\ROOT';Service='usbccgp'},
    @{Id='HID\VID_37D7&PID_2502&IG_02\SECOND_COLLECTION';Container='APEX';Parent=$xinputDevice;Service='xinputhid'},
    @{Id=$proxyRoot;Container='PROXY';Parent='';Service='hidvirtualdriver'},
    @{Id='USB\VID_045E&PID_028E\PROXY';Container='PROXY';Parent=$proxyRoot;Service='xusb22'},
    @{Id='HID\VID_045E&PID_028E\PROXY';Container='PROXY';Parent='USB\VID_045E&PID_028E\PROXY';Service='hidusb'},
    @{Id='ROOT\USBIP_VHCI\0000';Container='VIIPER';Parent='';Service='usbip_vhci'},
    @{Id='USB\VID_054C&PID_0CE6\VIIPER';Container='VIIPER';Parent='ROOT\USBIP_VHCI\0000';Service='usbccgp'},
    @{Id='HID\VID_054C&PID_0CE6\VIIPER';Container='VIIPER';Parent='USB\VID_054C&PID_0CE6\VIIPER';Service='hidusb'},
    @{Id='HID\VID_37D7&PID_2502&IG_01\OTHER';Container='OTHER';Parent='USB\OTHER';Service='xinputhid'}
)
$targets=@(Select-Apex6IsolationTargets $nodes 'APEX' $vendor $device $xinputDevice)
Assert ($targets.Count -eq 7 -and $targets -contains $vendor -and $targets -contains $proxyRoot -and $targets -notcontains 'USB\APEX' -and $targets -notcontains 'HID\VID_054C&PID_0CE6\VIIPER') 'Exact vendor/proxy ancestry selection failed.'
$nodes[5].Service='hidvirtualdriver_evil';Reject {Select-Apex6IsolationTargets $nodes 'APEX' $vendor $device $xinputDevice};$nodes[5].Service='hidvirtualdriver'
Reject {Select-Apex6IsolationTargets $nodes 'OTHER' $vendor $device $xinputDevice}
Reset-State
$strictBefore=Get-Apex6HidHideState $cli
$strictPlan=New-Apex6HidHidePlan $strictBefore $exe $device $xinputDevice $targets -Strict
$strictActive=Enable-Apex6HidHidePlan $cli $strictPlan
Assert ($strictActive.Apps.Count -eq 1 -and $strictActive.Apps[0] -eq $exe) 'Competing whitelist entry retained.'
Assert-Apex6IsolationState $strictActive $strictPlan
$tampered=Read-State;$tampered.Apps+='C:\Unrelated Writer.exe';Reject {Assert-Apex6IsolationState $tampered $strictPlan}
$strictRestored=Restore-Apex6HidHidePlan $cli $strictPlan
Assert (($strictRestored|ConvertTo-Json -Compress) -eq ($strictBefore|ConvertTo-Json -Compress)) 'Strict allowlist restoration failed.'
$adminBefore=[pscustomobject]@{Active=$false;Inverse=$false;Devices=@();Apps=@('C:\HidHide\HidHideCLI.exe','C:\Other Writer.exe')}
$adminPlan=New-Apex6HidHidePlan $adminBefore $exe $device $xinputDevice $targets -Strict -AdministrativeCli 'C:\HidHide\HidHideCLI.exe'
Assert ($adminPlan.AllowedApps.Count -eq 2 -and $adminPlan.RemovedApps.Count -eq 1 -and $adminPlan.RemovedApps[0] -eq 'C:\Other Writer.exe') 'CLI self-registration exception admitted unrelated apps.'
Reject {New-Apex6HidHidePlan $adminBefore $exe $device $xinputDevice $targets -Strict -AdministrativeCli 'C:\Other Writer.exe'}
Reset-State
$broken=Read-State;$broken.FailOn='--cloak-on';Write-State $broken
Reject {Enable-Apex6HidHidePlan $cli $strictPlan}
$broken=Read-State;$broken.FailOn='';Write-State $broken
$recovered=Restore-Apex6HidHidePlan $cli $strictPlan
Assert ($recovered.Apps -contains 'C:\Original App.exe' -and $recovered.Apps -notcontains $exe -and -not $recovered.Active) 'Strict partial activation did not restore removed allowlist.'
Write-Host 'HidHide parsing, exact target, rollback, conflict, partial failure and watchdog recovery tests passed.'
