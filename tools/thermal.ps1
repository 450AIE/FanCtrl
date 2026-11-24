param(
    [string]$Rpms    = "3000",
    [string]$Duties  = "100",
    [int]$StepSec    = 120,
    [string]$Tag     = "test",
    [int]$Threads    = 16,
    [int]$SafetyTemp = 94
)

$ErrorActionPreference = 'Stop'
$AppDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root    = Split-Path -Parent $AppDir
$OutDir  = Join-Path $Root 'thermal'
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$RawCsv  = Join-Path $OutDir ("raw_$Tag.csv")
$SumCsv  = Join-Path $OutDir ("summary_$Tag.csv")

# ---------- 防止睡眠 ----------
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Keep {
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
}
"@
[Keep]::SetThreadExecutionState([uint32]2147483649) | Out-Null   # ES_CONTINUOUS|ES_SYSTEM_REQUIRED   # ES_CONTINUOUS|SYSTEM|DISPLAY

# ---------- 温度：PawnIO + MSR ----------
$lhmDll = Join-Path (Split-Path -Parent $Root) 'lhm\bin\LibreHardwareMonitorLib.dll'
if (-not (Test-Path $lhmDll)) { throw "找不到 LibreHardwareMonitorLib.dll: $lhmDll" }
$asm    = [Reflection.Assembly]::LoadFrom($lhmDll)
$msrType = $asm.GetType('LibreHardwareMonitor.PawnIo.IntelMsr')
if (-not $msrType) { throw "找不到 IntelMsr 类型" }
$script:msr     = [Activator]::CreateInstance($msrType)
$script:msrRead = $msrType.GetMethod('ReadMsr', [type[]]@([uint32], [uint64].MakeByRefType()))

function Read-Msr([uint32]$index) {
    $box = @($index, [uint64]0)
    if (-not $script:msrRead.Invoke($script:msr, $box)) { return $null }
    return [uint64]$box[1]
}

$tjRaw = Read-Msr 0x1A2
if ($null -eq $tjRaw) { throw "读不到 MSR 0x1A2，PawnIO 可能没装好" }
$script:TjMax = [int](($tjRaw -shr 16) -band 0xFF)
Write-Output ("TjMax = " + $script:TjMax)

# RAPL 能量单位（用于算封装功耗）
$raplUnit = Read-Msr 0x606
$script:EnergyUnit = 1.0 / [math]::Pow(2, [int](($raplUnit -shr 8) -band 0x1F))
Write-Output ("RAPL energy unit = " + $script:EnergyUnit + " J")

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

# ---------- 风扇 ----------
$script:om    = Get-WmiObject -Namespace root/WMI -Class LENOVO_OTHER_METHOD
$script:FanId = @([uint32]0x04030001, [uint32]0x04030004)

function Set-Fan([int]$rpm) {
    foreach ($id in $script:FanId) {
        try { [void]$script:om.SetFeatureValue($id, [uint32]$rpm) } catch {}
    }
}
function Get-FanRpm([int]$i) {
    try { return [int]$script:om.GetFeatureValue($script:FanId[$i]).value } catch { return -1 }
}
function Release-Fan { Set-Fan 0 }

# ---------- 负载 ----------
$loadExe = Join-Path $AppDir 'loadgen.exe'
if (-not (Test-Path $loadExe)) { throw "找不到 loadgen.exe" }

# ---------- 主流程 ----------
$rpmList  = $Rpms   -split ','   | ForEach-Object { [int]$_.Trim() }
$dutyList = $Duties -split ','   | ForEach-Object { [int]$_.Trim() }
$dutyList = $dutyList | Sort-Object

"tag,rpm,duty,cpuPct,elapsedSec,tempC,pkgW,fan1,fan2,targetRpm" | Out-File $RawCsv -Encoding utf8
"tag,targetRpm,duty,tempAvg,tempMin,tempMax,driftLast30s,pkgW,fan1,fan2,settled" | Out-File $SumCsv -Encoding utf8

$script:loadProc = $null
$abort = $false

