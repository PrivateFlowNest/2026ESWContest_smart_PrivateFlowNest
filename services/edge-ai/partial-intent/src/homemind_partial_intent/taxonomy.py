"""Fixed intent taxonomy and its non-executable preparation hints."""

from __future__ import annotations


INTENTS = (
    "CLIMATE_COMFORT",
    "LIGHTING",
    "VENTILATION",
    "MEDIA",
    "ACCESS",
    "LAUNDRY",
    "CLEANING",
    "QUIET_ENVIRONMENT",
    "NO_ACTION",
)


HINTS = {
    "CLIMATE_COMFORT": {
        "candidate_devices": ["AIR_CONDITIONER"],
        "safe_preparations": ["CHECK_TEMPERATURE", "CHECK_PRESENCE", "CHECK_DEVICE_STATE"],
    },
    "LIGHTING": {
        "candidate_devices": ["LIGHT"],
        "safe_preparations": ["CHECK_PRESENCE", "CHECK_DEVICE_STATE"],
    },
    "VENTILATION": {
        "candidate_devices": ["WINDOW", "VENTILATION_FAN", "AIR_PURIFIER"],
        "safe_preparations": ["CHECK_AIR_QUALITY", "CHECK_OPENING_STATE", "CHECK_DEVICE_STATE"],
    },
    "MEDIA": {
        "candidate_devices": ["TV"],
        "safe_preparations": ["CHECK_DEVICE_STATE"],
    },
    "ACCESS": {
        "candidate_devices": ["ENTRY_DOOR", "BATHROOM_DOOR"],
        "safe_preparations": ["CHECK_OPENING_STATE", "CHECK_PRESENCE"],
    },
    "LAUNDRY": {
        "candidate_devices": ["WASHER"],
        "safe_preparations": ["CHECK_TASK_STATE", "CHECK_DEVICE_STATE"],
    },
    "CLEANING": {
        "candidate_devices": ["ROBOT_VACUUM"],
        "safe_preparations": ["CHECK_TASK_STATE", "CHECK_PRESENCE", "CHECK_DEVICE_STATE"],
    },
    "QUIET_ENVIRONMENT": {
        "candidate_devices": ["ALL_QUIET_DEVICES"],
        "safe_preparations": ["WAIT_FOR_CONTEXT_EVIDENCE", "CHECK_PRESENCE"],
    },
    "NO_ACTION": {"candidate_devices": [], "safe_preparations": []},
}
