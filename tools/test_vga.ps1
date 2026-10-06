$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Drawing

$floppy = 'D:\GT-DOS\build\gt-dos.img'
$disk   = 'D:\GT-DOS\build\gt-dos-hdd.img'
$sport  = 4491
$mport  = 4492
$ppm    = 'D:\GT-DOS\build\screen.ppm'

$a = @('-m','32','-fda',$floppy,'-hda',$disk,'-boot','a','-display','none',
       '-monitor',"tcp:127.0.0.1:$mport,server,nowait",
       '-serial',"tcp:127.0.0.1:$sport,server,nowait")
$p = Start-Process -FilePath 'D:\GT-DOS\qemu\qemu-system-i386.exe' -ArgumentList $a -PassThru -WindowStyle Hidden

function Connect-Port([int]$port) {
    for ($i = 0; $i -lt 400; $i++) {
        try { $t = New-Object System.Net.Sockets.TcpClient; $t.Connect('127.0.0.1',$port); return $t }
        catch { Start-Sleep -Milliseconds 5 }
    }
    return $null
}
$ser = Connect-Port $sport; $mon = Connect-Port $mport
if (-not $ser -or -not $mon) { Write-Host 'CONNECT FAILED'; Stop-Process -Id $p.Id -Force; exit 1 }
$ss = $ser.GetStream(); $ss.ReadTimeout = 1000
$ms = $mon.GetStream(); $ms.ReadTimeout = 1000

function DrainSer([int]$ms2) {
    $sb = New-Object System.Text.StringBuilder
    $dl = (Get-Date).AddMilliseconds($ms2)
    while ((Get-Date) -lt $dl) {
        try { $b = New-Object byte[] 8192; $n = $ss.Read($b,0,8192); if ($n -le 0) { break }
              [void]$sb.Append([Text.Encoding]::UTF8.GetString($b,0,$n)) } catch { break }
    }
    return $sb.ToString()
}
function Mon([string]$cmd) {
    $m = [Text.Encoding]::ASCII.GetBytes($cmd + "`n")
    $ms.Write($m,0,$m.Length); $ms.Flush()
    Start-Sleep -Milliseconds 150
    try { $b = New-Object byte[] 4096; [void]$ms.Read($b,0,4096) } catch {}
}
function Shot([string]$name) {
    if (Test-Path $name) { Remove-Item $name }
    Mon "screendump $name"
    Start-Sleep -Milliseconds 300
}

# 启动后: 用 help 输出超过一屏的内容
DrainSer 2500 | Out-Null
foreach ($k in 'help'.ToCharArray()) { Mon "sendkey $k" }
Mon 'sendkey ret'
DrainSer 2500 | Out-Null

# 1) 未滚动时的画面 (检查提示符颜色)
Shot 'D:\GT-DOS\build\s_noscroll.ppm'

# 2) Ctrl+Up x5 -> 应出现右下角黄色 SCROLL 指示条
Mon 'sendkey ctrl-up'; Mon 'sendkey ctrl-up'; Mon 'sendkey ctrl-up'
Mon 'sendkey ctrl-up'; Mon 'sendkey ctrl-up'
Shot 'D:\GT-DOS\build\s_scroll.ppm'

# 3) 一直 Ctrl+Down 到底部 -> 指示条消失
1..12 | ForEach-Object { Mon 'sendkey ctrl-down' }
Shot 'D:\GT-DOS\build\s_bottom.ppm'

# 4) 再一直 Ctrl+Up 到顶 -> 上限钳制 (行数不超过回滚上限)
1..400 | ForEach-Object { Mon 'sendkey ctrl-up' }
Shot 'D:\GT-DOS\build\s_top.ppm'

Mon 'sendkey h'; Mon 'sendkey a'; Mon 'sendkey l'; Mon 'sendkey t'; Mon 'sendkey ret'
DrainSer 1500 | Out-Null
$ser.Close(); $mon.Close()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

# ---- 分析 PPM: 检查黄色像素 (R>200,G>200,B<80) 数量 ----
function Analyze([string]$f) {
    if (-not (Test-Path $f)) { Write-Host "$f MISSING"; return }
    $bytes = [IO.File]::ReadAllBytes($f)
    # P6 头部解析: "P6\n<w> <h>\n255\n"
    $pos = 0
    function NextToken {
        param([ref]$p)
        while ($bytes[$p.Value] -in 10,32,9) { $p.Value++ }
        $s = ''
        while ($bytes[$p.Value] -notin 10,32,9) { $s += [char]$bytes[$p.Value]; $p.Value++ }
        $p.Value++
        return $s
    }
    $pref = [ref]$pos
    $magic = NextToken $pref
    $w = [int](NextToken $pref)
    $h = [int](NextToken $pref)
    $maxv = NextToken $pref
    $data = $pos
    $yellow = 0
    $y0 = [int]($h*0.88)
    for ($y = $y0; $y -lt $h; $y += 2) {
        for ($x = [int]($w*0.4); $x -lt $w; $x += 2) {
            $i = $data + ($y * $w + $x) * 3
            if ($bytes[$i] -gt 200 -and $bytes[$i+1] -gt 200 -and $bytes[$i+2] -lt 120) { $yellow++ }
        }
    }
    Write-Host ("{0}: magic={1} {2}x{3}  yellow-pixels-in-bottom-right={4}" -f (Split-Path $f -Leaf), $magic, $w, $h, $yellow)
}
Analyze 'D:\GT-DOS\build\s_noscroll.ppm'
Analyze 'D:\GT-DOS\build\s_scroll.ppm'
Analyze 'D:\GT-DOS\build\s_bottom.ppm'
Analyze 'D:\GT-DOS\build\s_top.ppm'
