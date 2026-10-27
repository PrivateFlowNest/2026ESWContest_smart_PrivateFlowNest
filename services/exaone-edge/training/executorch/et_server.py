"""Minimal OpenAI-compatible server around an ExecuTorch EXAONE .pte (plan 2 trial, PC simulation).

It implements only what the production client (src/homemind_exaone/client.py) uses:

    GET  /health                 -> {"status": "ok"}
    GET  /v1/models
    POST /v1/chat/completions    -> greedy decoding, max_tokens, response_format json_schema

Decoding mirrors llama-server as used for the llama.cpp v3 measurements:
  * prompt = the model's chat template (chat_template.jinja of the merged v3 checkpoint) with
    chat_template_kwargs from the request, no BOS (EXAONE template / add_bos_token=False);
  * --prompt-mode llama-compat (default) reproduces llama-server b10588, whose Jinja renderer
    drops the leading system turn of this template (verified token-identical with
    /apply-template + /tokenize, see parity_check.py); --prompt-mode hf renders the template
    with Hugging Face Jinja2 including the system prompt (the format the LoRA was trained on);
  * temperature is ignored: always greedy (the client sends 0.0);
  * response_format json_schema is enforced with xgrammar token masks (llama-server converts
    the same schema to a GBNF grammar); --no-grammar disables it;
  * the KV cache keeps the previous prompt, so a shared prefix (system prompt + few-shots)
    is not re-evaluated (same idea as llama-server cache_prompt).

Run inside WSL with ~/venvs/homemind-executorch:
    python et_server.py --pte <model.pte> --tokenizer <merged-v3 dir> --port 8093
"""

from __future__ import annotations

import argparse
import json
import logging
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import torch

LOG = logging.getLogger("et_server")


class PteLM:
    """ExecuTorch program exported by optimum-executorch (forward(input_ids, cache_position) -> logits)."""

    def __init__(self, pte_path: str, threads: int | None, prefill_chunk: int) -> None:
        # portable_lib must be loaded first: the kernel .so files of the executorch 1.3.1 wheel resolve
        # _portable_lib by soname (their RUNPATH points at a non-existent "extensions/pybindings").
        from executorch.extension.pybindings import portable_lib
        from executorch.kernels import quantized  # noqa: F401  (quantized_decomposed::embedding_byte etc.)
        from executorch.extension.llm.custom_ops import custom_ops  # noqa: F401  (custom_sdpa, update_cache)

        if threads:
            portable_lib._unsafe_reset_threadpool(int(threads))
        started = time.perf_counter()
        self.module = portable_lib._load_for_executorch(pte_path)
        self.load_s = time.perf_counter() - started
        self.max_seq_len = int(self.module.run_method("get_max_seq_len")[0])
        meta = self.module.method_meta("forward")
        self.dynamic_prefill = int(meta.input_tensor_meta(1).sizes()[0]) > 1
        self.prefill_chunk = max(1, int(prefill_chunk)) if self.dynamic_prefill else 1
        self.threads = int(portable_lib._threadpool_get_thread_count())
        self.cached: list[int] = []  # tokens whose K/V are valid at positions 0..len-1

    def _run(self, tokens: list[int], start: int) -> torch.Tensor:
        ids = torch.tensor([tokens], dtype=torch.long)
        pos = torch.arange(start, start + len(tokens), dtype=torch.long)
        logits = self.module.forward((ids, pos))[0]
        if logits.dim() == 3:
            logits = logits[0, -1]
        elif logits.dim() == 2:
            logits = logits[-1]
        return logits.float()

    def prefill(self, prompt: list[int]) -> tuple[torch.Tensor, int]:
        """Evaluate prompt reusing the cached prefix. Returns (last logits, reused token count)."""
        if len(prompt) >= self.max_seq_len:
            raise ValueError(f"prompt has {len(prompt)} tokens, max_seq_len is {self.max_seq_len}")
        common = 0
        for a, b in zip(self.cached, prompt):
            if a != b:
                break
            common += 1
        common = min(common, len(prompt) - 1)  # always feed at least one token to get logits
        logits = None
        pos = common
        while pos < len(prompt):
            chunk = prompt[pos:pos + self.prefill_chunk]
            logits = self._run(chunk, pos)
            pos += len(chunk)
        self.cached = list(prompt)
        assert logits is not None
        return logits, common

    def step(self, token: int) -> torch.Tensor:
        pos = len(self.cached)
        if pos >= self.max_seq_len:
            raise ValueError("KV cache full")
        logits = self._run([token], pos)
        self.cached.append(token)
        return logits


