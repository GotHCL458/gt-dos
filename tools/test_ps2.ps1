$ErrorActionPreference = 'SilentlyContinue'
$floppy = 'D:\GT-DOS\build\gt-dos.img'
$disk   = 'D:\GT-DOS\build\gt-dos-hdd.img'
$sport  = 4481
$mport  = 4482

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

$ser = Connect-Port $sport
$mon = Connect-Port $mport
if (-not $ser -or -not $mon) { Write-Host 'CONNECT FAILED'; Stop-Process -Id $p.Id -Force; exit 1 }

$ss = $ser.GetStream(); $ss.ReadTimeout = 1500
$ms = $mon.GetStream(); $ms.ReadTimeout = 1500

function DrainSer([int]$ms2) {
    $sb = New-Object System.Text.StringBuilder
    $dl = (Get-Date).AddMilliseconds($ms2)
    while ((Get-Date) -lt $dl) {
        try {
            $b = New-Object byte[] 8192
            $n = $ss.Read($b, 0, 8192)
            if ($n -le 0) { break }
            [void]$sb.Append([Text.Encoding]::UTF8.GetString($b, 0, $n))
        } catch { break }
    }
    return $sb.ToString()
}

function Mon([string]$cmd) {
    $m = [Text.Encoding]::ASCII.GetBytes($cmd + "`n")
    $ms.Write($m, 0, $m.Length); $ms.Flush()
    Start-Sleep -Milliseconds 120
    # 排空 monitor 回显
    try { $b = New-Object byte[] 4096; [void]$ms.Read($b, 0, 4096) } catch {}
}

Start-Sleep -Milliseconds 1500
$boot = DrainSer 2000
Write-Host "=================== BOOT ==================="
Write-Host $boot

# ---- 用 sendkey 模拟 PS/2 键盘: 输入 "dir" + Enter ----
Mon 'sendkey d'
Mon 'sendkey i'
Mon 'sendkey r'
Mon 'sendkey ret'
Write-Host "=================== PS2 sendkey 'dir' ========="
Write-Host (DrainSer 3000)

# ---- 输入退格测试: "diX" 然后 backspace 再 "r" enter => dir ----
Mon 'sendkey d'
Mon 'sendkey i'
Mon 'sendkey x'
Mon 'sendkey backspace'
Mon 'sendkey r'
Mon 'sendkey ret'
Write-Host "=================== PS2 backspace edit (expect dir) ========="
Write-Host (DrainSer 3000)

# ---- 方向键: 输入 "ver" 回车, 再按上箭头+回车 应重复 ver ----
Mon 'sendkey v'
Mon 'sendkey e'
Mon 'sendkey r'
Mon 'sendkey ret'
Write-Host "=================== PS2 'ver' ========="
Write-Host (DrainSer 2000)
Mon 'sendkey up'
Mon 'sendkey ret'
Write-Host "=================== PS2 Up+Enter (expect ver again) ========="
Write-Host (DrainSer 2000)

# ---- Ctrl+Up 滚动 ----
Mon 'sendkey ctrl-ent'   # 先清行 (ctrl-ent 无效果, 只是保险)
Mon 'sendkey ctrl-up'
Mon 'sendkey ctrl-up'
Mon 'sendkey ctrl-up'
Write-Host "=================== PS2 Ctrl+Up x3 (scroll) ========="
Write-Host (DrainSer 2000)
Mon 'sendkey ctrl-down'
Write-Host "=================== PS2 Ctrl+Down x1 ========="
Write-Host (DrainSer 2000)

Mon 'sendkey h'
Mon 'sendkey a'
Mon 'sendkey l'
Mon 'sendkey t'
Mon 'sendkey ret'
Write-Host "=================== HALT ========="
Write-Host (DrainSer 2000)

$ser.Close(); $mon.Close()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
