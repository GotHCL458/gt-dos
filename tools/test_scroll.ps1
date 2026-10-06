$ErrorActionPreference = 'SilentlyContinue'
$floppy = 'D:\GT-DOS\build\gt-dos.img'
$disk   = 'D:\GT-DOS\build\gt-dos-hdd.img'
$sport  = 4495
$mport  = 4496

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
$ss = $ser.GetStream(); $ss.ReadTimeout = 1200
$ms = $mon.GetStream(); $ms.ReadTimeout = 1200

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
    Start-Sleep -Milliseconds 100
    try { $b = New-Object byte[] 4096; [void]$ms.Read($b,0,4096) } catch {}
}
function SendSer([string]$s) {
    $m = [Text.Encoding]::ASCII.GetBytes($s)
    $ss.Write($m,0,$m.Length); $ss.Flush()
}

DrainSer 2500 | Out-Null

# 制造历史: help
SendSer "help`r"; DrainSer 1500 | Out-Null

# 串口发 Ctrl+Up x3 (ESC[1;5A)
SendSer ([char]0x1B + '[1;5A' + [char]0x1B + '[1;5A' + [char]0x1B + '[1;5A')
Start-Sleep -Milliseconds 500
SendSer "view`r"
Write-Host "=== after 3x Ctrl+Up (expect 9) ==="; Write-Host (DrainSer 1500)

# Ctrl+Down x2 (ESC[1;5B)
SendSer ([char]0x1B + '[1;5B' + [char]0x1B + '[1;5B')
Start-Sleep -Milliseconds 500
SendSer "view`r"
Write-Host "=== after 2x Ctrl+Down (expect 3) ==="; Write-Host (DrainSer 1500)

# Ctrl+Down x5 -> 到底 0
SendSer ([char]0x1B + '[1;5B' + [char]0x1B + '[1;5B' + [char]0x1B + '[1;5B' + [char]0x1B + '[1;5B' + [char]0x1B + '[1;5B')
Start-Sleep -Milliseconds 500
SendSer "view`r"
Write-Host "=== after bottom clamp (expect 0) ==="; Write-Host (DrainSer 1500)

# Ctrl+Up x200 -> 到顶钳制
$up = ([char]0x1B + '[1;5A') * 200
SendSer $up
Start-Sleep -Milliseconds 2000
SendSer "view`r"
Write-Host "=== after top clamp (expect max_off) ==="; Write-Host (DrainSer 1500)

SendSer "halt`r"; DrainSer 1500 | Out-Null
$ser.Close(); $mon.Close()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
