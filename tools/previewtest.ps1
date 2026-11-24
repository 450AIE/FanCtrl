$ErrorActionPreference = 'Stop'
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root   = Split-Path -Parent $AppDir
$exe    = Join-Path $Root 'FanCtrl.exe'
$om = Get-WmiObject -Namespace root/WMI -Class LENOVO_OTHER_METHOD
function F1 { return $om.GetFeatureValue([uint32]0x04030001).value }
function F2 { return $om.GetFeatureValue([uint32]0x04030004).value }

function TestPreview([int]$rpm) {
    Get-Process FanCtrl -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
    Start-Sleep -Seconds 2
    $argList = @("/preview", "$rpm")
    Start-Process -FilePath $exe -ArgumentList $argList
    Start-Sleep -Seconds 8
    "preview $rpm -> fan $(F1) / $(F2)"
}

TestPreview 3000
TestPreview 4500
TestPreview 2000

Get-Process FanCtrl -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep -Seconds 2
[void]$om.SetFeatureValue([uint32]0x04030001, [uint32]0)
[void]$om.SetFeatureValue([uint32]0x04030004, [uint32]0)
"released. final: $(F1) / $(F2)"
