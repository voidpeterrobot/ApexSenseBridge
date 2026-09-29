# Session-only HidHide configuration. No driver installation, profile writes,
# service changes, game launch, or broad controller selection.
function Assert-Apex6GameClosed {
    param([string]$GameExecutable, [string[]]$AllowedApps=@())
    $game=(Resolve-Path -LiteralPath $GameExecutable -ErrorAction Stop).Path
    if ([IO.Path]::GetExtension($game) -ne '.exe') { throw 'Expected the game executable.' }
    if ($AllowedApps -contains $game) { throw 'The game is whitelisted in HidHide and would bypass physical input isolation.' }
    $name=[IO.Path]::GetFileNameWithoutExtension($game)
    # Refuse even when process-path access is denied; never kill a running game.
    $running=@(Get-Process -Name $name -ErrorAction SilentlyContinue)
    if ($running.Count) { throw "Close $name completely before starting the isolated bridge; launch it only after the virtual DualSense is ready." }
    [pscustomobject]@{ executable=$game; closed=$true; utc=[DateTime]::UtcNow.ToString('o') }
}
function Invoke-Apex6HidHideCommand {
    param([string]$Cli, [string[]]$Arguments)
    $lines = @(& $Cli @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "HidHide command failed ($LASTEXITCODE): $($Arguments -join ' ')" }
    return $lines
}
function Get-Apex6HidHideState {
    param([string]$Cli)
    $state = [ordered]@{ Active=$null; Inverse=$null; Apps=@(); Devices=@() }
    foreach ($line in (Invoke-Apex6HidHideCommand $Cli @('--cloak-state','--inv-state','--app-list','--dev-list'))) {
        $line = ([string]$line).Trim()
        if (-not $line) { continue }
        switch -Regex ($line) {
            '^--cloak-(on|off)$' { if ($null -ne $state.Active) { throw 'Duplicate HidHide cloak state.' }; $state.Active=($Matches[1] -eq 'on'); break }
            '^--inv-(on|off)$' { if ($null -ne $state.Inverse) { throw 'Duplicate HidHide inverse state.' }; $state.Inverse=($Matches[1] -eq 'on'); break }
            '^--app-reg "([^"\r\n]+)"$' { $state.Apps += $Matches[1]; break }
            '^--dev-hide "([^"\r\n]+)"$' { $state.Devices += $Matches[1]; break }
            default { throw "Unrecognized HidHide state: $line" }
        }
    }
    if ($null -eq $state.Active -or $null -eq $state.Inverse) { throw 'Incomplete HidHide state.' }
    return [pscustomobject]$state
}
function New-Apex6HidHidePlan {
    param($Before, [string]$Executable, [string]$InputDevice, [string]$XInputDevice, [string[]]$VerifiedTargets=@(), [switch]$Strict, [string]$AdministrativeCli='')
    if ($Before.Active -or $Before.Inverse -or @($Before.Devices).Count) {
        throw 'Existing HidHide cloaking, inverse mode, or hidden-device configuration requires separate review; not overwriting it.'
    }
    if ($InputDevice -notmatch '^HID\\VID_37D7&PID_2502&IG_[0-9A-F]{2}\\[^"\r\n]+$') { throw 'Expected an exact Apex6 XInput gamepad instance.' }
    if ($XInputDevice -notmatch '^USB\\VID_37D7&PID_2502&MI_00\\[^"\r\n]+$') { throw 'Expected the exact Apex6 XUSB function instance.' }
    if (-not [IO.Path]::IsPathRooted($Executable) -or [IO.Path]::GetFileName($Executable) -ne 'ApexSenseBridgeApex6LiveBridge.exe') { throw 'Expected the exact live bridge executable.' }
    $targets=@($InputDevice,$XInputDevice)+@($VerifiedTargets) | Sort-Object -Unique
    $allowedApps=@($Executable)
    if($AdministrativeCli){
        if(-not [IO.Path]::IsPathRooted($AdministrativeCli) -or [IO.Path]::GetFileName($AdministrativeCli) -ne 'HidHideCLI.exe' -or $Before.Apps -notcontains $AdministrativeCli){throw 'Expected the exact self-registered HidHide CLI.'}
        $allowedApps+=$AdministrativeCli
    }
    [pscustomobject]@{ Before=$Before; Executable=$Executable; InputDevice=$InputDevice; XInputDevice=$XInputDevice; Devices=@($targets); AddedApp=($Before.Apps -notcontains $Executable); Strict=[bool]$Strict; AllowedApps=$allowedApps; RemovedApps=@($Before.Apps | Where-Object { $Strict -and $allowedApps -notcontains $_ }) }
}
function Enable-Apex6HidHidePlan {
    param([string]$Cli, $Plan)
    # Recheck immediately before mutation; snapshots must not replace newer state.
    $current=Get-Apex6HidHideState $Cli
    if (($current | ConvertTo-Json -Compress) -ne ($Plan.Before | ConvertTo-Json -Compress)) { throw 'HidHide changed after snapshot.' }
    foreach ($app in $Plan.RemovedApps) { $null=Invoke-Apex6HidHideCommand $Cli @('--app-unreg',$app) }
    if ($Plan.AddedApp) { $null=Invoke-Apex6HidHideCommand $Cli @('--app-reg',$Plan.Executable) }
    foreach ($target in $Plan.Devices) { $null=Invoke-Apex6HidHideCommand $Cli @('--dev-hide',$target) }
    $null=Invoke-Apex6HidHideCommand $Cli @('--cloak-on')
    $after=Get-Apex6HidHideState $Cli
    if (-not $after.Active -or $after.Inverse -or @($after.Devices).Count -ne @($Plan.Devices).Count -or $after.Apps -notcontains $Plan.Executable) { throw 'HidHide isolation readback failed.' }
    foreach ($target in $Plan.Devices) { if ($after.Devices -notcontains $target) { throw 'HidHide target missing from readback.' } }
    if ($Plan.Strict) { Assert-Apex6IsolationState $after $Plan }
    else { foreach ($app in $Plan.Before.Apps) { if ($after.Apps -notcontains $app) { throw 'HidHide removed an existing application.' } } }
    return $after
}
function Restore-Apex6HidHidePlan {
    param([string]$Cli, $Plan)
    $current=Get-Apex6HidHideState $Cli
    # Remove only this session's additions. Preserve concurrent unrelated edits.
    foreach ($target in $Plan.Devices) { if ($current.Devices -contains $target) { $null=Invoke-Apex6HidHideCommand $Cli @('--dev-unhide',$target) } }
    $current=Get-Apex6HidHideState $Cli
    if (-not @($current.Devices).Count -and -not $Plan.Before.Active -and -not $current.Inverse) { $null=Invoke-Apex6HidHideCommand $Cli @('--cloak-off') }
    if ($Plan.AddedApp -and $current.Apps -contains $Plan.Executable) { $null=Invoke-Apex6HidHideCommand $Cli @('--app-unreg',$Plan.Executable) }
    foreach ($app in $Plan.RemovedApps) { if ($current.Apps -notcontains $app) { $null=Invoke-Apex6HidHideCommand $Cli @('--app-reg',$app) } }
    $after=Get-Apex6HidHideState $Cli
    foreach ($target in $Plan.Devices) { if ($after.Devices -contains $target) { throw 'HidHide device addition was not restored.' } }
    if ($Plan.AddedApp -and $after.Apps -contains $Plan.Executable) { throw 'HidHide application addition was not restored.' }
    if (-not @($after.Devices).Count -and -not $after.Inverse -and $after.Active -ne $Plan.Before.Active) { throw 'HidHide cloak state was not restored.' }
    return $after
}

