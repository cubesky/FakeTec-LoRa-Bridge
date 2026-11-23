# FAST mode (2.4 GHz) test on two bridges: negotiate, measure the throughput in both
# directions, then switch back to LoRa in the middle of a transfer and check that no byte
# is lost or repeated.   .\fast_test.ps1 [-PortA COM3] [-PortB COM4] [-N 200000]
param([string]$PortA='COM3',[string]$PortB='COM4',[int]$N=200000)
function Open($n){ $p = New-Object System.IO.Ports.SerialPort $n,115200; $p.DtrEnable=$true; $p.RtsEnable=$true; $p.NewLine="`r`n"; $p.ReadBufferSize=1048576; $p.WriteTimeout=60000; $p.Open(); $p }
function Cmd($p,$c,$ms=400){ $p.WriteLine($c); Start-Sleep -Milliseconds $ms; ($p.ReadExisting() -replace "(\r?\n)+"," | ") }
function CmdMode(){ Start-Sleep -Milliseconds 1200; foreach($p in $a,$b){ $p.Write('+++') }; Start-Sleep -Milliseconds 1300; $null=$a.ReadExisting(); $null=$b.ReadExisting() }
function DataMode(){ foreach($p in $a,$b){ $null=Cmd $p 'ATO' 300; $null=$p.ReadExisting() } }
function Mk($tag,$n){ $sb=New-Object Text.StringBuilder; $i=0; while($sb.Length -lt $n){ [void]$sb.Append(("{0}{1:D7};" -f $tag,$i)); $i++ }; $sb.ToString().Substring(0,$n) }
function Up(){ $r=Cmd $a 'AT+FAST=1'; for($i=0;$i -lt 12;$i++){ Start-Sleep -Milliseconds 500; $r=Cmd $a 'AT+FAST?' 200; if($r -match 'FAST=2' -and (Cmd $b 'AT+FAST?' 200) -match 'FAST=2'){ return $true } }; Write-Host "FAST not up: $r"; return $false }
function Pump($pairs,$tmo){
  $sw=[Diagnostics.Stopwatch]::StartNew()
  while($sw.Elapsed.TotalSeconds -lt $tmo){
    $done=$true; $writing=$false
    foreach($x in $pairs){
      if($x.off -lt $x.data.Length){ $k=[Math]::Min(2048,$x.data.Length-$x.off); $x.src.Write($x.data.Substring($x.off,$k)); $x.off+=$k; $writing=$true }
      [void]$x.sb.Append($x.dst.ReadExisting())
      if($x.sb.Length -lt $x.data.Length){ $done=$false }
    }
    if($done){break}
    if(-not $writing){ Start-Sleep -Milliseconds 5 }
  }
  $sw.Elapsed.TotalSeconds
}
function Job($src,$dst,$tag,$n){ @{src=$src;dst=$dst;data=(Mk $tag $n);sb=(New-Object Text.StringBuilder);off=0} }
function Res($x,$t){ "{0}/{1} bytes in {2:N1}s = {3:N0} B/s, intact={4}" -f $x.sb.Length,$x.data.Length,$t,($x.sb.Length/$t),($x.sb.ToString() -ceq $x.data) }
$a=Open $PortA; $b=Open $PortB; Start-Sleep -Milliseconds 300; CmdMode
for($i=0;$i -lt 25;$i++){ $r=Cmd $a 'AT+ST' 300; if($r -match 'ST=LINK'){break}; Start-Sleep 1 }
if($r -notmatch 'ST=LINK'){ "LoRa link not up: $r"; $a.Close(); $b.Close(); exit 1 }
if(-not (Up)){ $a.Close(); $b.Close(); exit 1 }
"FAST up: " + (Cmd $a 'AT+FAST?' 200)
DataMode
$x=Job $a $b 'A' $N; "A->B     : " + (Res $x (Pump @($x) 120))
$x=Job $b $a 'B' $N; "B->A     : " + (Res $x (Pump @($x) 120))
$x=Job $a $b 'X' $N; $y=Job $b $a 'Y' $N; $t=Pump @($x,$y) 240; "both A->B: " + (Res $x $t); "both B->A: " + (Res $y $t)
# back to LoRa in the middle of a two-way transfer (in-band command, no +++ needed)
$M=14000; $x=Job $a $b 'P' $M; $y=Job $b $a 'Q' $M; $sw=[Diagnostics.Stopwatch]::StartNew(); $rel=$false
while($sw.Elapsed.TotalSeconds -lt 240 -and ($x.sb.Length -lt $M -or $y.sb.Length -lt ($M+9))){
  if($x.off -lt $M){ $k=[Math]::Min(1000,$M-$x.off); $a.Write($x.data.Substring($x.off,$k)); $b.Write($y.data.Substring($x.off,$k)); $x.off+=$k
    if(-not $rel -and $x.off -ge 7000){ $a.Write("`e]LB;AT+FAST=0`a"); $rel=$true } } else { Start-Sleep -Milliseconds 50 }
  [void]$x.sb.Append($b.ReadExisting()); [void]$y.sb.Append($a.ReadExisting())
}
$ya=($y.sb.ToString() -replace "`e\]LB;[^`a]*`a",'')
"switch back to LoRa mid-transfer ({0:N0}s): A->B {1}/{2} intact={3}, B->A {4}/{2} intact={5}" -f $sw.Elapsed.TotalSeconds,$x.sb.Length,$M,($x.sb.ToString() -ceq $x.data),$ya.Length,($ya -ceq $y.data)
CmdMode
"A: " + (Cmd $a 'AT+FAST?' 300); "B: " + (Cmd $b 'AT+FAST?' 300)
DataMode; $a.Close(); $b.Close()
