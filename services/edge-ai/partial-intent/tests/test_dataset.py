from homemind_partial_intent.dataset import group_leakage, load_rows
from homemind_partial_intent.taxonomy import INTENTS


def test_dataset_has_group_separated_splits(trained_baseline):
    data, _ = trained_baseline
    all_rows = []
    for split in ("train", "validation", "test"):
        rows = load_rows(data, split)
        assert rows
        assert set(row["intent"] for row in rows) == set(INTENTS)
        all_rows.extend(rows)
    assert group_leakage(all_rows) == []
