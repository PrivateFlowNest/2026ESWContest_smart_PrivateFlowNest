"""Deterministic safety policy applied after probabilistic classification."""

from __future__ import annotations

from .features import normalize_text


SUPPRESSION_PHRASES = (
    "지 마",
    "지마",
    "하지 마",
    "하지마",
    "말고",
    "그대로 둬",
    "그대로 유지",
    "건드리지",
    "필요 없어",
    "필요없어",
    "원하지 않아",
    "원한 게 아니",
    "뜻은 아니",
    "뜻이 아니",
    "하라는 게 아니",
    "달라는 뜻은 아니",
    "부탁한 게 아니",
    "줄이지 않았으면",
    "열지 않았으면",
    "시작하라고 말한 게 아니",
)

META_REFERENCE_PHRASES = (
    "제품 이름",
    "제품 비교",
    "사용 설명서",
    "광고에",
    "디자인에 관해",
    "사진이 들어간",
    "사진을 인테리어",
    "모형을 어떻게",
    "제목의",
    "제목이",
    "단어의 뜻",
    "뜻을 설명",
    "약자가",
)

DIRECT_ACTION_TERMS = (
    "켜줘",
    "켜고",
    "끄고",
    "꺼줘",
    "열어",
    "닫아",
    "작동해",
    "작동시켜",
    "시작해",
    "멈춰",
    "줄여",
    "낮춰",
    "높여",
    "올려",
    "바꿔",
    "바꿀",
    "작동해 달라고",
    "켜지",
    "열지",
    "시작하라고",
)


def apply_safety_policy(text: str, predicted_intent: str) -> str:
    normalized = normalize_text(text)
    if any(phrase in normalized for phrase in SUPPRESSION_PHRASES + META_REFERENCE_PHRASES):
        return "NO_ACTION"
    return predicted_intent


def requires_final_transcript(text: str) -> bool:
    normalized = normalize_text(text)
    return any(term in normalized for term in DIRECT_ACTION_TERMS)
