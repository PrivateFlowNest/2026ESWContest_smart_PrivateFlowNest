"""Deterministic guards for cases that must not depend on LLM judgment."""

from __future__ import annotations

import re
from typing import Any, Mapping


DEVICE_ALIASES = {
    "LIGHT": ("불", "조명", "전등"),
    "TV": ("tv", "티비", "티브이", "텔레비전", "텔레비"),
    "WASHER": ("세탁기", "세탁"),
    "WINDOW": ("창문",),
    "AIR_PURIFIER": ("공기청정기", "공청기"),
    "ENTRY_DOOR": ("현관문", "현관 문", "출입문", "대문"),
    "BATHROOM_DOOR": ("화장실문", "화장실 문", "욕실문", "욕실 문"),
    "VENTILATION_FAN": ("환기팬", "환풍기", "환기 팬"),
    "AIR_CONDITIONER": ("에어컨", "에어콘", "냉방"),
    "LAPTOP": ("노트북",),
    "ROBOT_VACUUM": ("로봇청소기", "로봇 청소기"),
    "NOTIFICATION": ("알림",),
}

DEVICE_LABELS = {
    "LIGHT": "조명",
    "TV": "TV",
    "WASHER": "세탁기",
    "WINDOW": "창문",
    "AIR_PURIFIER": "공기청정기",
    "ENTRY_DOOR": "현관문",
    "BATHROOM_DOOR": "화장실문",
    "VENTILATION_FAN": "환기팬",
    "AIR_CONDITIONER": "에어컨",
    "LAPTOP": "노트북",
    "ROBOT_VACUUM": "로봇청소기",
    "NOTIFICATION": "알림",
}

UNSUPPORTED_DEVICE_ALIASES = (
    "오븐",
    "전자레인지",
    "가스레인지",
    "보일러",
    # Household appliances HomeMind does not control (registry v3 has none of them).
    "냉장고",
    "식기세척기",
    "건조기",
    "정수기",
    "전기장판",
    "인덕션",
    "밥솥",
    "커피머신",
    "커피 머신",
    "스타일러",
    "가습기",
    "제습기",
    "히터",
    "선풍기",
)

ACTION_MARKERS = (
    "켜",
    "틀어",
    "돌려",
    "키워",
    "끄",
    "꺼",
    "열",
    "닫",
    "줄",
    "낮",
    "올",
    "높",
    "음소거",
    "상태",
    "끝났",
    "조용",
    "미뤄",
    "재개",
    "시작",
    "돌아",
    "설정",
    "맞춰",
    "온도",
    "저소음",
    "안 나게",
    "다시 나게",
    "다시 알려",
    "나중에 알려",
)


def _proposal(
    intent: str,
    *,
    device: str | None = None,
    action: str | None = None,
    parameters: dict[str, Any] | None = None,
    ambiguous: bool = False,
    flags: list[str] | None = None,
    question: str | None = None,
) -> dict[str, Any]:
    return {
        "intent": intent,
        "device": device,
        "action": action,
        "parameters": parameters or {},
        "ambiguous": ambiguous,
        "safety_flags": flags or [],
        "clarification_question": question,
    }


# Command verbs, excluding state forms such as 켜진/켜져/꺼진/열려/닫혀 so that
# "켜진 거 꺼줘" is TURN_OFF and "불 꺼져 있어?" is a status question.
_ON_VERB = re.compile(r"(?:켜|틀어|돌려)(?![져진졌있])")
_OFF_VERB = re.compile(r"(?:꺼|끄)(?![져진졌])")
_OPEN_VERB = re.compile(r"열(?![려렸린])")
_CLOSE_VERB = re.compile(r"닫(?![혀혔힌])")
_STATE_WORDS = (
    "상태", "켜져", "켜졌", "꺼져", "꺼졌", "열려", "열렸", "닫혀", "닫혔", "끝났",
    "돌았", "다 됐", "몇 도", "몇 단", "돌아가고 있",
)
_QUESTION_END = re.compile(r"(?:\?|(?:있어|있나|있니|있어요|됐어|됐나|돌았어|인가|맞지|맞아)\s*\??)\s*$")
_RELATIVE_DOWN = ("줄여", "낮춰", "내려")
_RELATIVE_UP = ("올려", "높여", "키워", "크게")
_INJECTION_MARKERS = (
    "제한 없",
    "권한으로 명령",
    "확인 같은 거 하지 말",
    "확인하지 말고",
    "이전 규칙",
    "규칙을 무시",
    "규칙은 취소",
    "지시를 무시",
    "json 규칙",
    "json 밖",
    "시스템 프롬프트",
    "프롬프트",
    "개발자 모드",
    "관리자 모드",
    "디버그 모드",
    "탈옥",
)


