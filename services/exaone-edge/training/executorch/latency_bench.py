"""Sequential per-request latency of an OpenAI-compatible endpoint on real client payloads (PC only).

    .venv/Scripts/python training/executorch/latency_bench.py --base-url http://127.0.0.1:8093 --model exaone-et \
        --n 20 --out <scratch>/lat-et.json

Payloads = production ExaoneClient._payload (with the guard's validated_local_candidate when it has one) for
the first N cases of the 100-sentence set; one warm-up request first (fills the shared-prefix cache).
Wall time per request as the client sees it, plus server timings when the server reports them.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from parity_check import _payloads  # noqa: E402


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--base-url", required=True)
    p.add_argument("--model", required=True)
    p.add_argument("--n", type=int, default=20)
    p.add_argument("--out", required=True)
    args = p.parse_args()
    items = [x for x in _payloads() if x["variant"] == "first"][: args.n + 1]
    rows = []
    for i, item in enumerate(items):
        body = dict(item["payload"], model=args.model)
        req = urllib.request.Request(args.base_url.rstrip("/") + "/v1/chat/completions",
                                     data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
                                     headers={"Content-Type": "application/json"})
        t = time.perf_counter()
        resp = json.loads(urllib.request.urlopen(req, timeout=600).read().decode("utf-8"))
        ms = (time.perf_counter() - t) * 1000
        rows.append({"id": item["id"], "warmup": i == 0, "wall_ms": round(ms, 1),
                     "completion_tokens": resp.get("usage", {}).get("completion_tokens"),
                     "timings": resp.get("timings"), "content": resp["choices"][0]["message"]["content"]})
    measured = [r["wall_ms"] for r in rows if not r["warmup"]]
    q = statistics.quantiles(measured, n=20)
    summary = {"base_url": args.base_url, "n": len(measured), "p50_ms": round(statistics.median(measured), 1),
               "p95_ms": round(q[18], 1), "max_ms": max(measured), "warmup_ms": rows[0]["wall_ms"]}
    Path(args.out).write_text(json.dumps({"summary": summary, "rows": rows}, ensure_ascii=False, indent=1),
                              encoding="utf-8")
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