class Grammar:
    def __init__(self, hf_tokenizer: Any, vocab_size: int) -> None:
        import xgrammar as xgr

        self.xgr = xgr
        info = xgr.TokenizerInfo.from_huggingface(hf_tokenizer, vocab_size=vocab_size)
        self.compiler = xgr.GrammarCompiler(info)
        self.vocab_size = vocab_size
        self._cache: dict[str, Any] = {}

    def matcher(self, schema: dict[str, Any]) -> Any:
        # Keep the schema's own property order: the grammar fixes key order to it (as llama.cpp's
        # json-schema-to-grammar does). Sorting the keys here would force alphabetical JSON.
        key = json.dumps(schema, ensure_ascii=False)
        if key not in self._cache:
            self._cache[key] = self.compiler.compile_json_schema(key, any_whitespace=True, strict_mode=True)
        return self.xgr.GrammarMatcher(self._cache[key])


class Engine:
    def __init__(self, args: argparse.Namespace) -> None:
        from transformers import AutoTokenizer

        self.tokenizer = AutoTokenizer.from_pretrained(args.tokenizer)
        self.lm = PteLM(args.pte, args.threads, args.prefill_chunk)
        self.vocab_size = int(self.lm.module.run_method("get_vocab_size")[0]) \
            if "get_vocab_size" in self._method_names() else 102400
        self.eos_ids = {int(self.tokenizer.convert_tokens_to_ids("[|endofturn|]"))}
        if self.tokenizer.eos_token_id is not None:
            self.eos_ids.add(int(self.tokenizer.eos_token_id))
        self.grammar = None if args.no_grammar else Grammar(self.tokenizer, self.vocab_size)
        self.prompt_mode = args.prompt_mode
        self.model_name = args.model_name
        self.lock = threading.Lock()
        self.trace = open(args.trace, "a", encoding="utf-8") if args.trace else None
        LOG.info("loaded %s in %.1fs, max_seq_len=%d, dynamic_prefill=%s, threads=%d, vocab=%d, eos=%s",
                 args.pte, self.lm.load_s, self.lm.max_seq_len, self.lm.dynamic_prefill, self.lm.threads,
                 self.vocab_size, sorted(self.eos_ids))

    def _method_names(self) -> set[str]:
        try:
            return set(self.lm.module.method_names())
        except Exception:  # older bindings
            return {"get_vocab_size"}

    def render(self, messages: list[dict[str, Any]], template_kwargs: dict[str, Any]) -> list[int]:
        if self.prompt_mode == "llama-compat":
            messages = [m for m in messages if m.get("role") != "system"]
        text = self.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True,
                                                  **template_kwargs)
        return list(self.tokenizer(text, add_special_tokens=False)["input_ids"])

    def complete(self, body: dict[str, Any]) -> dict[str, Any]:
        max_tokens = int(body.get("max_tokens") or 256)
        schema = None
        fmt = body.get("response_format") or {}
        if fmt.get("type") == "json_schema":
            schema = (fmt.get("json_schema") or {}).get("schema")
        elif fmt.get("type") == "json_object":
            schema = {"type": "object"}
        prompt = self.render(body["messages"], dict(body.get("chat_template_kwargs") or {}))
        with self.lock:
            t0 = time.perf_counter()
            logits, reused = self.lm.prefill(prompt)
            t1 = time.perf_counter()
            matcher = self.grammar.matcher(schema) if (self.grammar is not None and schema) else None
            bitmask = self.grammar.xgr.allocate_token_bitmask(1, self.vocab_size) if matcher else None
            out: list[int] = []
            finish = "length"
            while len(out) < max_tokens:
                if matcher is not None:
                    matcher.fill_next_token_bitmask(bitmask)
                    logits = logits.unsqueeze(0)
                    self.grammar.xgr.apply_token_bitmask_inplace(logits, bitmask)
                    logits = logits[0]
                token = int(torch.argmax(logits).item())
                if token in self.eos_ids:
                    finish = "stop"
                    break
                if matcher is not None:
                    if not matcher.accept_token(token):
                        finish = "grammar_error"
                        break
                out.append(token)
                if matcher is not None and matcher.is_terminated():
                    finish = "stop"
                    break
                if len(self.lm.cached) + 1 >= self.lm.max_seq_len:
                    break
                logits = self.lm.step(token)
            t2 = time.perf_counter()
        content = self.tokenizer.decode(out, skip_special_tokens=True)
        timings = {
            "prompt_n": len(prompt) - reused, "prompt_cached": reused, "prompt_ms": round((t1 - t0) * 1000, 3),
            "predicted_n": len(out), "predicted_ms": round((t2 - t1) * 1000, 3),
        }
        if self.trace:
            self.trace.write(json.dumps({"prompt_tokens": len(prompt), "content": content, "finish": finish,
                                         "timings": timings}, ensure_ascii=False) + "\n")
            self.trace.flush()
        return {
            "id": f"chatcmpl-{uuid.uuid4().hex[:16]}",
            "object": "chat.completion",
            "created": int(time.time()),
            "model": self.model_name,
            "choices": [{"index": 0, "finish_reason": finish,
                         "message": {"role": "assistant", "content": content}}],
            "usage": {"prompt_tokens": len(prompt), "completion_tokens": len(out),
                      "total_tokens": len(prompt) + len(out),
                      "prompt_tokens_details": {"cached_tokens": reused}},
            "timings": timings,
        }


