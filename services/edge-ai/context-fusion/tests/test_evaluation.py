from homemind_context_fusion.evaluation import _confusion_summary, build_evaluation_summary


def test_evaluation_reports_quiet_recall_and_false_positive_rate():
    report = build_evaluation_summary(
        [
            {
                "expected": "IN_PERSON_MEETING",
                "predicted": "IN_PERSON_MEETING",
                "quiet_required": True,
                "activation_delay_seconds": 6.0,
            },
            {
                "expected": "ZOOM_SPEAKER",
                "predicted": "UNKNOWN",
                "quiet_required": False,
                "activation_delay_seconds": None,
            },
            {
                "expected": "TV_MEDIA",
                "predicted": "UNKNOWN",
                "quiet_required": False,
                "activation_delay_seconds": None,
            },
            {
                "expected": "SELF_TALK",
                "predicted": "IN_PERSON_MEETING",
                "quiet_required": True,
                "activation_delay_seconds": 6.0,
            },
        ],
        latency_ms=[10.0, 20.0, 30.0],
        model_version="test",
    )

    assert report["quiet_required"]["recall"] == 0.5
    assert report["quiet_required"]["false_positive_rate"] == 0.5
    assert report["activation_delay_seconds"]["mean"] == 6.0
    assert report["privacy"]["file_paths_in_report"] is False


def test_classifier_confusion_summary_is_separate_from_fusion_metrics():
    summary = _confusion_summary(
        [
            {"expected": "TV_MEDIA", "predicted": "TV_MEDIA"},
            {"expected": "TV_MEDIA", "predicted": "IN_PERSON_MEETING"},
            {"expected": "SELF_TALK", "predicted": "SELF_TALK"},
        ]
    )

    assert summary["samples"] == 3
    assert summary["accuracy"] == 0.6667
    assert summary["confusion_matrix"]["labels"] == [
        "IN_PERSON_MEETING",
        "SELF_TALK",
        "TV_MEDIA",
    ]
