"""hm_audio vs the Python reference (services/runtime audio/vad.py, homemind_audio WAV loading, scipy resample_poly).

PC x86_64 Docker only (not a Raspberry Pi). Real audio: AIHub clips from the local dataset drive (HM_AIHUB_DIR,
default E:/HomeMind-Datasets/raw/aihub), copied into the pytest temp folder for the run and deleted with it.
Never committed. Real-audio cases are skipped when the drive is not there.

Run (from the repo root):
  set HM_NATIVE_BUILDER=perception
  services/runtime/.venv/Scripts/python -m pytest native/tests/test_perception_audio.py -q -p no:cacheprovider \
      --basetemp "%TEMP%/hmpt/b1"
"""

from __future__ import annotations

import glob
import itertools
import json
import os
import shutil
import sys
import wave
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import REPO, run_shell  # noqa: E402
from test_perception_support import BIN, build_targets  # noqa: E402

from homemind_audio.audio import load_wav_16k_mono, load_wav_segment_16k_mono  # noqa: E402
from homemind_runtime.audio.stt import write_pcm16_wav  # noqa: E402
from homemind_runtime.audio.vad import EndpointConfig, EnergyEndpointer  # noqa: E402
from scipy.io import wavfile  # noqa: E402
from scipy.signal import firwin, resample_poly  # noqa: E402

PROBE = f"{BIN}/libs/hm_audio/hm_audio_probe"
SR = 16_000
AIHUB = Path(os.environ.get("HM_AIHUB_DIR", "E:/HomeMind-Datasets/raw/aihub"))
MANIFEST = REPO / "services/edge-ai/speech-to-text/artifacts/manifests/aihub-71405-home-near-homemind-100.jsonl"
REPORT: dict[str, object] = {}


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_targets("hm_audio_probe hm_audio_tests")


@pytest.fixture(scope="module")
def work(tmp_path_factory) -> Path:
    return tmp_path_factory.mktemp("audio")


@pytest.fixture(scope="module", autouse=True)
def report(work):
    yield
    (work / "report.json").write_text(json.dumps(REPORT, indent=1), encoding="utf-8")
    print("\nPERCEPTION_AUDIO_REPORT " + json.dumps(REPORT, ensure_ascii=False))


def probe(work: Path, requests: list[dict]) -> list[dict]:
    lines = "\n".join(json.dumps(item, ensure_ascii=False) for item in requests) + "\n"
    result = run_shell(PROBE, stdin=lines, extra_mounts=[f"{work.as_posix()}:/t"], timeout=1200)
    assert result.returncode == 0, result.stderr
    answers = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    assert len(answers) == len(requests), result.stderr
    errors = [a["error"] for a in answers if "error" in a]
    assert not errors, errors
    return answers


def f32(work: Path, name: str, samples: np.ndarray) -> str:
    np.asarray(samples, dtype="<f4").tofile(work / name)
    return f"/t/{name}"


def read_f32(work: Path, name: str) -> np.ndarray:
    return np.fromfile(work / name, dtype="<f4")


