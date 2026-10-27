"""Portable signed hashing for Korean character n-gram features."""

from __future__ import annotations

import re
import unicodedata

import numpy as np


SPACE = re.compile(r"\s+")


SEMANTIC_TERMS = {
    "CLIMATE_COMFORT": ("춥", "쌀쌀", "으슬", "따뜻", "덥", "후끈", "시원", "냉방", "난방", "온도", "기온", "몇 도", "실내가 몇", "에어컨", "차갑", "포근", "냉기", "열기", "뜨거", "서늘", "선선", "찜통", "땀"),
    "LIGHTING": ("어둡", "밝", "불빛", "조명", "전등", "눈부", "깜깜", "방 불", "현관 불", "환하", "빛", "조도", "스탠드", "천장 등"),
    "VENTILATION": ("환기", "공기", "냄새", "탁해", "습", "눅눅", "먼지", "창문", "열린 창", "환풍", "공기청정", "매캐", "숨쉬", "수증기", "외풍", "퀴퀴"),
    "MEDIA": ("티비", "tv", "텔레비전", "영상", "영화", "뉴스", "드라마", "대사", "볼륨", "음량", "음소거", "방송", "프로그램", "미디어", "화면", "채널", "재생"),
    "ACCESS": ("현관문", "출입문", "화장실 문", "욕실", "문 상태", "현관", "출입 상태", "출입구", "문 앞", "입구", "택배 기사"),
    "LAUNDRY": ("세탁", "빨래", "세탁기", "옷을 넣", "옷을 다 빨", "남은 세탁"),
    "CLEANING": ("청소", "로봇청소기", "청소기", "방바닥", "바닥", "부스러기", "치우", "방 정리", "먼지 치우", "정리가 끝"),
    "QUIET_ENVIRONMENT": ("회의", "조용", "고요", "방해", "집중", "통화", "화상", "발표", "온라인 수업", "온라인 강의", "말소리", "상대방 목소리", "알림", "마이크", "수업"),
}

NON_ACTION_TERMS = (
    "단어", "제목", "이야기", "사진", "광고", "리뷰", "영어로", "표현", "색상",
    "새 제품", "좋아하는 편", "친구를 만나", "메뉴", "일정", "생각이 안 나", "배경",
    "디자인", "배우", "노래", "인테리어", "참고용", "약자", "설명서", "뜻은 아니",
    "말한 게 아니", "건드리지", "그대로", "유지해", "부르기도", "제품", "지 마",
    "하지 마", "말고", "뜻이 아니", "부탁한 게 아니", "줄이지 않았으면",
)

REQUEST_TERMS = (
    "싶", "필요", "해야", "확인", "상태", "궁금", "좋겠", "너무", "같은데",
    "모르겠", "준비", "조절", "알려", "보고 싶", "켜", "끄", "열", "닫", "줄", "낮",
)


def normalize_text(text: str) -> str:
    value = unicodedata.normalize("NFKC", text).strip().lower()
    return SPACE.sub(" ", value)[:512]


def fnv1a_32(value: str) -> int:
    result = 2166136261
    for byte in value.encode("utf-8", errors="replace"):
        result ^= byte
        result = (result * 16777619) & 0xFFFFFFFF
    return result


def vectorize_one(text: str, *, dimensions: int = 4096) -> np.ndarray:
    if dimensions < 128:
        raise ValueError("dimensions must be at least 128")
    plain = normalize_text(text)
    normalized = f"^{plain}$"
    vector = np.zeros(dimensions, dtype=np.float32)
    hashed_dimensions = dimensions - 64
    for size in (1, 2, 3, 4, 5):
        if len(normalized) < size:
            continue
        for start in range(len(normalized) - size + 1):
            token = normalized[start : start + size]
            hashed = fnv1a_32(f"{size}:{token}")
            index = hashed % hashed_dimensions
            sign = 1.0 if (hashed & 0x80000000) == 0 else -1.0
            vector[index] += sign / size**0.5
    offset = hashed_dimensions
    for index, terms in enumerate(SEMANTIC_TERMS.values()):
        matches = sum(plain.count(term) for term in terms)
        vector[offset + index * 2] = min(matches, 3) * 8.0
        vector[offset + index * 2 + 1] = 8.0 if matches else 0.0
    non_action_matches = sum(plain.count(term) for term in NON_ACTION_TERMS)
    request_matches = sum(plain.count(term) for term in REQUEST_TERMS)
    vector[offset + 20] = min(non_action_matches, 3) * 10.0
    vector[offset + 21] = 10.0 if non_action_matches else 0.0
    vector[offset + 22] = min(request_matches, 4) * 2.0
    vector[offset + 23] = 2.0 if request_matches else 0.0
    vector[offset + 24] = min(len(plain), 100) / 25.0
    norm = float(np.linalg.norm(vector))
    if norm:
        vector /= norm
    return vector


def vectorize(texts: list[str], *, dimensions: int = 4096) -> np.ndarray:
    return np.stack([vectorize_one(text, dimensions=dimensions) for text in texts])
