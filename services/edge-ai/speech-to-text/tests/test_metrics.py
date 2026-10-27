from homemind_stt.metrics import (
    character_error_counts,
    keyword_group_recall,
    keyword_recall,
    normalize_korean,
)


def test_korean_normalization_ignores_spacing_and_punctuation():
    assert normalize_korean("거실 불을 켜 줘!") == "거실불을켜줘"
    assert character_error_counts("거실 불을 켜 줘", "거실불을 켜줘") == (0, 6)


def test_keyword_recall_counts_only_expected_keywords():
    assert keyword_recall("TV 소리 줄여줘", "티비 소리 줄여 줘", ["TV", "소리"]) == (1, 2)


def test_keyword_group_recall_supports_aliases_without_double_counting():
    groups = {"tv": ("TV", "티비"), "sound": ("소리", "볼륨")}
    assert keyword_group_recall("TV 소리 줄여줘", "티비 볼륨 줄여 줘", groups) == (2, 2)


def test_light_keyword_does_not_match_bulgogi():
    groups = {"light": ("조명", "전등", "불을", "불켜", "불꺼")}
    assert keyword_group_recall("불고기를 좋아해", "불고기를 좋아해", groups) == (0, 0)


def test_light_keyword_accepts_command_with_intervening_words():
    groups = {"light": ("조명", "전등", "불을", "불켜", "불꺼")}
    assert keyword_group_recall("불 좀 켜 줘", "불을 조금만 켜 줘", groups) == (1, 1)
