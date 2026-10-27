#!/usr/bin/env bash
# Extra whisper-server options on the real NODE-RPI5-02. stt-bench.sh on 2026-10-23: -ac 512 (audio context of
# ~10 s instead of 30 s) took 6.1 s per command instead of 12 s, with the same transcripts. RUN ONLY AFTER THE USER
# APPROVES.
#   bash deploy/NODE-RPI5-02/tune-whisper.sh                 HM_WHISPER_EXTRA_ARGS="-ac 512"
#   bash deploy/NODE-RPI5-02/tune-whisper.sh "-ac 768"
# Sets HM_WHISPER_EXTRA_ARGS in config/whisper.env and an /etc drop-in that passes it to whisper-server (the 0.1.0
# unit has no such variable; later packages do), restarts homemind-whisper and shows its running command line.
# Keep vad.max_ms (8 s) shorter than the audio context: 512 covers 10.24 s.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
set -a; . "$here/../site.env"; set +a
. "$here/../common/payload.sh"
host="${RPI5_02_HOST:?RPI5_02_HOST in deploy/site.env}"
opts=(-o UserKnownHostsFile="$here/../.known_hosts" -o StrictHostKeyChecking=accept-new)
while read -r o; do opts+=("$o"); done < <(hm_ssh_shared_opts)
on() { ssh "${opts[@]}" "root@$host" "$@"; }
args="${1:--ac 512}"
[[ "$args" =~ ^[-a-z0-9\ ]+$ ]] || { echo "options: letters, digits, '-' and spaces only" >&2; exit 2; }
H=/data/share/homemind

on "LD_LIBRARY_PATH=$H/lib $H/bin/whisper-server --help 2>&1 | grep -q -- '--audio-ctx'" \
  || { echo "whisper-server on the device has no --audio-ctx" >&2; exit 1; }
echo "== 1. $H/config/whisper.env: HM_WHISPER_EXTRA_ARGS=\"$args\""
on "cd $H/config && if grep -q '^HM_WHISPER_EXTRA_ARGS=' whisper.env; then \
  sed -i 's/^HM_WHISPER_EXTRA_ARGS=.*/HM_WHISPER_EXTRA_ARGS=\"$args\"/' whisper.env; \
  else printf 'HM_WHISPER_EXTRA_ARGS=\"%s\"\n' '$args' >> whisper.env; fi; grep '^HM_WHISPER' whisper.env | sed 's/^/  /'"

echo "== 2. /etc drop-in that passes it to whisper-server"
on "$(hm_etc_write_cmd /etc/systemd/system/homemind-whisper.service.d/20-extra-args.conf)" <<EOF
[Service]
ExecStart=
ExecStart=/bin/sh -c 'exec $H/bin/whisper-server -m "\$HM_WHISPER_MODEL" --host 127.0.0.1 --port 8090 -t "\$HM_WHISPER_THREADS" -l ko \$HM_WHISPER_EXTRA_ARGS'
EOF

echo "== 3. restart homemind-whisper"
on "systemctl daemon-reload; systemctl restart homemind-whisper; sleep 4; printf '  '; systemctl is-active homemind-whisper; \
  pid=\$(pidof whisper-server); [ -n \"\$pid\" ] && printf '  running: %s\n' \"\$(tr '\0' ' ' < /proc/\$pid/cmdline)\""
