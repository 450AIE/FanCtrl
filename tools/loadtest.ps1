param(
    [int]$Threads = 16,
    [int]$Duty = 100,
    [int]$Secs = 90,
    [string]$Mode = "avx",
    [double]$SafetyTemp = 88.0,
    [string]$Tag = "probe",
    [switch]$Gpu,
    [int]$GpuGroups = 2048,
    [int]$GpuIter = 4096
)

$ErrorActionPreference = 'Stop'
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root   = Split-Path -Parent $AppDir
$OutDir = Join-Path $Root 'thermal'
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$Csv = Join-Path $OutDir ("auto_" + $Tag + ".csv")

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Keep2 {
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
}
"@
[Keep2]::SetThreadExecutionState([uint32]2147483649) | Out-Null

# ---------- temp / power via MSR ----------
$lhmDll = Join-Path (Split-Path -Parent $Root) 'lhm\bin\LibreHardwareMonitorLib.dll'
if (-not (Test-Path $lhmDll)) { throw "LibreHardwareMonitorLib.dll not found: $lhmDll" }
$asm = [Reflection.Assembly]::LoadFrom($lhmDll)
$msrType = $asm.GetType('LibreHardwareMonitor.PawnIo.IntelMsr')
$script:msr     = [Activator]::CreateInstance($msrType)
$script:msrRead = $msrType.GetMethod('ReadMsr', [type[]]@([uint32], [uint64].MakeByRefType()))

function Read-Msr([uint32]$index) {
    $box = @($index, [uint64]0)
    if (-not $script:msrRead.Invoke($script:msr, $box)) { return $null }
    return [uint64]$box[1]
}
$tjRaw = Read-Msr 0x1A2
if ($null -eq $tjRaw) { throw "MSR 0x1A2 unreadable, PawnIO not available" }
$script:TjMax = [int](($tjRaw -shr 16) -band 0xFF)
$raplUnit = Read-Msr 0x606
$script:EnergyUnit = 1.0 / [math]::Pow(2, [int](($raplUnit -shr 8) -band 0x1F))

function Read-TempC {
    $v = Read-Msr 0x1B1
    if ($null -eq $v) { return $null }
    if ((($v -shr 31) -band 1) -ne 1) { return $null }
    return [int]($script:TjMax - (($v -shr 16) -band 0x7F))
}
function Read-Energy {
    $e = Read-Msr 0x611
    if ($null -eq $e) { return $null }
    return [uint64]($e -band 0xFFFFFFFF)
}

# ---------- fan: force release to EC auto ----------
$om = Get-WmiObject -Namespace root/WMI -Class LENOVO_OTHER_METHOD
$FanId = @([uint32]0x04030001, [uint32]0x04030004)
function Get-FanRpm([int]$i) {
    try { return [int]$om.GetFeatureValue($FanId[$i]).value } catch { return -1 }
}
function Release-Fan {
    foreach ($id in $FanId) {
        try { [void]$om.SetFeatureValue($id, [uint32]0) } catch { Write-Output ("  [WARN] release fan failed: " + $_.Exception.Message) }
    }
}

Write-Output "=== 1. release fans to EC (auto mode) ==="
Release-Fan
Start-Sleep -Seconds 4
$f1 = Get-FanRpm 0; $f2 = Get-FanRpm 1
Write-Output "  fan1=$f1 fan2=$f2 (auto)"
if ($f1 -lt 0) { throw "fan read failed, abort" }

# ---------- load ----------
$loadExe = Join-Path $AppDir 'loadgen.exe'
$gpuExe  = Join-Path $AppDir 'gpuload.exe'
if (-not (Test-Path $loadExe)) { throw "loadgen.exe not found" }
if ($Gpu -and -not (Test-Path $gpuExe)) { throw "gpuload.exe not found" }
$loadProc = $null
$gpuProc  = $null
$aborted = $false

