#!/bin/zsh
# Render FrameLift's UI with demo data on a Windows test PC (desktop session, tools/testpc.env) and copy the PNGs back.
# usage: uishot.sh <out dir on Mac> "<name> <args...>" ["<name> <args...>" ...]
# e.g.   uishot.sh /tmp/ui "live --advanced" "fs --fullscreen --nodash --size 1920x1080"
set -e
source "${0:A:h}/testpc.env" || { echo 'create tools/testpc.env (see testpc.env.example)'; exit 1; }
out=$1; shift
H=($FL_SSH)
scp -q $FL_SCP_OPTS build-mingw/FrameLift.exe "$FL_SCP_HOST:C:/FrameLift/bench/FrameLift.exe"
frame=${FL_FRAME:-'C:\FrameLift\bench\demo.png'}
cmds=""
for spec in "$@"; do
  name=${spec%% *}; args=${spec#* }; [[ "$args" == "$name" ]] && args=""
  cmds+="& 'C:\\FrameLift\\bench\\FrameLift.exe' --uishot 'C:\\FrameLift\\bench\\ui-$name.png' --frame '$frame' $args | Out-Null; "
done
ps=$(mktemp)
cat > $ps <<PS
Remove-Item -ErrorAction SilentlyContinue 'C:\FrameLift\bench\ui-*.png'
Set-Content -Path 'C:\FrameLift\bench\uishot.ps1' -Value "$cmds Set-Content C:\FrameLift\bench\uishot.done ok"
Remove-Item -ErrorAction SilentlyContinue 'C:\FrameLift\bench\uishot.done'
\$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\FrameLift\bench\uishot.ps1' -WorkingDirectory 'C:\FrameLift\bench'
\$p = New-ScheduledTaskPrincipal -UserId '$FL_USER' -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'FrameLiftUiShot' -Action \$a -Principal \$p -Force | Out-Null
Start-ScheduledTask -TaskName 'FrameLiftUiShot'
PS
B=$(iconv -t UTF-16LE $ps | base64 | tr -d '\n'); rm -f $ps
ssh $H "powershell -NoProfile -EncodedCommand $B" >/dev/null 2>&1
for i in $(seq 1 60); do ssh $H "if exist C:\\FrameLift\\bench\\uishot.done echo done" 2>/dev/null | grep -q done && break; sleep 2; done
mkdir -p $out
scp -q $FL_SCP_OPTS "$FL_SCP_HOST:C:/FrameLift/bench/ui-*.png" $out/ && ls $out
