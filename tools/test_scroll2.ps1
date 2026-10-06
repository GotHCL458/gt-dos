$ErrorActionPreference = 'SilentlyContinue'
$floppy = 'D:\GT-DOS\build\gt-dos.img'
$disk   = 'D:\GT-DOS\build\gt-dos-hdd.img'
$sport  = 4501
$mport  = 4502

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
    Start-Sleep -Milliseconds 120
    try { $b = New-Object byte[] 4096; [void]$ms.Read($b,0,4096) } catch {}
}
function SendSer([string]$s) {
    $m = [Text.Encoding]::ASCII.GetBytes($s)
    $ss.Write($m,0,$m.Length); $ss.Flush()
}
function Shot([string]$name) {
    if (Test-Path $name) { Remove-Item $name }
    Mon "screendump $name"
    Start-Sleep -Milliseconds 250
}

DrainSer 2500 | Out-Null
SendSer "help`r"; DrainSer 1500 | Out-Null   # 制造多行历史

$CU = [char]0x1B + '[1;5A'   # Ctrl+Up
$CD = [char]0x1B + '[1;5B'   # Ctrl+Down

# 1) 滚到中间
SendSer ($CU * 5); Start-Sleep -Milliseconds 400
Shot 'D:\GT-DOS\build\t1_mid.ppm'

# 2) 滚到顶
SendSer ($CU * 60); Start-Sleep -Milliseconds 800
Shot 'D:\GT-DOS\build\t2_top.ppm'

# 3) 滚回到底 (钳制, 指示应消失)
SendSer ($CD * 200); Start-Sleep -Milliseconds 1500
Shot 'D:\GT-DOS\build\t3_bottom.ppm'

SendSer "halt`r"; DrainSer 1000 | Out-Null
$ser.Close(); $mon.Close()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

function Analyze([string]$f) {
    if (-not (Test-Path $f)) { Write-Host "$f MISSING"; return }
    $bytes = [IO.File]::ReadAllBytes($f)
    $pos = 0
    function NextToken { param([ref]$p)
        while ($bytes[$p.Value] -in 10,32,9) { $p.Value++ }
        $s = ''
        while ($bytes[$p.Value] -notin 10,32,9) { $s += [char]$bytes[$p.Value]; $p.Value++ }
        $p.Value++
        return $s
    }
    $pref = [ref]$pos
    $magic = NextToken $pref; $w = [int](NextToken $pref); $h = [int](NextToken $pref); $maxv = NextToken $pref
    $data = $pos
    $yellow = 0
    for ($y = [int]($h*0.88); $y -lt $h; $y += 2) {
        for ($x = [int]($w*0.4); $x -lt $w; $x += 2) {
            $i = $data + ($y * $w + $x) * 3
            if ($bytes[$i] -gt 200 -and $bytes[$i+1] -gt 200 -and $bytes[$i+2] -lt 120) { $yellow++ }
        }
    }
    Write-Host ("{0}: yellow={1}" -f (Split-Path $f -Leaf), $yellow)
}
Analyze 'D:\GT-DOS\build\t1_mid.ppm'
Analyze 'D:\GT-DOS\build\t2_top.ppm'
Analyze 'D:\GT-DOS\build\t3_bottom.ppm'
