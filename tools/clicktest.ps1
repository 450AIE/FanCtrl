# UI 回归：模拟点击关键控件，用 EC 实际转速 / settings.ini 验证是否生效
#
# 两个坑，都踩过：
# 1. 本文件必须以 UTF-8 with BOM 保存。PowerShell 5.1 会把无 BOM 的 UTF-8 当 GBK 解析，
#    中文注释会破坏语法，表现为脚本「前半段能跑、后面莫名失效」。
# 2. 脚本开头必须把自身设为 DPI 感知（SetProcessDpiAwarenessContext）。
#    FanCtrl 是 PER_MONITOR_AWARE_V2 进程，客户端坐标是物理像素；
#    DPI 不感知的调用方拿到的 GetClientRect 会被虚拟化（200% 缩放下只剩一半），
#    按虚拟坐标 PostMessage 会落到窗口别的位置，点击「无声失败」。
#
# 坐标不是写死的，而是按 main.cpp 的 ComputeLayout 复算，换分辨率/缩放比都能用。

$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class CT {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
[void][CT]::SetProcessDpiAwarenessContext([IntPtr](-4))

$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root   = Split-Path -Parent $AppDir
$exe    = Join-Path $Root 'FanCtrl.exe'

Get-Process FanCtrl -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
# 等旧实例彻底退出：单实例锁还在时新进程会直接把消息转给旧窗口然后自杀，
# 这时抓到的句柄是别人的，点击会「无声失败」。
for ($i = 0; $i -lt 20; $i++) {
    if (-not (Get-Process FanCtrl -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 300
}

$t0 = Get-Date
Start-Process -FilePath $exe

$p = $null
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    $p = Get-Process FanCtrl -ErrorAction SilentlyContinue |
         Where-Object { $_.StartTime -ge $t0.AddSeconds(-1) -and $_.MainWindowHandle -ne 0 } |
         Select-Object -First 1
    if ($p) { break }
}
if (-not $p) { 'no process'; exit 1 }
Start-Sleep -Seconds 5
[void][CT]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 600
$h = $p.MainWindowHandle

$r = New-Object CT+RECT
[void][CT]::GetClientRect($h, [ref]$r)
$dpi = [CT]::GetDpiForWindow($h)
$k   = $dpi / 96.0
# 注意变量名：PowerShell 不区分大小写，这里千万别叫 $W / $H，
# 会把上面的窗口句柄 $h 覆盖掉，之后 PostMessage 全部失败且不报错。
$cw = $r.R; $ch = $r.B
"client=$cw x $ch  dpi=$dpi  scale=$k"

# ---- 按 ComputeLayout 复算各控件中心（逻辑单位 -> 物理像素）----
function SD([double]$v) { return [int][math]::Round($v * $k) }

$pad   = SD 16
$rightW = SD 212
$bottomH = SD 58
$cardRight = $cw - $pad - $rightW - (SD 12)
$bottomTop = $ch - $pad - $bottomH + (SD 8)
$bottomBot = $ch - $pad
$bw = SD 66; $bh = SD 30
$by = $bottomTop + [int](($bottomBot - $bottomTop - $bh) / 2)
$cy = $by + [int]($bh / 2)

$btnCurve = $pad + [int]($bw / 2)
$btnFull  = $pad + 2 * ($bw + (SD 4)) + [int]($bw / 2)

$sw = SD 86
$relLeft  = ($cw - $pad) - $sw
$saveLeft = $relLeft - (SD 8) - $sw
$recLeft  = $saveLeft - (SD 8) - $sw
$btnRelease = $relLeft + [int]($sw / 2)
$btnSave    = $saveLeft + [int]($sw / 2)
$btnRec     = $recLeft + [int]($sw / 2)

# 试听区：右侧面板底部
$panelL = $cardRight + (SD 12)
$panelR = $cw - $pad
$py  = ($ch - $pad - $bottomH) - (SD 142)
$pbw = [int](($panelR - $panelL - (SD 32) - (SD 8)) / 3)
$bx  = $panelL + (SD 16)
$pcy = $py + (SD 48) + [int]((SD 30) / 2)
$plusLeft = $bx + 2 * ($pbw + (SD 4))
$btnMinus  = $bx + [int]($pbw / 2)
$btnToggle = $bx + $pbw + (SD 4) + [int]($pbw / 2)
$btnPlus   = $plusLeft + [int](($panelR - (SD 16) - $plusLeft) / 2)

"coords: full=($btnFull,$cy) save=($btnSave,$cy) rec=($btnRec,$cy) release=($btnRelease,$cy)"
"        prev: minus=($btnMinus,$pcy) toggle=($btnToggle,$pcy) plus=($btnPlus,$pcy)"

$om = Get-WmiObject -Namespace root/WMI -Class LENOVO_OTHER_METHOD
function F1 { return $om.GetFeatureValue([uint32]0x04030001).value }
function F2 { return $om.GetFeatureValue([uint32]0x04030004).value }

function K([int]$x, [int]$y) {
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    $a = [CT]::PostMessage($h, 0x0201, [IntPtr]1, $lp)
    Start-Sleep -Milliseconds 90
    $b = [CT]::PostMessage($h, 0x0202, [IntPtr]0, $lp)
    "  post ($x,$y) hwnd=$h down=$a up=$b"
    Start-Sleep -Milliseconds 400
}

# 先在图表空白处点一下。刚启动的窗口第一条鼠标消息常常被激活过程吃掉，
# 后续点击才是真正的「用户点击」。
K (SD 500) (SD 150)
Start-Sleep -Seconds 1

"before:         $(F1) / $(F2)"
"click full"
K $btnFull $cy
Start-Sleep -Seconds 5
"after full:     $(F1) / $(F2)"
"click preview on"
K $btnToggle $pcy
Start-Sleep -Seconds 5
"after preview:  $(F1) / $(F2)"
"click +100"
K $btnPlus $pcy
Start-Sleep -Seconds 4
"after +100:     $(F1) / $(F2)"
"click -100 twice"
K $btnMinus $pcy
K $btnMinus $pcy
Start-Sleep -Seconds 4
"after -200:     $(F1) / $(F2)"
"click preview off"
K $btnToggle $pcy
Start-Sleep -Seconds 5
"after stop:     $(F1) / $(F2)"

$ini = Join-Path $Root 'settings.ini'
"ini: $((Get-Content $ini) -join ' | ')"