function Assert-Apex6IsolationState {
    param($State,$Plan)
    if (-not $State.Active -or $State.Inverse -or @($State.Apps).Count -ne @($Plan.AllowedApps).Count) { throw ('Strict HidHide allowlist/cloak changed: '+($State|ConvertTo-Json -Compress)) }
    foreach($app in $Plan.AllowedApps){if($State.Apps -notcontains $app){throw 'Strict HidHide allowed application missing.'}}
    if (@($State.Devices).Count -ne @($Plan.Devices).Count) { throw 'Strict HidHide target set changed.' }
    foreach ($target in $Plan.Devices) { if ($State.Devices -notcontains $target) { throw 'Strict HidHide target missing.' } }
}

# Same ancestry rule as WindowsPhysicalControllerIsolation: match the complete
# GeniTech root and driver service, never Sony/Microsoft VID/PID alone.
function Select-Apex6IsolationTargets {
    param([object[]]$Nodes,[string]$Container,[string]$Vendor,[string]$InputDevice,[string]$XInputDevice)
    $byId=@{};foreach($node in $Nodes){$byId[$node.Id]=$node}
    foreach($id in @($Vendor,$InputDevice,$XInputDevice)){
        if(-not $byId.ContainsKey($id) -or $byId[$id].Container -ne $Container){throw "Isolation interface/container changed: $id (expected $Container)."}
    }
    if($Vendor -notmatch '^HID\\VID_37D7&PID_2502&MI_02\\[^"\r\n]+$' -or $byId[$XInputDevice].Service -notmatch '^xusb(21|22)$'){throw 'Unexpected vendor/XUSB identity.'}
    $targets=New-Object 'System.Collections.Generic.List[string]'
    foreach($node in $Nodes){
        if($node.Container -eq $Container -and ($node.Id -match '^HID\\VID_37D7&PID_2502&IG_[0-9A-F]{2}\\' -or $node.Id -eq $Vendor -or $node.Id -eq $XInputDevice)){$targets.Add($node.Id)}
        if($node.Id -like 'ROOT\GENITECH_VIRTUAL_GAMEPAD_DEVICE\*'){
            if($node.Service -ne 'hidvirtualdriver'){throw 'Unexpected GeniTech proxy driver; refusing ambiguous isolation.'}
            $targets.Add($node.Id)
        }
    }
    foreach($node in $Nodes){
        if($node.Id -notmatch '^(HID|USB)\\'){continue}
        $ancestor=$node; $seen=@{}
        for($depth=0;$depth -lt 32;$depth++){
            if($seen.ContainsKey($ancestor.Id)){throw 'Cycle in isolation device ancestry.'};$seen[$ancestor.Id]=$true
            if($ancestor.Id -like 'ROOT\GENITECH_VIRTUAL_GAMEPAD_DEVICE\*'){
                if($ancestor.Service -ne 'hidvirtualdriver'){throw 'Unexpected proxy service.'}
                $targets.Add($node.Id);break
            }
            if(-not $ancestor.Parent -or -not $byId.ContainsKey($ancestor.Parent)){break}
            $ancestor=$byId[$ancestor.Parent]
        }
    }
    @($targets | Sort-Object -Unique)
}