"t,tempC,pkgW,fan1,fan2,duty,threads,mode,safetyTemp" | Out-File $Csv -Encoding utf8

try {
    Write-Output "=== 2. load start: mode=$Mode threads=$Threads duty=$Duty% gpu=$Gpu ==="
    if ($Gpu) {
        $gpuProc = Start-Process -FilePath $gpuExe -ArgumentList @("$($Secs + 60)", "256", "$GpuGroups", "$GpuIter") -PassThru -WindowStyle Hidden
    }
    $loadProc = Start-Process -FilePath $loadExe -ArgumentList @("$Threads", "$Duty", "$($Secs + 60)", "$Mode") -PassThru -WindowStyle Hidden

    $t0 = Get-Date
    $prevE = Read-Energy
    $prevT = $t0
    $samples = @()
    $maxW = 0.0; $maxTemp = 0

    while ($true) {
        Start-Sleep -Seconds 2
        $now = Get-Date
        $el = [math]::Round(($now - $t0).TotalSeconds, 1)
        if ($el -gt $Secs) { break }
        if ($loadProc.HasExited) { Write-Output "  (load exited early)"; break }

        $temp = Read-TempC
        $e = Read-Energy
        $pkgW = ''
        if ($null -ne $e -and $null -ne $prevE) {
            $dt = ($now - $prevT).TotalSeconds
            if ($dt -gt 0) {
                $de = [double]($e - $prevE)
                if ($de -lt 0) { $de += 4294967296.0 }
                $pkgW = [math]::Round($de * $script:EnergyUnit / $dt, 1)
            }
        }
        if ($null -ne $e) { $prevE = $e }
        $prevT = $now

        if ($null -eq $temp) { Write-Output "  temp read failed"; continue }

        $f1 = Get-FanRpm 0; $f2 = Get-FanRpm 1
        $samples += [pscustomobject]@{ t=$el; temp=$temp; w=$pkgW; f1=$f1; f2=$f2 }
        if ($pkgW -ne '' -and $pkgW -gt $maxW) { $maxW = $pkgW }
        if ($temp -gt $maxTemp) { $maxTemp = $temp }

        "$el,$temp,$pkgW,$f1,$f2,$Duty,$Threads,$Mode,$SafetyTemp" | Out-File $Csv -Append -Encoding utf8
        Write-Output ("  t=" + $el + "s  " + $temp + "C  " + $pkgW + "W  fan " + $f1 + "/" + $f2)

        if ($temp -ge $SafetyTemp) {
            Write-Output ("  !! temp " + $temp + " >= " + $SafetyTemp + ", stop load now")
            $aborted = $true
            break
        }
    }
}
finally {
    if ($loadProc -and -not $loadProc.HasExited) { try { $loadProc.Kill() } catch {} }
    if ($gpuProc  -and -not $gpuProc.HasExited)  { try { $gpuProc.Kill() }  catch {} }
    Start-Sleep -Seconds 2
    Release-Fan
    [Keep2]::SetThreadExecutionState([uint32]2147483648) | Out-Null
}

# ---------- summary ----------
if ($samples.Count -gt 0) {
    $tail = $samples | Where-Object { $_.t -ge ($samples[-1].t - 30) }
    if ($tail.Count -eq 0) { $tail = $samples | Select-Object -Last 3 }
    $avgT = [math]::Round((($tail | Measure-Object -Property temp -Average).Average), 1)
    $avgW = [math]::Round((($tail | Where-Object { $_.w -ne '' } | Measure-Object -Property w -Average).Average), 1)
    $last = $samples[-1]
    Write-Output ""
    Write-Output ("=== result: last30s avg " + $avgT + "C / " + $avgW + "W, peak " + $maxTemp + "C / " + $maxW + "W, end fan " + $last.f1 + "/" + $last.f2 + " ===")
    if ($aborted) { Write-Output "(aborted by over-temp, data for reference only)" }
}
Write-Output "fans released to EC (auto mode)"
