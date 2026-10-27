"""Korean ASR normalization and privacy-safe aggregate metrics."""

from __future__ import annotations

import re
import unicodedata


def normalize_korean(text: str) -> str:
    value = unicodedata.normalize("NFKC", text).casefold()
    return re.sub(r"[^0-9a-z가-힣]", "", value)


def edit_distance(reference: str, hypothesis: str) -> int:
    previous = list(range(len(hypothesis) + 1))
    for index, ref_char in enumerate(reference, start=1):
        current = [index]
        for other, hyp_char in enumerate(hypothesis, start=1):
            current.append(
                min(
                    current[-1] + 1,
                    previous[other] + 1,
                    previous[other - 1] + (ref_char != hyp_char),
                )
            )
        previous = current
    return previous[-1]


def character_error_counts(reference: str, hypothesis: str) -> tuple[int, int]:
    normalized_reference = normalize_korean(reference)
    normalized_hypothesis = normalize_korean(hypothesis)
    return edit_distance(normalized_reference, normalized_hypothesis), max(
        1, len(normalized_reference)
    )


def keyword_recall(reference: str, hypothesis: str, keywords: list[str]) -> tuple[int, int]:
    ref = normalize_korean(reference)
    hyp = normalize_korean(hypothesis)
    expected = [normalize_korean(item) for item in keywords if normalize_korean(item) in ref]
    return sum(item in hyp for item in expected), len(expected)


def keyword_group_recall(
    reference: str,
    hypothesis: str,
    groups: dict[str, tuple[str, ...]],
) -> tuple[int, int]:
    """Count a concept once even when it has Korean/English spelling aliases."""
    results = keyword_group_results(reference, hypothesis, groups)
    return sum(results.values()), len(results)


def keyword_group_results(
    reference: str,
    hypothesis: str,
    groups: dict[str, tuple[str, ...]],
) -> dict[str, bool]:
    """Return hit/miss only for concepts expected by the reference."""
    ref = normalize_korean(reference)
    hyp = normalize_korean(hypothesis)
    results: dict[str, bool] = {}
    for name, aliases in groups.items():
        normalized = tuple(normalize_korean(alias) for alias in aliases)
        if _keyword_group_present(name, normalized, ref):
            results[name] = _keyword_group_present(name, normalized, hyp)
    return results


def _keyword_group_present(
    name: str, aliases: tuple[str, ...], normalized_text: str
) -> bool:
    if any(alias and alias in normalized_text for alias in aliases):
        return True
    if name == "light":
        return (
            "불" in normalized_text
            and "불고기" not in normalized_text
            and any(action in normalized_text for action in ("켜", "꺼", "끄", "밝", "어둡"))
        )
    return False
