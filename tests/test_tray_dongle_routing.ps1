param([Parameter(Mandatory=$true)][string]$TrayAssembly)
$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom([IO.Path]::GetFullPath($TrayAssembly))
$settingsType = $assembly.GetType('ApexSenseBridgeTray.Models.TraySettings', $true)
$manager = $assembly.GetType('ApexSenseBridgeTray.Services.EngineSessionManager', $true)
$build = $manager.GetMethod('BuildArguments', [Reflection.BindingFlags]'NonPublic,Static')
if ($null -eq $build) { throw 'Bridge argument builder is missing.' }
foreach ($selectedApex6 in @($false, $true)) {
    foreach ($enabled in @($false, $true)) {
        $settings = [Activator]::CreateInstance($settingsType)
        $settings.Apex6DongleBeta = $enabled
        $arguments = $build.Invoke($null, @('standard', $settings, 0, $selectedApex6))
        if ($arguments.Contains('--apex6-dongle-beta') -ne ($enabled -and $selectedApex6)) {
            throw 'Dongle option routed to the wrong controller/setting combination.'
        }
        if ($arguments.Contains('--controller-model apex6-pro') -ne $selectedApex6) {
            throw 'Apex6 selection changed.'
        }
        if ($arguments.Contains('--haptic-threshold') -eq $selectedApex6) {
            throw 'Apex4/5 summary haptics routing changed.'
        }
    }
}
Write-Output 'Tray transport routing passed for USB, dongle and Apex4/5.'
