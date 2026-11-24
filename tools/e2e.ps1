param([int]$LoadSecs = 30, [switch]$Gpu)

$ErrorActionPreference = 'Stop'
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root   = Split-Path -Parent $AppDir
$exe    = Join-Path $Root 'FanCtrl.exe'
$load   = Join-Path $Root 'tools\loadgen.exe'
# LibreHardwareMonitorLib.dll 放在仓库同级目录，仅用于读 MSR 做对照
$lhmDll = Join-Path (Split-Path -Parent $Root) 'lhm\bin\LibreHardwareMonitorLib.dll'

Get-Process FanCtrl -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep -Seconds 2

Start-Process -FilePath $exe
Start-Sleep -Seconds 8

$asm = [System.Reflection.Assembly]::LoadFrom($lhmDll)
$t = $asm.GetType('LibreHardwareMonitor.PawnIo.IntelMsr')
$m = [Activator]::CreateInstance($t)
$rd = $t.GetMethod('ReadMsr', [type[]]@([uint32], [uint64].MakeByRefType()))
function Msr([uint32]$i) {
    $b = @($i, [uint64]0)
    if (-not $rd.Invoke($m, $b)) { return $null }
    return [uint64]$b[1]
}
$unit = Msr 0x606
$eu = 1.0 / [math]::Pow(2, [int](($unit -shr 8) -band 0x1F))
$tj = [int](((Msr 0x1A2) -shr 16) -band 0xFF)

function Watt {
    $e = Msr 0x611
    if ($null -eq $e) { return $null }
    return [uint64]($e -band 0xFFFFFFFF)
}

$gpuProc = $null
if ($Gpu) {
    $gpuExe = Join-Path $Root 'tools\gpuload.exe'
    $gpuProc = Start-Process -FilePath $gpuExe -ArgumentList @("$($LoadSecs + 20)", "256", "2048", "4096") -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 1
}
$p = Start-Process -FilePath $load -ArgumentList @("16","100","$LoadSecs","avx") -PassThru -WindowStyle Hidden

$prevE = Watt
$prevT = Get-Date
"t     watt  temp  fan1  fan2"
"----  ----  ----  ----  ----"

$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt ($LoadSecs + 12)) {
    Start-Sleep -Seconds 3
    $now = Get-Date
    $el = [math]::Round(($now - $t0).TotalSeconds, 1)

    $e = Watt
    $w = ''
    if ($null -ne $e -and $null -ne $prevE) {
        $dt = ($now - $prevT).TotalSeconds
        $de = [double]($e - $prevE)
        if ($de -lt 0) { $de += 4294967296.0 }
        $w = [math]::Round($de * $eu / $dt, 1)
    }
    $prevE = $e
    $prevT = $now

    $v = Msr 0x1B1
    if ($null -ne $v -and (($v -shr 31) -band 1)) { $temp = [int]($tj - (($v -shr 16) -band 0x7F)) } else { $temp = -1 }

    $om = Get-WmiObject -Namespace root/WMI -Class LENOVO_OTHER_METHOD
    $f1 = $om.GetFeatureValue([uint32]0x04030001).value
    $f2 = $om.GetFeatureValue([uint32]0x04030004).value

    "{0,4}  {1,4}  {2,4}  {3,4}  {4,4}" -f $el, $w, $temp, $f1, $f2
}

if ($p -and -not $p.HasExited) { try { $p.Kill() } catch {} }
if ($gpuProc -and -not $gpuProc.HasExited) { try { $gpuProc.Kill() } catch {} }

$om = Get-WmiObject -Namespace root/WMI -Class LENOVO_OTHER_METHOD
[void]$om.SetFeatureValue([uint32]0x04030001, [uint32]0)
[void]$om.SetFeatureValue([uint32]0x04030004, [uint32]0)
"fan released to EC"
