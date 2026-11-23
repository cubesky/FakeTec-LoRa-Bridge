param([int]$N=2000,[switch]$Flash)
$nu="$env:USERPROFILE\.platformio\packages\framework-arduinoadafruitnrf52\tools\adafruit-nrfutil\win32\adafruit-nrfutil.exe"
if($Flash){ foreach($c in 'COM3','COM4'){ & $nu dfu serial -pkg "$PSScriptRoot\..\.pio\build\faketec\firmware.zip" -p $c -b 115200 -t 1200 --singlebank 2>&1 | Select-Object -Last 1 }; Start-Sleep 12 }
function Open($n){ $p = New-Object System.IO.Ports.SerialPort $n,115200; $p.DtrEnable=$true; $p.RtsEnable=$true; $p.NewLine="`r`n"; $p.ReadBufferSize=65536; $p.Open(); $p }
function Cmd($p,$c,$ms=700){ $p.WriteLine($c); Start-Sleep -Milliseconds $ms; $p.ReadExisting() }
function Mk($tag,$n){ $sb=New-Object Text.StringBuilder; $i=0; while($sb.Length -lt $n){ [void]$sb.Append(("{0}{1:D5};" -f $tag,$i)); $i++ }; $sb.ToString().Substring(0,$n) }
function Xfer($src,$dst,$data,$tmo){ $sw=[Diagnostics.Stopwatch]::StartNew(); $src.Write($data); $got=''; while($got.Length -lt $data.Length -and $sw.Elapsed.TotalSeconds -lt $tmo){ Start-Sleep -Milliseconds 100; $got+=$dst.ReadExisting() }; "{0}/{1} bytes in {2:N1}s = {3:N0} B/s, intact={4}" -f $got.Length,$data.Length,$sw.Elapsed.TotalSeconds,($got.Length/$sw.Elapsed.TotalSeconds),($got -ceq $data) }
$a=Open COM3; $b=Open COM4; Start-Sleep -Milliseconds 1500
foreach($p in $a,$b){ $p.Write('+++') }; Start-Sleep -Milliseconds 1300
for($i=0;$i -lt 15;$i++){ $r=Cmd $a 'ATI' 500; if($r -match 'link UP'){break}; Start-Sleep 1 }
"link: " + (($r -split "`n" | ? { $_ -match 'Peer' }) -join '')
foreach($p in $a,$b){ $null=Cmd $p 'ATO' 300; $null=$p.ReadExisting() }
"A->B ${N}: " + (Xfer $a $b (Mk 'A' $N) 120)
"B->A ${N}: " + (Xfer $b $a (Mk 'B' $N) 120)
$da=Mk 'X' $N; $db=Mk 'Y' $N; $sw=[Diagnostics.Stopwatch]::StartNew(); $a.Write($da); $b.Write($db); $ga='';$gb=''
while(($ga.Length -lt $N -or $gb.Length -lt $N) -and $sw.Elapsed.TotalSeconds -lt 180){ Start-Sleep -Milliseconds 100; $ga+=$a.ReadExisting(); $gb+=$b.ReadExisting() }
"both ways $N each: {0:N1}s  A got {1} intact={2}; B got {3} intact={4}" -f $sw.Elapsed.TotalSeconds,$ga.Length,($ga -ceq $db),$gb.Length,($gb -ceq $da)
foreach($p in $a,$b){ Start-Sleep -Milliseconds 1100; $p.Write('+++') }; Start-Sleep -Milliseconds 1300
"== A"; (Cmd $a 'AT+STAT') -replace "(\r?\n)+","`n"; "== B"; (Cmd $b 'AT+STAT') -replace "(\r?\n)+","`n"
foreach($p in $a,$b){ $null=Cmd $p 'ATO' 300 }
$a.Close(); $b.Close()