def _last_match(pattern: re.Pattern[str], text: str) -> int:
    positions = [match.start() for match in pattern.finditer(text)]
    return positions[-1] if positions else -1


def _last_of(text: str, first: tuple[str, re.Pattern[str]], second: tuple[str, re.Pattern[str]]) -> str | None:
    """Return the action whose verb appears last in the sentence (the final verb wins)."""
    first_at = _last_match(first[1], text)
    second_at = _last_match(second[1], text)
    if first_at < 0 and second_at < 0:
        return None
    return first[0] if first_at > second_at else second[0]


def _is_injection(text: str) -> bool:
    if any(marker in text for marker in _INJECTION_MARKERS):
        return True
    names_rules = any(word in text for word in ("규칙", "지시", "명령", "설정"))
    overrides = any(word in text for word in ("무시", "잊", "취소하고", "해제하고", "없던 걸로"))
    if names_rules and overrides:
        return True
    # Asking to skip the safety checks themselves ("정책 검사 건너뛰고", "승인 없이 바로").
    names_checks = any(word in text for word in ("검사", "검증", "확인", "승인", "정책", "보안"))
    bypass = re.search(r"건너뛰|생략하고|우회|없이\s*(?:바로|그냥|전부|다)|무시하고", text)
    return names_checks and bypass is not None


def _mentioned_devices(text: str) -> list[str]:
    return [
        device
        for device, aliases in DEVICE_ALIASES.items()
        if any(alias in text for alias in aliases)
    ]


