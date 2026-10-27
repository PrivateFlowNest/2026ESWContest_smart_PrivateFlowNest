"""Canonical labels derived from the current HomeMind Notion specification."""

ACTIVITY_LABELS = (
    "QUIET",
    "SELF_TALK",
    "TV_MEDIA",
    "ZOOM_SPEAKER",
    "ZOOM_HEADPHONES",
    "IN_PERSON_MEETING",
    "UNKNOWN",
)

SOUND_EVENT_LABELS = (
    "DOORBELL",
    "WASHER_COMPLETE",
    "ALARM",
    "OTHER",
)

QUIET_REQUIRED_CONTEXTS = frozenset(
    {
        "ZOOM_SPEAKER",
        "ZOOM_HEADPHONES",
        "IN_PERSON_MEETING",
    }
)

HEAD_LABELS = {
    "activity": ACTIVITY_LABELS,
    "sound_event": SOUND_EVENT_LABELS,
}
