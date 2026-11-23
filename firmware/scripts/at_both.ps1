param([string[]]$Cmds)
foreach($n in 'COM3','COM4'){ $p = New-Object System.IO.Ports.SerialPort $n,115200; $p.DtrEnable=$true; $p.RtsEnable=$true; $p.NewLine="`r`n"; $p.Open(); Start-Sleep -Milliseconds 1200; $p.Write('+++'); Start-Sleep -Milliseconds 1300
 foreach($c in ($Cmds -join ";").Split(";")){ $p.WriteLine($c); Start-Sleep -Milliseconds 600; "$n> " + (($p.ReadExisting() -replace "(\r?\n)+"," | ")) }
 $p.WriteLine('ATO'); Start-Sleep -Milliseconds 300; $p.Close() }