def preflight_proposal(
    utterance: str,
    capabilities: Mapping[str, Any],
    context: Mapping[str, Any] | None = None,
) -> dict[str, Any] | None:
    """Return a safe deterministic proposal, or None when EXAONE should decide."""

    text = utterance.casefold().strip()
    devices = _mentioned_devices(text)
    # "세탁기 알림" names the washer's notification feature, not two
    # independently targeted devices.
    if "WASHER" in devices and "NOTIFICATION" in devices:
        devices.remove("NOTIFICATION")
    enabled = {
        name
        for name, value in capabilities.get("devices", {}).items()
        if isinstance(value, dict) and value.get("enabled")
    }

    active_context = context or {}

    if _is_injection(text):
        return _proposal(
            "CLARIFY",
            ambiguous=True,
            flags=["CONFLICTING_REQUEST"],
            question="요청에 상충하는 지시가 있습니다. 원하는 기기 동작만 다시 말해 주세요.",
        )

    # Negation in any ending ("켜지 말라니까", "열지 말고") and "안 해도 돼" style permission
    # not to act ("굳이 안 꺼도 돼") must never become a command.
    if re.search(r"(?:지\s*마|지\s*말|말아\s*(?:줘|주세요))", text) or re.search(
        r"(?:안|않아)\s*\S*\s*(?:도\s*(?:돼|된다|되|괜찮))", text
    ):
        return _proposal("NO_ACTION", flags=["NEGATED_REQUEST"])

    if any(
        marker in text
        for marker in (
            "불이 났", "불난", "화재", "탄내", "타는 냄새", "연기가 나", "연기 나",
            "가스 냄새", "가스가 새", "침입자", "도둑", "따고 들어", "들어오려",
            "사람을 다치", "쓰러졌", "숨을 못",
            # medical emergency
            "숨이 안 쉬어", "숨이 잘 안 쉬어", "숨쉬기가 힘", "숨쉬기 힘", "숨이 막", "가슴이 아파",
            "가슴이 너무 아파", "가슴이 조여", "의식이 없", "피가 많이", "구급차", "응급",
        )
    ) or re.search(r"문\s*(?:을|를)?\s*(?:계속\s*|자꾸\s*|몰래\s*)?따(?:려|고|는|네)", text):
        return _proposal(
            "CLARIFY",
            ambiguous=True,
            flags=["HIGH_RISK"],
            question="위험 상황은 자동 실행하지 않습니다. 안전을 확인한 뒤 기기 동작을 다시 말해 주세요.",
        )

    unsupported_name = next(
        (name for name in UNSUPPORTED_DEVICE_ALIASES if name in text), None
    )
    disabled = next((device for device in devices if device not in enabled), None)
    if unsupported_name or disabled:
        label = unsupported_name or DEVICE_LABELS.get(disabled or "", disabled)
        return _proposal(
            "CLARIFY",
            ambiguous=True,
            flags=["UNSUPPORTED_DEVICE"],
            question=f"{label}은(는) 현재 지원되지 않습니다. 다른 기기를 말씀하셨나요?",
        )

    # A quiet context must not swallow an explicit device command spoken during a
    # meeting ("창문 닫아줘"); it only answers sentences that name no device.
    quiet_words = ("조용", "소리", "방해", "시끄", "알림")
    if bool(active_context.get("quiet_required")) and not devices and any(w in text for w in quiet_words):
        return _proposal(
            "CONTEXT_SUGGESTION",
            device="ALL_QUIET_DEVICES",
            action="SET_QUIET_MODE",
        )

    if not devices and any(
        word in text
        for word in ("그거", "이거", "저거", "그 장치", "이 장치", "저 장치", "그 기기", "이 기기", "저 기기")
    ):
        return _proposal(
            "CLARIFY",
            ambiguous=True,
            flags=["AMBIGUOUS_TARGET"],
            question="어느 기기를 말씀하셨나요?",
        )

    volume = re.search(r"(?:볼륨|소리)[^0-9]{0,12}(\d{1,3})", text)
    absolute_volume = re.search(r"\d{1,3}\s*(?:으로|로)", text)
    if "TV" in devices and volume and not absolute_volume:
        # "소리 10 정도 줄여줘" is a relative change, not "set the volume to 10".
        if any(word in text for word in _RELATIVE_DOWN):
            return _proposal("DEVICE_CONTROL", device="TV", action="DECREASE_VOLUME")
        if any(word in text for word in _RELATIVE_UP):
            return _proposal("DEVICE_CONTROL", device="TV", action="INCREASE_VOLUME")
    if "TV" in devices and volume:
        value = int(volume.group(1))
        if value > 100:
            return _proposal(
                "CLARIFY",
                ambiguous=True,
                flags=["AMBIGUOUS_ACTION"],
                question="TV 볼륨은 0부터 100 사이로 말해 주세요.",
            )
        return _proposal(
            "DEVICE_CONTROL",
            device="TV",
            action="SET_VOLUME",
            parameters={"volume": value},
        )

    brightness = re.search(r"밝기[^0-9]{0,12}(\d{1,3})", text)
    brightness_word = re.search(r"밝기\s*(백|오십|삼십|이십|십)(?:으로|로)?", text)
    if "LIGHT" in devices and brightness:
        value = int(brightness.group(1))
        if value > 100:
            return _proposal(
                "CLARIFY",
                ambiguous=True,
                flags=["AMBIGUOUS_ACTION"],
                question="조명 밝기는 0부터 100 사이로 말해 주세요.",
            )
        return _proposal(
            "DEVICE_CONTROL",
            device="LIGHT",
            action="SET_BRIGHTNESS",
            parameters={"brightness": value},
        )
    if "LIGHT" in devices and brightness_word:
        value = {"백": 100, "오십": 50, "삼십": 30, "이십": 20, "십": 10}[
            brightness_word.group(1)
        ]
        return _proposal(
            "DEVICE_CONTROL",
            device="LIGHT",
            action="SET_BRIGHTNESS",
            parameters={"brightness": value},
        )

    # Whole-room quiet context outranks a single notification alias in the
    # same sentence (for example, a meeting plus deferred notifications).
    quiet_request = (
        any(word in text for word in ("조용", "방해되는 소리", "전체 소리"))
        and any(word in text for word in ("바꿔", "줄여", "멈춰", "없애"))
    ) or (
        "회의" in text
        and "알림" in text
        and any(word in text for word in ("줄여", "조용", "미뤄"))
    )
    if quiet_request:
        return _proposal(
            "CONTEXT_SUGGESTION",
            device="ALL_QUIET_DEVICES",
            action="SET_QUIET_MODE",
        )

    if len(devices) == 1:
        device = devices[0]

        has_command_verb = any(
            pattern.search(text) for pattern in (_ON_VERB, _OFF_VERB, _OPEN_VERB, _CLOSE_VERB)
        )
        asks_state = any(word in text for word in _STATE_WORDS) or _QUESTION_END.search(text)
        if asks_state and not has_command_verb:
            return _proposal("DEVICE_QUERY", device=device, action="QUERY_STATUS")

        if device == "LIGHT" and "밝혀" in text:
            return _proposal("DEVICE_CONTROL", device=device, action="TURN_ON")

        if device == "TV":
            if any(
                word in text
                for word in (
                    "다시 나게",
                    "다시 나오게",
                    "음소거 해제",
                    "음소거를 풀",
                    "음소거 풀",
                )
            ):
                return _proposal("DEVICE_CONTROL", device=device, action="UNMUTE")
            if any(word in text for word in ("안 나게", "음소거")):
                return _proposal("DEVICE_CONTROL", device=device, action="MUTE")
            if any(word in text for word in ("줄여", "낮춰", "내려", "작게")):
                return _proposal("DEVICE_CONTROL", device=device, action="DECREASE_VOLUME")
            if any(word in text for word in ("올려", "높여", "키워", "크게")):
                return _proposal("DEVICE_CONTROL", device=device, action="INCREASE_VOLUME")

        if device == "WASHER":
            if any(word in text for word in ("다시 알려", "재개", "다시 받", "다시 보내")):
                return _proposal(
                    "DEVICE_CONTROL", device=device, action="RESUME_NOTIFICATION"
                )
            if "나중에 알려" in text or "미뤄" in text:
                return _proposal(
                    "DEVICE_CONTROL", device=device, action="DEFER_NOTIFICATION"
                )

        if device == "AIR_CONDITIONER" and any(
            word in text for word in ("저소음", "조용한 모드", "조용하게")
        ):
            return _proposal(
                "DEVICE_CONTROL", device=device, action="SET_QUIET_MODE"
            )

        if device == "AIR_CONDITIONER":
            temperature = re.search(r"(\d{1,2})\s*도", text)
            if temperature:
                value = int(temperature.group(1))
                if not 16 <= value <= 30:
                    return _proposal(
                        "CLARIFY",
                        ambiguous=True,
                        flags=["AMBIGUOUS_ACTION"],
                        question="에어컨 온도는 16도부터 30도 사이로 말해 주세요.",
                    )
                return _proposal(
                    "DEVICE_CONTROL",
                    device=device,
                    action="SET_TEMPERATURE",
                    parameters={"temperature_celsius": value},
                )
            level = re.search(r"([1-5])\s*단(?:계)?", text)
            if level:
                return _proposal(
                    "DEVICE_CONTROL",
                    device=device,
                    action="SET_LEVEL",
                    parameters={"level": int(level.group(1))},
                )

        if device == "NOTIFICATION":
            if any(word in text for word in ("재개", "다시 시작", "다시 보내", "다시 받", "다시 알려")):
                return _proposal(
                    "DEVICE_CONTROL", device=device, action="RESUME_NOTIFICATION"
                )
            if any(word in text for word in ("나중", "미뤄")):
                return _proposal(
                    "DEVICE_CONTROL", device=device, action="DEFER_NOTIFICATION"
                )
            if any(
                word in text
                for word in ("음소거를 풀", "음소거 풀", "음소거 해제", "다시 들리", "다시 나")
            ):
                return _proposal("DEVICE_CONTROL", device=device, action="UNMUTE")
            if any(word in text for word in ("음소거", "소리 없애")):
                return _proposal("DEVICE_CONTROL", device=device, action="MUTE")

        if device in {"ENTRY_DOOR", "BATHROOM_DOOR", "WINDOW"}:
            door_action = _last_of(text, ("OPEN", _OPEN_VERB), ("CLOSE", _CLOSE_VERB))
            if door_action:
                return _proposal("DEVICE_CONTROL", device=device, action=door_action)

        switch_action = _last_of(text, ("TURN_ON", _ON_VERB), ("TURN_OFF", _OFF_VERB))
        if switch_action:
            return _proposal("DEVICE_CONTROL", device=device, action=switch_action)

    if devices and re.search(r"(?:할까|할래|할 예정)(?:\s|[.!?]|$)", text):
        return _proposal("NO_ACTION")

    if devices and not any(marker in text for marker in ACTION_MARKERS):
        return _proposal(
            "CLARIFY",
            ambiguous=True,
            flags=["AMBIGUOUS_ACTION"],
            question="해당 기기를 어떻게 할까요?",
        )

    contextual = any(word in text for word in ("회의", "집중", "조용"))
    if "회의 준비" in text and any(word in text for word in ("다 했", "끝났", "됐")):
        return _proposal("NO_ACTION")
    if not devices and not contextual:
        explicit_control_without_target = re.search(
            r"(?:켜|틀어|꺼|끄|열어|닫아|줄여|올려|낮춰|높여|음소거|설정해|맞춰)"
            r"(?:\s*(?:줘|주세요|줄래|줄 수|줬으면))",
            text,
        )
        if explicit_control_without_target:
            return _proposal(
                "CLARIFY",
                ambiguous=True,
                flags=["AMBIGUOUS_TARGET"],
                question="어느 기기를 말씀하셨나요?",
            )
        # No device and no explicit command: a possible hidden intent ("좀 덥네").
        # EXAONE decides; its proposal still needs UI approval (task-policy llm_needs_approval).
        return None
    return None
