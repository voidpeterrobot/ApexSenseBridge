$stateFile=Join-Path $PSScriptRoot 'fake-state.json'
$state=[IO.File]::ReadAllText($stateFile) | ConvertFrom-Json
$global:LASTEXITCODE=0
if ($state.Malformed) { Write-Output 'unexpected driver output';return }
for($i=0;$i -lt $args.Count;$i++) {
    $command=$args[$i]
    if ($state.FailOn -eq $command) { $global:LASTEXITCODE=1;return }
    switch($command) {
        '--cloak-state' { if($state.Active){'--cloak-on'}else{'--cloak-off'} }
        '--inv-state' { if($state.Inverse){'--inv-on'}else{'--inv-off'} }
        '--app-list' { foreach($app in $state.Apps){'--app-reg "'+$app+'"'} }
        '--dev-list' { foreach($device in $state.Devices){'--dev-hide "'+$device+'"'} }
        '--cloak-on' { $state.Active=$true }
        '--cloak-off' { $state.Active=$false }
        '--app-reg' { $i++;if($state.Apps -notcontains $args[$i]){$state.Apps=@($state.Apps)+$args[$i]} }
        '--app-unreg' { $i++;$target=$args[$i];$state.Apps=@($state.Apps | Where-Object {$_ -ne $target}) }
        '--dev-hide' { $i++;if($state.Devices -notcontains $args[$i]){$state.Devices=@($state.Devices)+$args[$i]} }
        '--dev-unhide' { $i++;$target=$args[$i];$state.Devices=@($state.Devices | Where-Object {$_ -ne $target}) }
        default { throw "Unexpected fake command $command" }
    }
    [IO.File]::WriteAllText($stateFile,($state | ConvertTo-Json -Depth 6))
}
