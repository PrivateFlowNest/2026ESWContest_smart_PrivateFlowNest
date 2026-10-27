"""Record the P8 real-microphone STT acceptance set (PC tool; a person reads each sentence).

    services/runtime/.venv/Scripts/python services/edge-ai/speech-to-text/tools/record_acceptance.py \
        --out E:/HomeMind-Datasets/recordings/real-mic-v1 --speaker S01 --distance near_0_5m [--device "USB"]
    services/runtime/.venv/Scripts/python .../record_acceptance.py --list-devices

Per sentence: Enter = start, Enter = stop, then [Enter] keep / r retry / p play / s skip / q quit.
Writes 16 kHz mono PCM16 WAVs and manifest.jsonl (same rows as the AI Hub manifests), resumable.
Evaluate afterwards with:
    homemind-stt eval --backend whisper --model-dir <whisper-bin> --model-file <ggml-...q5_0.bin> \
        --manifest <out>/manifest.jsonl --report <out>/report-<model>.json
Audio and manifest stay outside git (LOCAL_EVALUATION_ONLY).
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 16_000
SENTENCES = Path(__file__).resolve().parents[1] / "config" / "acceptance-real-mic-v1.json"


def _record(device: str | None) -> np.ndarray:
    import sounddevice as sd

    chunks: list[np.ndarray] = []
    stop = threading.Event()

    def callback(indata, frames, time_info, status) -> None:
        del frames, time_info
        if status:
            print(f"  ({status})", file=sys.stderr)
        chunks.append(indata[:, 0].copy())

    with sd.InputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16", device=device, callback=callback):
        threading.Thread(target=lambda: (input(), stop.set()), daemon=True).start()
        stop.wait()
    return np.concatenate(chunks) if chunks else np.zeros(0, dtype=np.int16)


def _play(samples: np.ndarray) -> None:
    import sounddevice as sd

    sd.play(samples, SAMPLE_RATE)
    sd.wait()


def _write_wav(path: Path, samples: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(samples.astype("<i2").tobytes())


def _level_db(samples: np.ndarray) -> float:
    if samples.size == 0:
        return -120.0
    rms = float(np.sqrt(np.mean((samples.astype(np.float64) / 32768.0) ** 2)))
    return 20.0 * np.log10(max(rms, 1e-6))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path)
    parser.add_argument("--speaker", default="S01", help="anonymous speaker code, never a name")
    parser.add_argument("--distance", default="near_0_5m")
    parser.add_argument("--device", default=None, help="sounddevice input name or index")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--sentences", type=Path, default=SENTENCES)
    args = parser.parse_args(argv)
    if args.list_devices:
        import sounddevice as sd

        print(sd.query_devices())
        return 0
    if args.out is None:
        parser.error("--out is required")
    config = json.loads(args.sentences.read_text(encoding="utf-8"))
    if args.distance not in config["distances"]:
        parser.error(f"--distance must be one of {config['distances']}")
    device = int(args.device) if args.device and args.device.isdigit() else args.device
    manifest = args.out / "manifest.jsonl"
    done = set()
    if manifest.exists():
        done = {json.loads(line)["id"] for line in manifest.read_text(encoding="utf-8").splitlines() if line.strip()}
    todo = [s for s in config["sentences"] if f"{config['set_version']}-{args.speaker}-{args.distance}-{s['id']}" not in done]
    print(f"{len(todo)} sentences left ({len(done)} recorded). Read naturally, about 1 s silence before/after.")
    for index, sentence in enumerate(todo, 1):
        row_id = f"{config['set_version']}-{args.speaker}-{args.distance}-{sentence['id']}"
        while True:
            print(f"\n[{index}/{len(todo)}] 「{sentence['text']}」  Enter = start")
            if input().strip().lower() == "q":
                return 0
            print("  recording... Enter = stop")
            samples = _record(device)
            seconds = samples.size / SAMPLE_RATE
            level = _level_db(samples)
            print(f"  {seconds:.1f} s, level {level:.0f} dBFS" + ("  (too quiet?)" if level < -45 else ""))
            choice = input("  [Enter] keep / r retry / p play / s skip / q quit: ").strip().lower()
            if choice == "p":
                _play(samples)
                choice = input("  [Enter] keep / r retry / s skip / q quit: ").strip().lower()
            if choice == "q":
                return 0
            if choice == "s":
                break
            if choice == "r" or seconds < 0.3:
                continue
            wav = args.out / "audio" / f"{row_id}.wav"
            _write_wav(wav, samples)
            row = {"id": row_id, "audio_path": str(wav.resolve()), "reference": sentence["text"],
                   "duration_seconds": round(seconds, 3), "domain": sentence["domain"], "speaker": args.speaker,
                   "distance": args.distance, "source": f"HOMEMIND-{config['set_version'].upper()}",
                   "privacy_scope": "LOCAL_EVALUATION_ONLY"}
            with manifest.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(row, ensure_ascii=False) + "\n")
            break
    print(f"\ndone: {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
