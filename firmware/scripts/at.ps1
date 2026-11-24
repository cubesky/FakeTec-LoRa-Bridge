# One or more AT commands to one bridge over the in-band control channel (no +++ needed,
# the data stream is not disturbed).   .\at.ps1 COM4 "AT+ETH=1;AT+ETHST"
# Works in Windows PowerShell 5 and PowerShell 7.
param([string]$Port='COM3',[string]$Cmds='ATI',[int]$Ms=3000)
$ESC=[string][char]27; $BEL=[string][char]7
$p = New-Object System.IO.Ports.SerialPort $Port,115200; $p.DtrEnable=$true; $p.RtsEnable=$true; $p.Open(); Start-Sleep -Milliseconds 300; $null=$p.ReadExisting()
foreach($c in $Cmds.Split(';')){
  $r=''; $sw=[Diagnostics.Stopwatch]::StartNew()
  try { $p.Write($ESC + ']LB;' + $c + $BEL)
    while($sw.ElapsedMilliseconds -lt $Ms -and $r.IndexOf($BEL) -lt 0){ Start-Sleep -Milliseconds 20; $r += $p.ReadExisting() } } catch { $r += ' (port closed)' }
  $m=[regex]::Match($r, [regex]::Escape($ESC) + '\]LB;([^' + $BEL + ']*)' + $BEL)
  "$Port> $c | " + (($(if($m.Success){$m.Groups[1].Value}else{$r}) -replace "(\r?\n)+"," | ")) }
try { $p.Close() } catch {}
