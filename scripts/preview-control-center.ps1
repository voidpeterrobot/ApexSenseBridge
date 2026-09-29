param([string]$OutputDirectory = "artifacts/control-center-preview")
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
$projectRoot = Split-Path -Parent $PSScriptRoot
[void][Reflection.Assembly]::LoadFrom((Join-Path $projectRoot 'ApexSenseBridgeTray/bin/Release/ApexSenseBridgeTray.exe'))
$previewApplication = New-Object System.Windows.Application
[ApexSenseBridgeTray.Common.ThemeManager]::Initialize()
[ApexSenseBridgeTray.Common.LocalizationManager]::Initialize('en')
$previewSettings = New-Object ApexSenseBridgeTray.Models.TraySettings
$previewSettings.SetLaunchWhitelisted('C:\Games\Example\Example.exe', $true)
$previewCatalog = New-Object ApexSenseBridgeTray.Services.CloudGameListService
$previewSessions = New-Object ApexSenseBridgeTray.Services.EngineSessionManager
# Do not create App, a process monitor, a bridge session, or a game process.
$previewWindow = New-Object ApexSenseBridgeTray.MainWindow($previewCatalog, $previewSessions, $null, $null, $null, $previewSettings)
$previewContent = $previewWindow.Content
$previewWindow.FindName('GripControlsHost').Content.RaiseEvent((New-Object System.Windows.RoutedEventArgs([System.Windows.FrameworkElement]::LoadedEvent)))
$previewContent.Measure((New-Object System.Windows.Size(600, 740)))
$previewContent.Arrange((New-Object System.Windows.Rect(0, 0, 600, 740)))
$previewContent.UpdateLayout()
$bitmap = New-Object System.Windows.Media.Imaging.RenderTargetBitmap(600, 740, 96, 96, ([System.Windows.Media.PixelFormats]::Pbgra32))
$bitmap.Render($previewContent)
$encoder = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
$encoder.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
$previewOutput = Join-Path $projectRoot $OutputDirectory
[void][IO.Directory]::CreateDirectory($previewOutput)
$previewStream = [IO.File]::Create((Join-Path $previewOutput 'main.png'))
try { $encoder.Save($previewStream) } finally { $previewStream.Dispose() }
Write-Output (Join-Path $previewOutput 'main.png')
