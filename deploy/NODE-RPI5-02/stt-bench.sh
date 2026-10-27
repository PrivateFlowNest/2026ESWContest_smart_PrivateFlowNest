#!/usr/bin/env bash
# Speech-to-text speed on the real NODE-RPI5-02: the same Whisper model with several whisper.cpp options, so the
# service can use the fastest one that still transcribes the commands (2026-10-23: 13.8 s for one command).
#   bash deploy/NODE-RPI5-02/stt-bench.sh                 macOS: makes 5 Korean commands with `say -v Yuna`
#   bash deploy/NODE-RPI5-02/stt-bench.sh --clips DIR     16 kHz mono WAVs + DIR/refs.tsv (file<TAB>reference)
# Stops homemind-perception for the run (CPU to itself) and starts it again afterwards. Clips stay in /tmp on the
# Pi and are removed at the end. Synthetic voices are clean; real-room accuracy is checked separately.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
set -a; . "$here/../site.env"; set +a
. "$here/../common/payload.sh"
host="${RPI5_02_HOST:?RPI5_02_HOST in deploy/site.env}"
opts=(-o UserKnownHostsFile="$here/../.known_hosts" -o StrictHostKeyChecking=accept-new)
while read -r o; do opts+=("$o"); done < <(hm_ssh_shared_opts)
on() { ssh "${opts[@]}" "root@$host" "$@"; }

work="$(mktemp -d)"
paused=0
# perception runs again even when this script stops half-way
trap 'rm -rf "$work"; [[ $paused -eq 0 ]] || on "systemctl start homemind-perception" || true' EXIT
if [[ "${1:-}" == "--clips" ]]; then
  cp "$2"/*.wav "$2/refs.tsv" "$work/"
else
  command -v say >/dev/null && command -v afconvert >/dev/null || { echo "no macOS say/afconvert: use --clips DIR" >&2; exit 2; }
  say -v '?' | grep -q '^Yuna ' || { echo "Korean voice Yuna missing (System Settings > Accessibility > Spoken Content)" >&2; exit 2; }
  i=0
  while IFS= read -r text; do
    i=$((i + 1))
    say -v Yuna -o "$work/c$i.aiff" "$text"
    afconvert -f WAVE -d LEI16@16000 -c 1 "$work/c$i.aiff" "$work/c$i.wav"
    rm -f "$work/c$i.aiff"
    printf 'c%s.wav\t%s\n' "$i" "$text" >> "$work/refs.tsv"
  done <<'EOF'
거실 불 켜줘
텔레비전 틀어라
너무 더우니까 에어컨 켜
현관문 열어줘
창문 닫아줘
EOF
fi
echo "== clips: $(ls "$work"/*.wav | wc -l | tr -d ' ') (references in refs.tsv)"
COPYFILE_DISABLE=1 tar --format ustar -cf - -C "$work" . | on 'rm -rf /tmp/hm-stt-bench; mkdir -p /tmp/hm-stt-bench; tar -xf - -C /tmp/hm-stt-bench'

echo "== whisper-cli options available on the device"
on 'LD_LIBRARY_PATH=/data/share/homemind/lib /data/share/homemind/bin/whisper-cli --help 2>&1 | grep -E -- "--(audio-ctx|flash-attn|beam-size|best-of|threads) " | sed "s/^/  /"'

echo "== run (homemind-perception paused)"
on 'systemctl stop homemind-perception'
paused=1
# shellcheck disable=SC2016
on 'H=/data/share/homemind; M=$H/models/ggml-whisper-small-homemind-lora-v4-q5_0.bin; cd /tmp/hm-stt-bench
  run() { name=$1; shift; for w in c*.wav; do
      LD_LIBRARY_PATH=$H/lib $H/bin/whisper-cli -m $M -l ko -t 4 -nt "$@" -f $w > out.txt 2> err.txt || true
      ms=$(sed -n "s/.*total time = *\([0-9.]*\) ms.*/\1/p" err.txt | tail -n 1)
      printf "%s\t%s\t%s\t%s\n" "$name" "$w" "${ms:-NA}" "$(tr -d "\n" < out.txt | sed "s/^ *//")"
    done; }
  run beam5      -bs 5 -bo 5
  run greedy     -bs 1 -bo 1
  run greedy_fa  -bs 1 -bo 1 -fa
  run fa_ac768   -bs 1 -bo 1 -fa -ac 768
  run fa_ac512   -bs 1 -bo 1 -fa -ac 512' > "$work/results.tsv" || true
paused=0
on 'systemctl start homemind-perception; rm -rf /tmp/hm-stt-bench; sleep 3; printf "  homemind-perception "; systemctl is-active homemind-perception'

python3 - "$work/results.tsv" "$work/refs.tsv" <<'PY'
import collections, re, sys
refs = dict(line.rstrip("\n").split("\t", 1) for line in open(sys.argv[2], encoding="utf-8") if "\t" in line)
def norm(text): return re.sub(r"[\s.,!?~]", "", text)
def cer(ref, hyp):
    a, b = norm(ref), norm(hyp)
    row = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        prev, row[0] = row[0], i
        for j, cb in enumerate(b, 1):
            prev, row[j] = row[j], min(row[j] + 1, row[j - 1] + 1, prev + (ca != cb))
    return row[-1] / max(1, len(a))
by = collections.OrderedDict()
for line in open(sys.argv[1], encoding="utf-8"):
    parts = line.rstrip("\n").split("\t")
    if len(parts) == 4: by.setdefault(parts[0], []).append(parts[1:])
print("== results (lower is better)")
print("  %-10s %9s %7s  %s" % ("option", "avg ms", "CER", "transcripts"))
for name, rows in by.items():
    times = [float(ms) for _, ms, _ in rows if ms != "NA"]
    errors = [cer(refs.get(w, ""), text) for w, _, text in rows]
    avg = sum(times) / len(times) if times else float("nan")
    print("  %-10s %9.0f %6.1f%%  %s" % (name, avg, 100 * sum(errors) / len(errors), " | ".join(t for _, _, t in rows)))
PY
