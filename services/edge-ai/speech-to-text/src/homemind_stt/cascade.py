"""Two-stage local ASR: fast partial hints followed by an accurate final transcript."""

from __future__ import annotations

from typing import Iterator
from uuid import uuid4

from .events import transcript_event


def cascade_events(partial_backend, final_backend, wav_path) -> Iterator[dict[str, object]]:
    """Emit one utterance stream without publishing audio outside Pi 5.

    Vosk's final hypothesis is deliberately discarded. It exists to provide
    non-executable early hints; the selected final backend owns the transcript
    passed to EXAONE.
    """
    partial_result = partial_backend.transcribe(wav_path)
    final_result = final_backend.transcribe(wav_path)
    utterance_id = f"utt-{uuid4()}"
    sequence = 0
    for partial in partial_result.partials:
        yield transcript_event(
            partial,
            utterance_id=utterance_id,
            sequence=sequence,
            is_final=False,
            model_version=partial_result.model_version,
        )
        sequence += 1
    yield transcript_event(
        final_result.text,
        utterance_id=utterance_id,
        sequence=sequence,
        is_final=True,
        model_version=final_result.model_version,
    )
