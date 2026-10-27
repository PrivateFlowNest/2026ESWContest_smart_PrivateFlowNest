import numpy as np

from homemind_partial_intent.features import normalize_text, vectorize_one


def test_features_are_normalized_and_deterministic():
    assert normalize_text("  거실   불을 켜줘  ") == "거실 불을 켜줘"
    first = vectorize_one("거실 불을 켜줘")
    second = vectorize_one("거실 불을 켜줘")
    assert first.dtype == np.float32
    assert np.array_equal(first, second)
    assert np.isclose(np.linalg.norm(first), 1.0)
