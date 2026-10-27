#!/usr/bin/env bash
# Installed-layout smoke test of homemind-rpi5-perception (QEMU arm64 emulation on the PC — not a Pi).
# Unpacks the IPK into a clean arm64 Ubuntu 24.04, copies the Whisper model to the packaged path, starts the
# packaged whisper-server and runs the packaged homemind-perception (bundled libs only) on real clips in file mode:
#   /t/cmd.wav      a spoken command  -> one pfn.v1 task request (voice parser)
#   /t/meeting.wav  meeting audio     -> ACTIVATED perception with quiet_required (context path, YAMNet ONNX)
#   docker run --rm --platform linux/arm64 -v "<repo>:/w" -v "<clips>:/t" ubuntu:24.04 \
#     bash /w/native/packaging/test/smoke-rpi5-perception.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends binutils curl ca-certificates >/dev/null
ipk="$(ls /w/native/dist-ipk/homemind-rpi5-perception_*_aarch64.ipk | sort -V | tail -1)"
mkdir -p /tmp/ipk && cd /tmp/ipk && ar x "$ipk" && tar -xzf data.tar.gz -C /
HM=/data/share/homemind
mkdir -p $HM/config $HM/models
cp $HM/package-defaults/rpi5-02.json $HM/config/rpi5-02.json
cp /w/services/edge-ai/speech-to-text/artifacts/training/whisper-small-homemind-lora-v4/ggml-whisper-small-homemind-lora-v4-q5_0.bin $HM/models/
# file mode prints to stdout; no broker needed
# QEMU runs whisper ~50x slower than a Pi 5 would, so the STT time limit is raised for this test only.
sed -i -e 's/"publish": "mqtt"/"publish": "stdout"/' -e 's/"stt_timeout_seconds": 60.0/"stt_timeout_seconds": 900.0/' -e 's/"username": "NODE-RPI5-02"/"username": null/' \
  -e 's|"password_file": "[^"]*"|"password_file": null|' $HM/config/rpi5-02.json
echo "== ldd (system libs this container lacks show as 'not found')"
LD_LIBRARY_PATH=$HM/lib ldd $HM/bin/homemind-perception | grep -E "onnxruntime|mosquitto|asound|not found" || true
LD_LIBRARY_PATH=$HM/lib ldd $HM/bin/homemind-vision | grep -E "onnxruntime|jpeg|png|systemd|not found" || true
# whisper-server exactly as the unit starts it (env file values)
set -a; . $HM/package-defaults/whisper.env; set +a
LD_LIBRARY_PATH=$HM/lib $HM/bin/whisper-server -m "$HM_WHISPER_MODEL" --host 127.0.0.1 --port 8090 -t "$HM_WHISPER_THREADS" -l ko \
  > /tmp/whisper.log 2>&1 &
for i in $(seq 1 120); do curl -s -m 2 http://127.0.0.1:8090/ >/dev/null 2>&1 && break; sleep 2; done
run() { LD_LIBRARY_PATH=$HM/lib $HM/bin/homemind-perception --config $HM/config/rpi5-02.json "$@"; }
echo "== command clip"
start=$(date +%s)
run --wav /t/cmd.wav --voice whole --fast > /tmp/cmd.out 2> /tmp/cmd.err || { cat /tmp/cmd.err; exit 1; }
grep -iE "stt|whisper|timeout|error" /tmp/cmd.err | head -5
echo "   $(( $(date +%s) - start )) s (QEMU)"
grep -c "pfn/v1/core/task/request" /tmp/cmd.out && grep "pfn/v1/core/task/request" /tmp/cmd.out | head -1 | cut -c1-400
echo "== meeting clip"
start=$(date +%s)
run --wav /t/meeting.wav --voice off --fast > /tmp/meeting.out 2> /tmp/meeting.err || { cat /tmp/meeting.err; exit 1; }
echo "   $(( $(date +%s) - start )) s (QEMU)"
grep "pfn/v1/ai/perception" /tmp/meeting.out | grep -o '"transition":"[A-Z]*"' | sort | uniq -c
vision_ok=1
if [[ -d /t/vision/empty ]]; then  # optional: synthetic one-axis scene from native/tests/test_vision_background.py
  echo "== vision (detector background, packaged zones.rail example)"
  cp $HM/share/vision/zones.rail.json.example $HM/config/zones.rail.json
  cat > $HM/config/vision-smoke.json <<EOF
{"base_dir": "$HM/config", "broker": {"host": "127.0.0.1", "port": 1883}, "contracts_dir": "$HM/share/contracts",
 "perception_node_id": "NODE-RPI5-02", "simulated": true,
 "vision": {"detector": "background", "republish_seconds": 0, "presence_fifo": null,
            "cameras": [{"camera_id": "CAM-INDOOR-01", "zones": "zones.rail.json",
                         "ignore": [[[0.455, 0.57], [0.52, 0.57], [0.52, 0.66], [0.455, 0.66]]],
                         "source": {"type": "images", "path": "/t/vision/route"}}]}}
EOF
  vrun() { LD_LIBRARY_PATH=$HM/lib $HM/bin/homemind-vision --config $HM/config/vision-smoke.json "$@"; }
  vrun --capture-background --images /t/vision/empty 2> /tmp/vision.err || { cat /tmp/vision.err; exit 1; }
  vrun --stdout --log-level WARNING > /tmp/vision.out 2>> /tmp/vision.err || { cat /tmp/vision.err; exit 1; }
  route="$(grep '"present":true' /tmp/vision.out | grep -o '"zone":"[a-z]*"' | uniq | tr '\n' ' ')"
  echo "   zones while present: $route"
  [[ "$route" == '"zone":"entrance" "zone":"bed" "zone":"bathroom" ' ]] || vision_ok=0
fi
grep -q '"intent":"device_control"' /tmp/cmd.out \
  && grep "pfn/v1/ai/perception" /tmp/meeting.out | grep '"transition":"ACTIVATED"' | grep -q '"quiet_required":true' \
  && [[ $vision_ok == 1 ]] \
  && echo SMOKE_RPI5_PERCEPTION_PASS || { echo SMOKE_RPI5_PERCEPTION_FAIL; tail -n 20 /tmp/cmd.err /tmp/meeting.err; exit 1; }