def make_handler(engine: Engine) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        def _send(self, code: int, value: dict[str, Any]) -> None:
            data = json.dumps(value, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self) -> None:  # noqa: N802
            if self.path == "/health":
                self._send(200, {"status": "ok"})
            elif self.path == "/v1/models":
                self._send(200, {"object": "list", "data": [{"id": engine.model_name, "object": "model"}]})
            else:
                self._send(404, {"error": "not found"})

        def do_POST(self) -> None:  # noqa: N802
            if self.path != "/v1/chat/completions":
                self._send(404, {"error": "not found"})
                return
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode("utf-8"))
                self._send(200, engine.complete(body))
            except Exception as exc:  # report, keep serving
                LOG.exception("request failed")
                self._send(500, {"error": {"message": str(exc)}})

        def log_message(self, fmt: str, *args: Any) -> None:
            LOG.debug(fmt, *args)

    return Handler


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--pte", required=True)
    p.add_argument("--tokenizer", required=True, help="dir with tokenizer.json + chat_template.jinja")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8093)
    p.add_argument("--model-name", default="exaone-et")
    p.add_argument("--prompt-mode", choices=["llama-compat", "hf"], default="llama-compat")
    p.add_argument("--no-grammar", action="store_true")
    p.add_argument("--threads", type=int, default=None)
    p.add_argument("--prefill-chunk", type=int, default=256)
    p.add_argument("--trace", default=None, help="append one JSON line per request (no utterance text)")
    args = p.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    engine = Engine(args)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(engine))
    LOG.info("listening on http://%s:%d (prompt_mode=%s, grammar=%s)", args.host, args.port, args.prompt_mode,
             engine.grammar is not None)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
