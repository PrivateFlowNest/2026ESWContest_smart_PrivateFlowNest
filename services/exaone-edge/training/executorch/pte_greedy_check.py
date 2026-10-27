"""Functional check of the .pte: fixed prompt token ids -> greedy token ids (no tokenizer needed).

Used to compare x86_64 (WSL) with aarch64 (Docker --platform linux/arm64, QEMU) on the same .pte.
QEMU timings are functional only; never quote them as device latency.

    python pte_greedy_check.py --pte model.pte --prompt-json prompt.json --steps 64 --out result.json
prompt.json: {"tokens": [...], "eos": [361]}
"""

from __future__ import annotations

import argparse
import json
import platform
import time

import torch


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--pte", required=True)
    p.add_argument("--prompt-json", required=True)
    p.add_argument("--steps", type=int, default=64)
    p.add_argument("--chunk", type=int, default=256)
    p.add_argument("--out", required=True)
    p.add_argument("--threads", type=int, default=0)
    args = p.parse_args()

    from executorch.extension.pybindings import portable_lib  # load first (see et_server.py)
    from executorch.kernels import quantized  # noqa: F401
    from executorch.extension.llm.custom_ops import custom_ops  # noqa: F401

    if args.threads:
        portable_lib._unsafe_reset_threadpool(args.threads)

    spec = json.load(open(args.prompt_json, encoding="utf-8"))
    tokens, eos = list(spec["tokens"]), set(spec.get("eos", [361]))
    t0 = time.perf_counter()
    module = portable_lib._load_for_executorch(args.pte)
    t1 = time.perf_counter()

    def run(ids: list[int], start: int) -> torch.Tensor:
        out = module.forward((torch.tensor([ids], dtype=torch.long),
                              torch.arange(start, start + len(ids), dtype=torch.long)))[0]
        return out[0, -1] if out.dim() == 3 else out.reshape(-1, out.shape[-1])[-1]

    pos, logits = 0, None
    while pos < len(tokens):
        chunk = tokens[pos:pos + args.chunk]
        logits = run(chunk, pos)
        pos += len(chunk)
    t2 = time.perf_counter()
    generated: list[int] = []
    for _ in range(args.steps):
        nxt = int(torch.argmax(logits).item())
        generated.append(nxt)
        if nxt in eos:
            break
        logits = run([nxt], pos)
        pos += 1
    t3 = time.perf_counter()
    result = {
        "machine": platform.machine(), "python": platform.python_version(),
        "threads": int(portable_lib._threadpool_get_thread_count()),
        "max_seq_len": int(module.run_method("get_max_seq_len")[0]),
        "prompt_tokens": len(tokens), "generated": generated,
        "load_s": round(t1 - t0, 3), "prefill_s": round(t2 - t1, 3), "decode_s": round(t3 - t2, 3),
    }
    json.dump(result, open(args.out, "w", encoding="utf-8"), indent=1)
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
