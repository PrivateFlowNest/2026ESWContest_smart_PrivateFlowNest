"""Explainable six-class fusion rules for the PC-first baseline."""

from __future__ import annotations

from dataclasses import dataclass

from .models import CONTEXT_TYPES, QUIET_REQUIRED_CONTEXTS, ContextObservation


CLASSIFIABLE_CONTEXTS = CONTEXT_TYPES[:-1]


@dataclass(frozen=True)
class Classification:
    context_type: str
    confidence: float
    evidence: tuple[str, ...]
    scores: dict[str, float]


def classify(
    observation: ContextObservation,
    *,
    quiet_audio_confidence: float = 0.0,
    meeting_tv_margin: float = 0.0,
) -> Classification:
    if not 0.0 <= quiet_audio_confidence <= 1.0:
        raise ValueError("quiet_audio_confidence must be between 0 and 1")
    if not 0.0 <= meeting_tv_margin <= 1.0:
        raise ValueError("meeting_tv_margin must be between 0 and 1")
    scores = {label: 0.04 for label in CLASSIFIABLE_CONTEXTS}
    evidence: dict[str, list[str]] = {label: [] for label in CLASSIFIABLE_CONTEXTS}

    def add(label: str, amount: float, reason: str) -> None:
        scores[label] += amount
        evidence[label].append(reason)

    probabilities = observation.audio_class_probabilities or {}
    meeting_probability = float(probabilities.get("IN_PERSON_MEETING", 0.0))
    tv_probability = float(probabilities.get("TV_MEDIA", 0.0))
    meeting_tv_ambiguous = (
        observation.audio_context == "IN_PERSON_MEETING"
        and "IN_PERSON_MEETING" in probabilities
        and "TV_MEDIA" in probabilities
        and meeting_probability - tv_probability < meeting_tv_margin
    )
    quiet_audio_suppressed = meeting_tv_ambiguous or (
        observation.audio_context in QUIET_REQUIRED_CONTEXTS
        and observation.audio_confidence < quiet_audio_confidence
    )
    suppression_reason = (
        "guard:meeting_tv_margin:"
        f"{meeting_probability - tv_probability:.2f}<{meeting_tv_margin:.2f}"
        if meeting_tv_ambiguous
        else "guard:quiet_audio_below_threshold:"
        f"{observation.audio_confidence:.2f}<{quiet_audio_confidence:.2f}"
    )
    if observation.audio_context in scores and not quiet_audio_suppressed:
        add(
            observation.audio_context,
            0.72 * observation.audio_confidence,
            f"audio:{observation.audio_context}:{observation.audio_confidence:.2f}",
        )
        if observation.audio_model_version:
            evidence[observation.audio_context].append(
                f"audio_model:{observation.audio_model_version}"
            )
    elif quiet_audio_suppressed:
        evidence["QUIET"].append(suppression_reason)
        if meeting_tv_ambiguous:
            add("TV_MEDIA", 0.72 * tv_probability, suppression_reason)

    if observation.speech_ratio < 0.08:
        add("QUIET", 0.48, f"speech_ratio:low:{observation.speech_ratio:.2f}")
    elif observation.speech_ratio >= 0.18:
        for label in (
            "SELF_TALK",
            "ZOOM_SPEAKER",
            "ZOOM_HEADPHONES",
            "IN_PERSON_MEETING",
        ):
            add(label, 0.08, f"speech_ratio:active:{observation.speech_ratio:.2f}")

    if observation.turns_per_minute is not None and observation.turns_per_minute >= 3.0:
        proxy = observation.turn_taking_method == "SPEECH_BURST_PROXY"
        add(
            "IN_PERSON_MEETING",
            0.10 if proxy else 0.27,
            f"turn_taking:{'proxy' if proxy else 'high'}:{observation.turns_per_minute:.1f}",
        )
        add(
            "ZOOM_SPEAKER",
            0.06 if proxy else 0.15,
            f"turn_taking:{'proxy' if proxy else 'present'}:{observation.turns_per_minute:.1f}",
        )
    elif (
        observation.turns_per_minute is not None
        and observation.turns_per_minute < 1.5
        and observation.speech_ratio >= 0.12
    ):
        add("SELF_TALK", 0.22, f"turn_taking:low:{observation.turns_per_minute:.1f}")
        add("TV_MEDIA", 0.10, f"turn_taking:low:{observation.turns_per_minute:.1f}")

    if observation.duration_seconds >= 20.0:
        for label in ("TV_MEDIA", "ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING"):
            add(label, 0.08, f"duration:sustained:{observation.duration_seconds:.0f}s")

    if observation.presence_zone in {"DESK", "COMMON"} and observation.presence_confidence >= 0.4:
        add("IN_PERSON_MEETING", 0.10, f"presence:{observation.presence_zone.lower()}")
        add("ZOOM_SPEAKER", 0.09, f"presence:{observation.presence_zone.lower()}")
        add("ZOOM_HEADPHONES", 0.09, f"presence:{observation.presence_zone.lower()}")

    if observation.tv_on is True:
        add("TV_MEDIA", 0.38, "device:tv_on")
    if observation.laptop_active is True:
        add("ZOOM_SPEAKER", 0.17, "device:laptop_active")
        add("ZOOM_HEADPHONES", 0.17, "device:laptop_active")
    if observation.headphones_connected is True:
        add("ZOOM_HEADPHONES", 0.36, "device:headphones_connected")

    # A speech-context classifier can still return a confident speech label for
    # digital silence. VAD is the stronger physical signal in this case.
    if observation.speech_ratio < 0.08:
        for label in (
            "SELF_TALK",
            "ZOOM_SPEAKER",
            "ZOOM_HEADPHONES",
            "IN_PERSON_MEETING",
        ):
            scores[label] *= 0.20
        evidence["QUIET"].append("guard:no_speech_context_suppressed")

    # False-positive guard from R-021: TV playback must not become a meeting merely
    # because speech and alternating voices are present in the programme audio.
    if observation.tv_on is True:
        for label in ("ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING"):
            scores[label] *= 0.35
        evidence["TV_MEDIA"].append("guard:tv_meeting_suppressed")

    ranking = sorted(scores, key=scores.get, reverse=True)
    winner, runner_up = ranking[:2]
    top, second = scores[winner], scores[runner_up]
    margin = top - second
    if top < 0.22 or margin < 0.025:
        return Classification(
            context_type="UNKNOWN",
            confidence=round(max(0.25, min(0.60, 0.35 + margin)), 4),
            evidence=tuple(
                item
                for item in (
                    suppression_reason if quiet_audio_suppressed else None,
                    f"uncertain:{winner}:{top:.3f}",
                    f"margin:{margin:.3f}",
                )
                if item is not None
            ),
            scores={key: round(value, 4) for key, value in scores.items()},
        )

    confidence = min(0.99, 0.42 + 0.32 * min(top, 1.0) + 0.34 * min(margin, 1.0))
    reasons = evidence[winner] or [f"score:{winner}:{top:.3f}"]
    reasons.append(f"margin:{margin:.3f}")
    return Classification(
        context_type=winner,
        confidence=round(confidence, 4),
        evidence=tuple(dict.fromkeys(reasons)),
        scores={key: round(value, 4) for key, value in scores.items()},
    )
