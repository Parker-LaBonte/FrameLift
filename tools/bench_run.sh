#!/bin/zsh
# Run the FrameLift GPU benchmark in a Windows test PC's desktop session (tools/testpc.env) (needed for D3D11<->CUDA interop).
# usage: bench_run.sh <configs file on Mac> <out name> [stride] [clips dir on PC]
set -e
source "${0:A:h}/testpc.env" || { echo 'create tools/testpc.env (see testpc.env.example)'; exit 1; }
cfg=$1; out=$2; stride=${3:-2}; clips=${4:-C:\\FrameLift\\bench\\clips}
H=($FL_SSH)
scp -q $FL_SCP_OPTS "$cfg" "$FL_SCP_HOST:C:/FrameLift/bench/$out.cfg"
ps=$(mktemp)
cat > $ps <<PS
\$a = New-ScheduledTaskAction -Execute 'C:\FrameLift\bench\FrameLift.exe' -Argument '--bench $clips --configs C:\FrameLift\bench\\$out.cfg --out C:\FrameLift\bench\\$out.csv --stride $stride' -WorkingDirectory 'C:\FrameLift\bench'
\$p = New-ScheduledTaskPrincipal -UserId '$FL_USER' -LogonType Interactive -RunLevel Limited
\$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName 'FrameLiftBench' -Action \$a -Principal \$p -Settings \$s -Force | Out-Null
Remove-Item -ErrorAction SilentlyContinue 'C:\FrameLift\bench\\$out.csv'
Start-ScheduledTask -TaskName 'FrameLiftBench'
PS
B=$(iconv -t UTF-16LE $ps | base64 | tr -d '\n'); rm -f $ps
ssh $H "powershell -NoProfile -EncodedCommand $B" >/dev/null 2>&1
sleep 3
while ssh $H "tasklist /fi \"imagename eq FrameLift.exe\" /v" 2>/dev/null | grep -q -- "--bench\|FrameLiftBench" ; do sleep 5; done
# fall back: wait until the CSV has its MEAN lines (the bench process isn't distinguishable by tasklist)
for i in $(seq 1 720); do ssh $H "type C:\\FrameLift\\bench\\$out.csv" 2>/dev/null | grep -q '^MEAN' && break; sleep 5; done
ssh $H "type C:\\FrameLift\\bench\\$out.csv" 2>/dev/null | grep '^MEAN' | awk -F, 'NR==1{b=$3;bm=$4;bs=$5;bw=$6} {printf "%-14s all %.3f (%+.3f)  moving %.3f (%+.3f)  ssim %.4f (%+.4f)  worst5 %.3f (%+.3f)\n",$2,$3,$3-b,$4,$4-bm,$5,$5-bs,$6,$6-bw}'