function Get-Apex6IsolationTargets {
    param([string]$Vendor,[string]$InputDevice,[string]$XInputDevice)
    $devices=@(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.InstanceId -match '^(HID|USB|ROOT)\\' })
    $properties=@{}
    # Request the three required keys explicitly. The Windows PowerShell PnP
    # provider can omit devices when enumerating every property in one batch.
    foreach($property in (Get-PnpDeviceProperty -InstanceId $devices.InstanceId -KeyName DEVPKEY_Device_ContainerId,DEVPKEY_Device_Parent,DEVPKEY_Device_Service -ErrorAction Stop)){
        if(-not $properties.ContainsKey($property.DeviceID)){$properties[$property.DeviceID]=@{}}
        $properties[$property.DeviceID][$property.KeyName]=$property.Data
    }
    $nodes=@(foreach($device in $devices){
        # Ancestry must include buses as well as their gamepad functions.
        $values=$properties[$device.InstanceId]
        # Some unrelated ROOT nodes publish no properties. Required physical
        # container and GeniTech service checks below still fail if missing.
        if($null -eq $values){$values=@{}}
        [pscustomobject]@{Id=$device.InstanceId;Container=[string]$values['DEVPKEY_Device_ContainerId'];Parent=[string]$values['DEVPKEY_Device_Parent'];Service=[string]$values['DEVPKEY_Device_Service']}
    })
    $selected=@($nodes|Where-Object Id -eq $Vendor)
    if($selected.Count -ne 1 -or -not $selected[0].Container){throw 'Exact vendor container unavailable.'}
    Select-Apex6IsolationTargets $nodes $selected[0].Container $Vendor $InputDevice $XInputDevice
}

function Assert-Apex6WritersStopped {
    $service=Get-Service -Name 'Flydigi Space Station Service' -ErrorAction SilentlyContinue
    if($service -and $service.Status -ne 'Stopped'){throw 'Flydigi Space Station Service must remain stopped.'}
    $writers=@(Get-Process -Name 'SpaceStation*','X-Haptic*','XHaptic*','DSX','DS4Windows','reWASD*','ApexSenseBridge' -ErrorAction SilentlyContinue)
    if($writers.Count){throw ('Close competing controller software: '+(($writers.ProcessName|Sort-Object -Unique)-join ', '))}
}
