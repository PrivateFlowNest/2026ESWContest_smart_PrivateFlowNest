"""Prompt parity between llama-server (reference for the llama.cpp v3 numbers) and et_server.py.

Step 1 (Windows, service .venv, llama-server with the v3 GGUF running):
    .venv/Scripts/python training/executorch/parity_check.py fetch --base-url http://127.0.0.1:8090 \
        --out <scratch>/llama-prompts.json
  Builds real client payloads (production ExaoneClient._payload, with and without the guard's
  validated_local_candidate and with retry feedback) for every case in the three acceptance sets and
  stores llama-server's /apply-template prompt and /tokenize ids.

Step 2 (WSL, ~/venvs/homemind-executorch):
    python parity_check.py compare --tokenizer <merged-v3 dir> --ref <scratch>/llama-prompts.json
  Renders the same payloads the way et_server.py does (llama-compat and hf modes) and reports
  how many are token-identical to llama-server.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

SERVICE = Path(__file__).resolve().parents[2]
SETS = ("acceptance-cases-v1", "acceptance-independent-v1", "acceptance-independent-v2")


def _payloads() -> list[dict]:
    sys.path.insert(0, str(SERVICE / "src"))
    from homemind_exaone.client import ExaoneClient  # noqa: E402
    from homemind_exaone.guardrails import preflight_proposal  # noqa: E402
    from homemind_exaone.validation import validate_activity_context, validate_proposal  # noqa: E402

    client = ExaoneClient()
    out = []
    for name in SETS:
        for line in (SERVICE / "data" / f"{name}.jsonl").read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            case = json.loads(line)
            ctx = validate_activity_context(case.get("context"))
            candidate = None
            guarded = preflight_proposal(case["utterance"], client.capabilities, ctx)
            if guarded is not None:
                try:
                    proposal = validate_proposal(guarded, schema=client.schema, capabilities=client.capabilities)
                    if proposal["intent"] in {"DEVICE_CONTROL", "DEVICE_QUERY", "CONTEXT_SUGGESTION"}:
                        candidate = proposal
                except Exception:
                    pass
            out.append({"id": case["id"], "variant": "first",
                        "payload": client._payload(case["utterance"].strip(), ctx, None, candidate)})
            if len(out) % 10 == 0:  # some retry-shaped prompts too
                out.append({"id": case["id"], "variant": "feedback",
                            "payload": client._payload(case["utterance"].strip(), ctx, ["invalid JSON: x"],
                                                       candidate)})
    return out


def fetch(args: argparse.Namespace) -> int:
    import urllib.request

    def post(path: str, body: dict) -> dict:
        req = urllib.request.Request(args.base_url.rstrip("/") + path,
                                     data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
                                     headers={"Content-Type": "application/json"})
        return json.loads(urllib.request.urlopen(req, timeout=120).read().decode("utf-8"))

    rows = []
    for item in _payloads():
        p = item["payload"]
        prompt = post("/apply-template", {"messages": p["messages"],
                                          "chat_template_kwargs": p["chat_template_kwargs"]})["prompt"]
        tokens = post("/tokenize", {"content": prompt, "add_special": True})["tokens"]
        rows.append({**item, "prompt": prompt, "tokens": tokens})
    Path(args.out).write_text(json.dumps(rows, ensure_ascii=False), encoding="utf-8")
    print(f"fetched {len(rows)} prompts -> {args.out}")
    return 0


def compare(args: argparse.Namespace) -> int:
    from transformers import AutoTokenizer

    tok = AutoTokenizer.from_pretrained(args.tokenizer)
    rows = json.loads(Path(args.ref).read_text(encoding="utf-8"))
    result = {}
    for mode in ("llama-compat", "hf"):
        same_text = same_ids = 0
        lengths = []
        for r in rows:
            msgs = r["payload"]["messages"]
            if mode == "llama-compat":
                msgs = [m for m in msgs if m["role"] != "system"]
            text = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=True,
                                           **r["payload"]["chat_template_kwargs"])
            ids = tok(text, add_special_tokens=False)["input_ids"]
            same_text += text == r["prompt"]
            same_ids += list(ids) == r["tokens"]
            lengths.append(len(ids))
        result[mode] = {"prompts": len(rows), "text_identical": same_text, "token_identical": same_ids,
                        "tokens_min": min(lengths), "tokens_max": max(lengths)}
    result["llama_server_tokens_max"] = max(len(r["tokens"]) for r in rows)
    print(json.dumps(result, indent=1))
    return 0


def main() -> int:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--base-url", required=True)
    f.add_argument("--out", required=True)
    c = sub.add_parser("compare")
    c.add_argument("--tokenizer", required=True)
    c.add_argument("--ref", required=True)
    args = p.parse_args()
    return fetch(args) if args.cmd == "fetch" else compare(args)


if __name__ == "__main__":
    raise SystemExit(main())
