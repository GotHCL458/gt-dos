$ErrorActionPreference = 'SilentlyContinue'
$floppy = 'D:\GT-DOS\build\gt-dos.img'
$disk   = 'D:\GT-DOS\build\gt-dos-hdd.img'
$port   = 4475

$a = @('-m','32','-fda',$floppy,'-hda',$disk,'-boot','a','-display','none',
       '-monitor','none','-serial',"tcp:127.0.0.1:$port,server,nowait")
$p = Start-Process -FilePath 'D:\GT-DOS\qemu\qemu-system-i386.exe' -ArgumentList $a -PassThru -WindowStyle Hidden

$c = $null
for ($i = 0; $i -lt 400 -and -not $c; $i++) {
    try { $t = New-Object System.Net.Sockets.TcpClient; $t.Connect('127.0.0.1',$port); $c = $t }
    catch { Start-Sleep -Milliseconds 5 }
}
if (-not $c) { Write-Host 'CONNECT FAILED'; Stop-Process -Id $p.Id -Force; exit 1 }

$st = $c.GetStream()
$st.ReadTimeout = 2000

function Drain([int]$seconds) {
    $sb = New-Object System.Text.StringBuilder
    $dl = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $dl) {
        try {
            $b = New-Object byte[] 8192
            $n = $st.Read($b, 0, 8192)
            if ($n -le 0) { break }
            [void]$sb.Append([Text.Encoding]::UTF8.GetString($b, 0, $n))
        } catch { break }
    }
    return $sb.ToString()
}

function Send([string]$s) {
    $m = [Text.Encoding]::ASCII.GetBytes($s)
    $st.Write($m, 0, $m.Length); $st.Flush()
}

# 触发一次重启, 以便捕获完整启动日志
Start-Sleep -Milliseconds 800
Send "reboot`r"
$boot = Drain 5
Write-Host "=================== BOOT LOG ==================="
Write-Host $boot

# 磁盘 / FAT 测试
Send "disk`r";      Write-Host "=================== DISK ==================="; Write-Host (Drain 3)
Send "mount`r";     Write-Host "=================== MOUNT =================="; Write-Host (Drain 3)
Send "dir`r";       Write-Host "=================== DIR ===================="; Write-Host (Drain 3)
Send "cat readme.txt`r"; Write-Host "=================== CAT README.TXT ========="; Write-Host (Drain 3)
Send "cat hello.txt`r"; Write-Host "=================== CAT HELLO.TXT =========="; Write-Host (Drain 3)
Send "cat bigfile.txt`r"; Write-Host "=================== CAT BIGFILE.TXT (multi-cluster) ========="; Write-Host (Drain 4)
Send "df`r";        Write-Host "=================== DF ====================="; Write-Host (Drain 3)

# 写入 / 回读 / 删除
Send "put note.txt hello gt-dos from kernel`r"; Write-Host "=================== PUT ====================="; Write-Host (Drain 3)
Send "dir`r";       Write-Host "=================== DIR (after PUT) ========="; Write-Host (Drain 3)
Send "cat note.txt`r"; Write-Host "=================== CAT NOTE.TXT ==========="; Write-Host (Drain 3)
Send "mkdir docs`r"; Write-Host "=================== MKDIR ==================="; Write-Host (Drain 3)
Send "dir`r";       Write-Host "=================== DIR (after MKDIR) ========"; Write-Host (Drain 3)
Send "del note.txt`r"; Write-Host "=================== DEL ====================="; Write-Host (Drain 3)
Send "dir`r";       Write-Host "=================== DIR (after DEL) ========="; Write-Host (Drain 3)

# 彩色输出
Send "color 11`r";  Write-Host "=================== COLOR 11 ==============="; Write-Host (Drain 2)
Send "color 7`r";   Write-Host "=================== COLOR 7 ================"; Write-Host (Drain 2)

# 错误路径 (红字)
Send "nosuchcmd`r"; Write-Host "=================== BAD CMD ================="; Write-Host (Drain 2)

# 行编辑: 输入 abc, 左移两次, 插入 X, 结果应为 aXbc
Send "abc"
Send ([char]0x1B + '[D' + [char]0x1B + '[D')
Send "X`r"
Write-Host "=================== LINE EDIT (expect aXbc) =="; Write-Host (Drain 2)

# 历史: 上翻一次回车, 应重复上一条命令 (color 7)
Send "uptime`r"; Write-Host "=================== UPTIME ================="; Write-Host (Drain 2)
Send ([char]0x1B + '[A')
Start-Sleep -Milliseconds 400
Send "`r"
Write-Host "=================== HISTORY (expect uptime) =="; Write-Host (Drain 2)

# 停机
Send "halt`r"
Write-Host "=================== HALT ==================="; Write-Host (Drain 2)

$c.Close()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