try {
    foreach ($rpm in $rpmList) {
        foreach ($duty in $dutyList) {
            $label = "rpm=$rpm duty=$duty"
            Write-Output ("--- $label ---")

            Set-Fan $rpm
            Start-Sleep -Seconds 10        # 等风扇转速到位、温度开始响应

            $dur = $StepSec + 90      # 仅作兜底，正常由脚本到点终止
            $script:loadProc = Start-Process -FilePath $loadExe `
                -ArgumentList @("$Threads", "$duty", "$dur") -PassThru -WindowStyle Hidden

            $t0     = Get-Date
            $prevE  = Read-Energy
            $prevT  = $t0
            $samples = @()
            $cellAbort = $false
            $lastCpu = -1

            while ($true) {
                Start-Sleep -Seconds 3
                $now = Get-Date
                $el  = [math]::Round(($now - $t0).TotalSeconds, 1)
                if ($el -gt $StepSec) { break }
                if ($script:loadProc.HasExited) { Write-Output "  (负载进程提前退出)"; break }

                $temp = Read-TempC
                $e    = Read-Energy
                $pkgW = ''
                if ($null -ne $e -and $null -ne $prevE) {
                    $dt = ($now - $prevT).TotalSeconds
                    if ($dt -gt 0) {
                        $de = [double]($e - $prevE)
                        if ($de -lt 0) { $de += 4294967296.0 }      # 32 位计数器回绕
                        $pkgW = [math]::Round($de * $script:EnergyUnit / $dt, 1)
                    }
                }
                if ($null -ne $e) { $prevE = $e }
                $prevT = $now

                if ($null -eq $temp) {
                    Write-Output "  温度读取失败"
                    continue
                }

                # 超温保护
                if ($temp -ge $SafetyTemp) {
                    Write-Output ("  !! 温度 $temp >= $SafetyTemp，本格放弃，交还 EC 并降温")
                    $cellAbort = $true
                    break
                }

                $f1 = Get-FanRpm 0
                $f2 = Get-FanRpm 1
                $samples += [pscustomobject]@{ t=$el; temp=$temp; pkg=$pkgW; f1=$f1; f2=$f2 }

                "$Tag,$rpm,$duty,,$el,$temp,$pkgW,$f1,$f2,$rpm" | Out-File $RawCsv -Append -Encoding utf8
                Write-Output ("   t=" + $el + "s  " + $temp + "C  " + $pkgW + "W  fan " + $f1 + "/" + $f2)
            }

            # 停负载
            if ($script:loadProc -and -not $script:loadProc.HasExited) {
                try { $script:loadProc.Kill() } catch {}
            }
            $script:loadProc = $null

            if ($cellAbort) {
                "$Tag,$rpm,$duty,ABORT,,,,,,$rpm" | Out-File $SumCsv -Append -Encoding utf8
                Release-Fan
                Start-Sleep -Seconds 25
                continue
            }

            # 统计：末尾 30 秒
            $tail = $samples | Where-Object { $_.t -ge ($samples[-1].t - 30) }
            if ($tail.Count -eq 0) { $tail = $samples | Select-Object -Last 3 }
            $avg  = ($tail | Measure-Object -Property temp -Average).Average
            $min  = ($tail | Measure-Object -Property temp -Minimum).Minimum
            $max  = ($tail | Measure-Object -Property temp -Maximum).Maximum
            $drift = if ($tail.Count -ge 2) { $tail[-1].temp - $tail[0].temp } else { 0 }
            $pkgAvg = ($tail | Where-Object { $_.pkg -ne '' } | Measure-Object -Property pkg -Average).Average
            $settled = if ([math]::Abs($drift) -le 1.5) { 'yes' } else { 'no' }

            "$Tag,$rpm,$duty," + [math]::Round($avg,1) + "," + $min + "," + $max + ",$drift," +
                [math]::Round($pkgAvg,1) + "," + $samples[-1].f1 + "," + $samples[-1].f2 + ",$settled" |
                Out-File $SumCsv -Append -Encoding utf8

            Write-Output ("   => 稳态 " + [math]::Round($avg,1) + "C  (" + $min + "~" + $max + ", drift=" + $drift + ", " + $settled + ")  " + [math]::Round($pkgAvg,1) + "W")

            Start-Sleep -Seconds 8
        }
        if ($abort) { break }
    }
}
finally {
    if ($script:loadProc -and -not $script:loadProc.HasExited) { try { $script:loadProc.Kill() } catch {} }
    Release-Fan
    [Keep]::SetThreadExecutionState([uint32]2147483648) | Out-Null
    Write-Output ("结束：风扇已交还 EC" + $(if ($abort) { "（因超温中止）" } else { "" }))
}