# ---- signals ----------------------------------------------------------------------------------------
def tone(seconds, amplitude=0.3, freq=220.0):
    t = np.arange(int(seconds * SR)) / SR
    return (amplitude * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def noise(seconds, amplitude=0.0005, seed=0):
    return (amplitude * np.random.default_rng(seed).standard_normal(int(seconds * SR))).astype(np.float32)


def synthetic_signals() -> dict[str, np.ndarray]:
    rng = np.random.default_rng(7)
    ramp = np.linspace(0.0, 1.0, 6 * SR, dtype=np.float32) ** 3 * 0.05
    modulated = (0.02 * rng.standard_normal(8 * SR) * (0.5 + 0.5 * np.sin(np.arange(8 * SR) / SR * 2 * np.pi * 1.3)))
    bursts = np.concatenate([noise(0.4 + 0.1 * (i % 3), seed=i).astype(np.float32) if i % 2 == 0
                             else (0.1 * rng.standard_normal(int(SR * (0.05 + 0.07 * i)))).astype(np.float32)
                             for i in range(14)])
    return {
        "python_one": np.concatenate([noise(1.0), tone(1.2), noise(1.5)]),
        "python_click": np.concatenate([noise(1.0), tone(0.1), noise(1.0), tone(0.3), noise(1.2)]),
        "python_two": np.concatenate([noise(0.5), tone(0.8), noise(1.0), tone(0.8), noise(1.0)]),
        "python_pause": np.concatenate([noise(0.5), tone(0.6), noise(0.4), tone(0.6), noise(1.0)]),
        "long_tone": np.concatenate([noise(0.5), tone(10.0)]),
        "ramp": (ramp * np.sin(np.arange(ramp.size) / SR * 2 * np.pi * 300)).astype(np.float32),
        "modulated_noise": modulated.astype(np.float32),
        "bursts": bursts,
        "digital_silence": np.zeros(3 * SR, dtype=np.float32),
        "full_scale_square": np.sign(tone(2.0, 1.0, 97.0)).astype(np.float32),
        "dc_offset": (np.concatenate([noise(1.0), tone(1.0)]) + 0.01).astype(np.float32),
        "odd_tail": np.concatenate([noise(0.73), tone(0.91, 0.05, 1234.5), noise(0.777)]),
    }


VAD_CONFIGS = {
    "default": {},
    "example_config": {"start_ms": 200, "end_silence_ms": 700, "max_ms": 8000, "min_ms": 400, "pre_roll_ms": 300,
                       "threshold_db": -45.0, "noise_margin_db": 10.0},
    "strict": {"start_ms": 100, "end_silence_ms": 400, "max_ms": 3000, "min_ms": 200, "pre_roll_ms": 500,
               "tail_ms": 0, "threshold_db": -35.0, "noise_margin_db": 6.0, "start_ratio": 0.6, "noise_adapt": 0.2},
}


class TracingEndpointer(EnergyEndpointer):
    """Python reference with a per-frame trace (same expressions as vad.py _frame)."""

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.trace: list[tuple[float, float, bool]] = []

    def _frame(self, frame):
        threshold = self.threshold_db()
        rms = float(np.sqrt(np.mean(np.square(frame, dtype=np.float64)) + 1e-12))
        level = 20.0 * np.log10(rms + 1e-10)
        self.trace.append((float(level), float(threshold), bool(level >= threshold)))
        return super()._frame(frame)


def python_vad(samples: np.ndarray, config: dict, *, armed=True, begin=(), stop=(), block=1600):
    endpointer = TracingEndpointer(EndpointConfig.from_mapping(config), armed=armed)
    found = []

    def record(u):
        return {"start": int(u.start_sample), "end": int(u.end_sample), "reason": u.reason, "n": int(u.samples.size)}

    for index, offset in enumerate(range(0, samples.size, block)):
        if index in begin:
            endpointer.begin()
        if index in stop:
            manual = endpointer.stop("manual")
            if manual is not None:
                found.append(record(manual))
        found.extend(record(u) for u in endpointer.feed(samples[offset: offset + block]))
    last = endpointer.stop("flush")
    if last is not None:
        found.append(record(last))
    return found, endpointer.trace


def compare_vad(work: Path, cases: list[tuple[str, np.ndarray, dict, dict]]) -> dict[str, float]:
    requests = []
    expected = []
    for name, samples, config, extra in cases:
        path = f32(work, f"vad_{name}.f32", samples)
        requests.append({"op": "vad", "in": path, "config": config, "trace": True, "block": 1600,
                         "armed": extra.get("armed", True), "begin": sorted(extra.get("begin", ())),
                         "stop": sorted(extra.get("stop", ()))})
        expected.append(python_vad(samples, config, armed=extra.get("armed", True), begin=set(extra.get("begin", ())),
                                   stop=set(extra.get("stop", ()))))
    answers = probe(work, requests)
    frames = flips = utterances = 0
    max_level = max_threshold = 0.0
    for (name, *_), (py_utts, py_trace), native in zip(cases, expected, answers):
        assert native["utterances"] == py_utts, (name, native["utterances"], py_utts)
        trace = native["trace"]
        assert len(trace) == len(py_trace), name
        for (level, threshold, voiced), (plevel, pthreshold, pvoiced) in zip(trace, py_trace):
            max_level = max(max_level, abs(level - plevel))
            max_threshold = max(max_threshold, abs(threshold - pthreshold))
            flips += voiced != pvoiced
        frames += len(trace)
        utterances += len(py_utts)
    assert flips == 0
    assert max_level < 1e-9 and max_threshold < 1e-9
    return {"cases": len(cases), "frames": frames, "utterances": utterances, "voiced_flag_mismatches": flips,
            "max_abs_level_diff_db": max_level, "max_abs_threshold_diff_db": max_threshold}


# ---- real clips -------------------------------------------------------------------------------------
def _manifest_clips(limit: int) -> list[Path]:
    if not MANIFEST.is_file():
        return []
    out = []
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        path = Path(json.loads(line)["audio_path"].replace("\\", "/"))
        if path.is_file():
            out.append(path)
        if len(out) >= limit:
            break
    return out


def _first(pattern: str) -> Path | None:
    return next((Path(p) for p in itertools.islice(glob.iglob(str(AIHUB / pattern), recursive=True), 1)), None)


def _copy_segment(source: Path, target: Path, offset_s: float, seconds: float) -> Path:
    with wave.open(str(source), "rb") as reader:
        rate = reader.getframerate()
        reader.setpos(int(offset_s * rate))
        frames = reader.readframes(int(seconds * rate))
        with wave.open(str(target), "wb") as writer:
            writer.setnchannels(reader.getnchannels())
            writer.setsampwidth(reader.getsampwidth())
            writer.setframerate(rate)
            writer.writeframes(frames)
    return target


@pytest.fixture(scope="module")
def real(work) -> dict[str, Path]:
    if not AIHUB.is_dir():
        pytest.skip(f"AIHub dataset folder not found: {AIHUB}")
    folder = work / "real"
    folder.mkdir(exist_ok=True)
    found: dict[str, Path] = {}
    for index, clip in enumerate(_manifest_clips(24)):
        found[f"cmd{index:02d}"] = Path(shutil.copy(clip, folder / f"cmd{index:02d}.wav"))
    meeting = _first("464_meeting/**/*.wav")
    if meeting:
        found["meeting60"] = _copy_segment(meeting, folder / "meeting60.wav", 600.0, 60.0)
    environment = _first("71698_environment_sound/**/*.wav")
    if environment:
        found["env44k"] = _copy_segment(environment, folder / "env44k.wav", 10.0, 30.0)
    conversation = _first("109_free_conversation/**/*.wav")
    if conversation:
        found["talk"] = Path(shutil.copy(conversation, folder / "talk.wav"))
    if len(found) < 4:
        pytest.skip("not enough AIHub clips on this PC")
    return found


def test_unit_tests_pass() -> None:
    result = run_shell(f"{BIN}/libs/hm_audio/hm_audio_tests --gtest_brief=1 2>&1 | tail -5")
    assert "PASSED" in result.stdout and "FAILED" not in result.stdout, result.stdout


def test_vad_parity_synthetic(work) -> None:
    cases = []
    for name, samples in synthetic_signals().items():
        for config_name, config in VAD_CONFIGS.items():
            cases.append((f"{name}-{config_name}", samples, config, {}))
    ptt = np.concatenate([noise(0.5), tone(1.0), noise(1.0), noise(1.0, seed=3), tone(0.8), noise(1.0), tone(0.6)])
    cases += [
        ("ptt_idle", ptt, {}, {"armed": False}),
        ("ptt_begin", ptt, {}, {"armed": False, "begin": {26}}),
        ("ptt_begin_stop", ptt, {}, {"armed": False, "begin": {5, 26, 50}, "stop": {12, 60}}),
        ("ptt_strict", ptt, VAD_CONFIGS["strict"], {"armed": False, "begin": {3, 30}, "stop": {40}}),
        ("always_begin_mid_speech", ptt, {}, {"armed": True, "begin": {8, 40}, "stop": {44}}),
    ]
    REPORT["vad_synthetic"] = compare_vad(work, cases)


def test_vad_parity_real_audio(work, real) -> None:
    gap = noise(0.8, amplitude=0.001, seed=11)
    commands = [load_wav_16k_mono(path) for key, path in sorted(real.items()) if key.startswith("cmd")]
    stream = np.concatenate([part for clip in commands for part in (clip, gap)]).astype(np.float32)
    cases = []
    for config_name, config in VAD_CONFIGS.items():
        cases.append((f"commands-{config_name}", stream, config, {}))
    for key in ("meeting60", "talk", "env44k"):
        if key in real:
            samples = load_wav_16k_mono(real[key])
            for config_name, config in VAD_CONFIGS.items():
                cases.append((f"{key}-{config_name}", samples, config, {}))
    begins = set(range(4, stream.size // 1600, 37))
    cases.append(("commands-ptt", stream, {}, {"armed": False, "begin": begins, "stop": {b + 25 for b in begins}}))
    result = compare_vad(work, cases)
    result["seconds"] = round(sum(case[1].size for case in cases) / SR, 1)
    REPORT["vad_real"] = result
    assert result["utterances"] > 20


def test_wav_loading_matches_python(work, real) -> None:
    folder = work / "fmt"
    folder.mkdir(exist_ok=True)
    speech = load_wav_16k_mono(real["cmd00"])
    talk = load_wav_16k_mono(real.get("talk", real["cmd01"]))
    files: dict[str, Path] = {key: path for key, path in real.items() if key in {"cmd00", "cmd01", "meeting60", "talk", "env44k"}}
    up48 = resample_poly(speech, 3, 1).astype(np.float32)
    wavfile.write(folder / "s48_int16.wav", 48_000, (np.clip(up48, -1, 1) * 32767).astype("<i2"))
    files["s48_int16"] = folder / "s48_int16.wav"
    n = min(speech.size, talk.size)
    stereo = np.stack([resample_poly(speech[:n], 3, 1), resample_poly(talk[:n], 3, 1)], axis=1)
    wavfile.write(folder / "stereo48.wav", 48_000, (np.clip(stereo, -1, 1) * 32767).astype("<i2"))
    files["stereo48"] = folder / "stereo48.wav"
    wavfile.write(folder / "s8k.wav", 8_000, (resample_poly(speech, 1, 2) * 32767).astype("<i2"))
    files["s8k"] = folder / "s8k.wav"
    wavfile.write(folder / "s22k.wav", 22_050, (resample_poly(speech, 441, 320) * 32767).astype("<i2"))
    files["s22k"] = folder / "s22k.wav"
    wavfile.write(folder / "float32.wav", 16_000, (speech * 3.0).astype("<f4"))  # clipped on load
    files["float32"] = folder / "float32.wav"
    wavfile.write(folder / "float64_48k.wav", 48_000, up48.astype("<f8"))
    files["float64_48k"] = folder / "float64_48k.wav"
    wavfile.write(folder / "u8.wav", 16_000, np.clip(speech * 128 + 128, 0, 255).astype(np.uint8))
    files["u8"] = folder / "u8.wav"
    pcm24 = np.clip(np.round(talk * 8_388_607), -8_388_608, 8_388_607).astype("<i4")
    with wave.open(str(folder / "pcm24.wav"), "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(3)
        writer.setframerate(16_000)
        writer.writeframes(b"".join(int(v).to_bytes(3, "little", signed=True) for v in pcm24))
    files["pcm24"] = folder / "pcm24.wav"
    files["short"] = folder / "short.wav"
    write_pcm16_wav(files["short"], speech[:5000])  # padded to 1 s

    requests, expected, names = [], [], []
    for key, path in files.items():
        requests.append({"op": "load", "wav": f"/t/{path.relative_to(work).as_posix()}", "out": f"/t/load_{key}.f32"})
        expected.append(load_wav_16k_mono(path))
        names.append((key, "whole"))
    for key, offset, seconds in (("meeting60", 12.3456, 20.0), ("env44k", 3.21, 9.99), ("stereo48", 0.5, 1.0),
                                 ("cmd00", 0.123, 1.0)):
        if key not in files:
            continue
        path = files[key]
        requests.append({"op": "load", "wav": f"/t/{path.relative_to(work).as_posix()}", "offset": offset,
                         "seconds": seconds, "out": f"/t/seg_{key}.f32"})
        expected.append(load_wav_segment_16k_mono(path, offset, offset + seconds))
        names.append((key, "segment"))
    probe(work, requests)
    results = {}
    for (key, kind), request, reference in zip(names, requests, expected):
        native = read_f32(work, Path(request["out"]).name)
        assert native.size == reference.size, (key, kind, native.size, reference.size)
        results[f"{key}/{kind}"] = float(np.max(np.abs(native - reference))) if native.size else 0.0
    exact = {k: v for k, v in results.items() if not any(s in k for s in ("48", "8k", "22k", "44k"))}
    assert all(value == 0.0 for value in exact.values()), exact
    assert max(results.values()) < 1e-5, results
    REPORT["wav_load_max_abs_diff"] = results


def test_resampler_matches_scipy(work, real) -> None:
    speech = load_wav_16k_mono(real["cmd00"])
    rng = np.random.default_rng(5)
    inputs = {
        "speech48k": resample_poly(speech, 3, 1).astype(np.float32),
        "noise44k": (0.3 * rng.standard_normal(44_100 * 3)).astype(np.float32),
        "speech8k": resample_poly(speech, 1, 2).astype(np.float32),
        "clipped_square22k": np.sign(np.sin(np.arange(22_050 * 2) / 22_050 * 2 * np.pi * 440)).astype(np.float32),
    }
    rates = {"speech48k": (1, 3), "noise44k": (160, 441), "speech8k": (2, 1), "clipped_square22k": (320, 441)}
    requests = []
    for key, x in inputs.items():
        up, down = rates[key]
        path = f32(work, f"rs_{key}.f32", x)
        requests.append({"op": "resample", "in": path, "up": up, "down": down, "out": f"/t/rs_{key}_out.f32"})
        requests.append({"op": "stream_resample", "in": path, "up": up, "down": down, "chunk": 4410,
                         "out": f"/t/rs_{key}_stream.f32"})
        requests.append({"op": "taps", "up": up, "down": down})
    answers = probe(work, requests)
    results = {}
    for index, (key, x) in enumerate(inputs.items()):
        up, down = rates[key]
        reference = resample_poly(x, up, down)
        native = read_f32(work, f"rs_{key}_out.f32")
        streamed = read_f32(work, f"rs_{key}_stream.f32")
        assert reference.dtype == np.float32 and native.size == reference.size
        max_rate = max(up, down)
        h = firwin(2 * 10 * max_rate + 1, 1.0 / max_rate, window=("kaiser", 5.0)).astype(np.float32)
        h *= up
        taps = np.asarray(answers[3 * index + 2]["taps"], dtype=np.float32)
        pre_pad = down - (10 * max_rate) % down
        assert np.all(taps[:pre_pad] == 0)
        tap_diff = np.abs(taps[pre_pad:] - h)
        results[key] = {
            "up_down": f"{up}/{down}",
            "max_abs_diff": float(np.max(np.abs(native - reference))),
            "exact_fraction": float(np.mean(native == reference)),
            "taps_differing": int(np.count_nonzero(tap_diff)),
            "taps_total": int(h.size),
            "stream_equals_batch": bool(np.array_equal(streamed, native[: streamed.size])),
        }
        assert results[key]["max_abs_diff"] < 1e-6, results[key]
        assert results[key]["stream_equals_batch"] and streamed.size > 0.9 * native.size
    REPORT["resampler"] = results


def test_resampled_input_gives_same_vad_decisions(work, real) -> None:
    # Each side loads the non-16 kHz file with its own resampler; the VAD must decide the same.
    folder = work / "fmt"
    keys = [k for k in ("env44k",) if k in real] + [p.stem for p in folder.glob("s48_int16.wav")]
    paths = {"env44k": real.get("env44k"), "s48_int16": folder / "s48_int16.wav"}
    requests, expected = [], []
    for key in keys:
        path = paths[key]
        requests.append({"op": "load", "wav": f"/t/{path.relative_to(work).as_posix()}", "out": f"/t/vadload_{key}.f32"})
    probe(work, requests)
    vad_requests = []
    for key in keys:
        vad_requests.append({"op": "vad", "in": f"/t/vadload_{key}.f32", "config": {}, "trace": True})
        expected.append(python_vad(load_wav_16k_mono(paths[key]), {}))
    answers = probe(work, vad_requests)
    for key, (utts, trace), native in zip(keys, expected, answers):
        assert native["utterances"] == utts, key
        assert [t[2] for t in native["trace"]] == [t[2] for t in trace], key
    REPORT["vad_on_resampled_input"] = {key: len(u) for key, (u, _) in zip(keys, expected)}


def test_pcm16_encoding_is_byte_identical(work, real) -> None:
    samples = np.concatenate([load_wav_16k_mono(real["cmd00"]), np.array([1.5, -1.5, 0.99999, -0.99999, 0.0], "f4")])
    write_pcm16_wav(work / "py.wav", samples)
    probe(work, [{"op": "encode", "in": f32(work, "enc.f32", samples), "out": "/t/native.wav"}])
    assert (work / "native.wav").read_bytes() == (work / "py.wav").read_bytes()
