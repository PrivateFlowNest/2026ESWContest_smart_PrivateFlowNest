#!/usr/bin/env bash
# Installed-layout smoke test of homemind-rpi5-llm (QEMU arm64 emulation on the PC — not a Pi).
# Unpacks the IPK data into / of a clean arm64 Ubuntu 24.04 container, keeps only the system libraries the
# device is assumed to have, points the packaged config at the PC demo broker and the PC llama-server,
# runs the packaged unit's command line, and answers one llm/request.
#   docker run --rm --platform linux/arm64 --add-host=host.docker.internal:host-gateway -v "<repo>:/w" \
#     ubuntu:24.04 bash /w/native/packaging/test/smoke-rpi5-llm.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends binutils mosquitto-clients ca-certificates >/dev/null
ipk="$(ls /w/native/dist-ipk/homemind-rpi5-llm_*_aarch64.ipk | sort -V | tail -1)"
work=/tmp/ipk && mkdir -p $work && cd $work && ar x "$ipk" && tar -xzf data.tar.gz -C /
# mosquitto-clients (test tools only) pull a system libmosquitto; ldd below shows the service loads the bundled one.
HM=/data/share/homemind
mkdir -p $HM/config && cp $HM/package-defaults/rpi5-01.json $HM/config/rpi5-01.json
# PC demo network: anonymous broker on 1883, llama-server on the PC (127.0.0.1:8080 via host.docker.internal).
sed -i -e 's/"host": "BROKER_HOST"/"host": "host.docker.internal"/' -e 's/"port": [0-9]*/"port": 1883/' \
  -e 's/"username": "NODE-RPI5-01"/"username": null/' -e 's|"password_file": "[^"]*"|"password_file": null|' \
  -e 's|http://127.0.0.1:8080|http://host.docker.internal:8080|' $HM/config/rpi5-01.json
echo "== ldd (bundled libs from $HM/lib)"
LD_LIBRARY_PATH=$HM/lib ldd $HM/bin/homemind-llm | grep -E "mosquitto|not found" || true
cmd="$(sed -n 's/^ExecStart=//p' /data/share/usr/lib/systemd/system/homemind-llm.service)"
echo "== run: $cmd"
LD_LIBRARY_PATH=$HM/lib $cmd --log-level INFO > /tmp/llm.log 2>&1 &
pid=$!
sleep 3
now=$(date +%s%3N)
uuid() { cat /proc/sys/kernel/random/uuid; }
trace=$(uuid)
mosquitto_sub -h host.docker.internal -p 1883 -t pfn/v1/ai/llm/result -C 1 -W 60 > /tmp/result.json &
sub=$!
sleep 1
mosquitto_pub -h host.docker.internal -p 1883 -q 1 -t pfn/v1/ai/llm/request -m "{\"schema_version\":\"pfn.v1\",\"event_id\":\"$(uuid)\",\"trace_id\":\"$trace\",\"request_id\":\"$(uuid)\",\"source_node_id\":\"NODE-RPI5-02\",\"utterance_id\":\"$(uuid)\",\"transcript\":{\"text\":\"아 좀 덥네\",\"language\":\"ko\",\"stt_model_version\":\"smoke\",\"stt_confidence\":null},\"parser\":{\"result\":\"NO_MATCH\",\"rule_version\":\"guardrails-2026-10-22\"},\"requested_at_ms\":$now,\"expires_at_ms\":$((now + 60000)),\"simulated\":true}"
wait $sub || true
kill $pid 2>/dev/null || true
echo "== log"; cat /tmp/llm.log
echo "== result"; cat /tmp/result.json; echo
grep -q "\"trace_id\":\"$trace\"" /tmp/result.json && grep -q '"status":"task_requested"' /tmp/result.json \
  && ! grep -q "아 좀 덥네" /tmp/llm.log && echo SMOKE_RPI5_LLM_PASS || { echo SMOKE_RPI5_LLM_FAIL; exit 1; }
