param(
    [int]$Secs = 40,
    [int]$Tail = 25,
    [int]$Threads = 16
)

$ErrorActionPreference = 'Stop'
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root   = Split-Path -Parent $AppDir

$lhmDll = Join-Path (Split-Path -Parent $Root) 'lhm\bin\LibreHardwareMonitorLib.dll'
$asm = [Reflection.Assembly]::LoadFrom($lhmDll)
$msrType = $asm.GetType('LibreHardwareMonitor.PawnIo.IntelMsr')
$script:msr = [Activator]::CreateInstance($msrType)
$script:msrRead = $msrType.GetMethod('ReadMsr', [type[]]@([uint32], [uint64].MakeByRefType()))
function Read-Msr([uint32]$i) {
    $box = @($i, [uint64]0)
    if (-not $script:msrRead.Invoke($script:msr, $box)) { return $null }
    return [uint64]$box[1]
}
$unit = Read-Msr 0x606
$eUnit = 1.0 / [math]::Pow(2, [int](($unit -shr 8) -band 0x1F))
$tj = [int](((Read-Msr 0x1A2) -shr 16) -band 0xFF)

$om = Get-WmiObject -Namespace root/WMI -Class LENOVO_OTHER_METHOD
$FanId = @([uint32]0x04030001, [uint32]0x04030004)
function FanRpm([int]$i) { try { return [int]$om.GetFeatureValue($FanId[$i]).value } catch { return -1 } }

function PkgW {
    $e = Read-Msr 0x611
    if ($null -eq $e) { return $null }
    return [uint64]($e -band 0xFFFFFFFF)
}
function TempC {
    $v = Read-Msr 0x1B1
    if ($null -eq $v) { return -1 }
    if ((($v -shr 31) -band 1) -ne 1) { return -1 }
    return [int]($tj - (($v -shr 16) -band 0x7F))
}

$loadExe = Join-Path $AppDir 'loadgen.exe'
$proc = $null

Write-Output "t(s)   pkgW   temp   fan1   fan2"
Write-Output "-----  -----  -----  -----  -----"

try {
    $proc = Start-Process -FilePath $loadExe -ArgumentList @("$Threads", "100", "$($Secs + 10)", "avx") -PassThru -WindowStyle Hidden
    $t0 = Get-Date
    $prevE = PkgW
    $prevT = $t0
    $total = $Secs + $Tail

    while ($true) {
        Start-Sleep -Seconds 2
        $now = Get-Date
        $el = [math]::Round(($now - $t0).TotalSeconds, 1)
        if ($el -gt $total) { break }
        if ($el -gt $Secs -and $proc -and -not $proc.HasExited) { try { $proc.Kill() } catch {} }

        $e = PkgW
        $w = ''
        if ($null -ne $e -and $null -ne $prevE) {
            $dt = ($now - $prevT).TotalSeconds
            $de = [double]($e - $prevE); if ($de -lt 0) { $de += 4294967296.0 }
            $w = [math]::Round($de * $eUnit / $dt, 1)
        }
        if ($null -ne $e) { $prevE = $e }
        $prevT = $now

        $f1 = FanRpm 0; $f2 = FanRpm 1
        Write-Output ("{0,5}  {1,5}  {2,5}  {3,5}  {4,5}" -f $el, $w, (TempC), $f1, $f2)
    }
}
finally {
    if ($proc -and -not $proc.HasExited) { try { $proc.Kill() } catch {} }
}
Write-Output "done"
