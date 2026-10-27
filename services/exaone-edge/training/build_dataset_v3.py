"""Build finetune-v3: v2 data + hidden-intent, chit-chat and extra safety examples.

Differences from build_dataset.py (v1/v2):
- Every prompt is produced by ``ExaoneClient._payload`` so the training input is
  byte-identical to what the production client sends to llama.cpp (current
  SYSTEM_PROMPT, few-shot turns, JSON formatting, enabled_device_actions).
- When the deterministic guard produces an actionable candidate for a v2 record,
  half of those records carry ``validated_local_candidate`` exactly as the client
  would send it; the other half stay candidate-free so the model keeps standalone
  command understanding for phrasings the guard cannot parse.
- New sentences are written here by hand. None are copied from the acceptance
  files. Every new record is checked against the guard: hidden-intent and
  chit-chat records must reach the model (guard returns None); records the guard
  would answer differently are dropped and listed in the manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import random
import sys
from typing import Any


SERVICE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE_ROOT / "src"))

from homemind_exaone.client import ExaoneClient  # noqa: E402
from homemind_exaone.guardrails import preflight_proposal  # noqa: E402
from homemind_exaone.validation import (  # noqa: E402
    load_json_object,
    validate_activity_context,
    validate_proposal,
)


def proposal(
    intent: str,
    *,
    device: str | None = None,
    action: str | None = None,
    ambiguous: bool = False,
    flags: list[str] | None = None,
    question: str | None = None,
) -> dict[str, Any]:
    return {
        "intent": intent,
        "device": device,
        "action": action,
        "parameters": {},
        "ambiguous": ambiguous,
        "safety_flags": flags or [],
        "clarification_question": question,
    }


def ctx(**updates: Any) -> dict[str, Any]:
    return validate_activity_context(updates)


DEFAULT = ctx()
COMMON = ctx(presence_zone="COMMON", activity="GENERAL_ACTIVITY", confidence=0.66)
DESK = ctx(presence_zone="DESK", activity="WORKING", audio_context="QUIET", confidence=0.74)
BED = ctx(presence_zone="BED", activity="RESTING", audio_context="QUIET", confidence=0.71)
BATH = ctx(presence_zone="BATHROOM", activity="BATHROOM_USE", audio_context="QUIET", confidence=0.7)
TVCTX = ctx(presence_zone="COMMON", activity="RESTING", audio_context="TV_MEDIA", confidence=0.72)
SELF = ctx(presence_zone="COMMON", activity="GENERAL_ACTIVITY", audio_context="SELF_TALK", confidence=0.6)

HIGH_RISK_Q = "위험 상황은 자동 실행하지 않습니다. 안전을 확인한 뒤 기기 동작을 다시 말해 주세요."

# label -> (expected proposal, contexts to rotate through, core sentences)
HIDDEN: dict[str, tuple[dict[str, Any], tuple[dict[str, Any], ...], tuple[str, ...]]] = {
    "hidden/AIR_CONDITIONER/TURN_ON": (
        proposal("DEVICE_CONTROL", device="AIR_CONDITIONER", action="TURN_ON"),
        (DEFAULT, COMMON, DESK, BED),
        (
            "방이 찜통 같아", "땀이 줄줄 흐른다", "숨 막히게 더워", "여기 너무 후텁지근하다",
            "더워서 아무것도 못 하겠어", "집이 한증막이네", "등에 땀이 흥건해",
            "너무 더워서 잠이 안 와", "시원하게 좀 해줘", "열기가 장난 아니다",
            "오늘 진짜 푹푹 찐다", "덥고 끈적거려서 짜증 나", "바깥보다 집이 더 더운 것 같아",
            "얼굴이 화끈거릴 정도로 덥다", "선풍기로는 감당이 안 되겠다", "실내 온도가 너무 높은 것 같아",
            "운동하고 왔더니 너무 더워", "햇볕이 들어와서 방이 달아올랐어", "더위 먹을 것 같아",
            "땀 때문에 옷이 다 젖었네", "공기가 뜨끈뜨끈하다", "요리했더니 집이 너무 더워졌어",
            "덥다 더워", "왜 이렇게 더운 거야", "좀 시원했으면 좋겠다", "집 안이 한여름이네",
            "더워 죽겠다", "열대야라 잠을 못 자겠어", "너무 더운데 어떻게 좀 해 줄래",
            "후덥지근해서 숨이 턱턱 막혀", "더워서 머리가 멍하다", "온몸이 땀범벅이야",
            "에휴 덥다 진짜", "시원한 바람이 그립다", "방 안 공기가 너무 뜨겁다",
            "택배 나르고 왔더니 땀이 비 오듯 해",
        ),
    ),
    "hidden/AIR_CONDITIONER/TURN_OFF": (
        proposal("DEVICE_CONTROL", device="AIR_CONDITIONER", action="TURN_OFF"),
        (DEFAULT, COMMON, BED),
        (
            "추워서 덜덜 떨려", "냉기가 너무 세다", "방이 냉장고 같아", "으슬으슬 춥다",
            "찬바람 때문에 목이 칼칼해", "이제 충분히 시원해졌어", "시원하다 못해 추워",
            "너무 서늘해서 긴팔 입어야겠다", "발이 꽁꽁 얼었어", "땀이 다 식었다 이제 됐어",
            "팔에 소름이 돋을 만큼 춥네", "이제 하나도 안 덥다", "너무 차가운 바람이 계속 나와",
            "몸이 너무 식었어", "실내가 너무 쌀쌀해졌다", "냉방병 걸릴 것 같아",
        ),
    ),
    "hidden/VENTILATION_FAN/TURN_ON": (
        proposal("DEVICE_CONTROL", device="VENTILATION_FAN", action="TURN_ON"),
        (DEFAULT, BATH, COMMON, BATH),
        (
            "욕실에 김이 꽉 찼어", "샤워하고 나니까 습기가 가득하다", "화장실 거울에 김이 서려서 안 보여",
            "화장실에서 냄새가 올라와", "벽에 곰팡이 필 것 같아", "욕실 공기가 축축하다",
            "공기가 눅눅해서 찝찝하다", "집 안이 꿉꿉해", "생선 구운 냄새가 안 빠져",
            "고기 냄새가 집에 배겠어", "라면 냄새가 진동을 한다", "환기가 필요해 보여",
            "공기 순환이 안 되는 느낌이야", "습도가 너무 높은 것 같아", "빨래 널었더니 집이 습해",
            "욕실 바닥이 안 말라", "화장실 냄새 좀 빼고 싶다", "샤워 끝났는데 수증기가 가득해",
            "방 안 공기가 퀴퀴하다", "청소했더니 락스 냄새가 독하다", "페인트 냄새 때문에 머리 아파",
            "꿉꿉해서 잠이 안 와", "습기 때문에 몸이 끈적끈적해", "음식 냄새 좀 빼야겠다",
            "방에 냄새가 가득 찼어", "공기가 탁하네", "화장실 쓰고 나니 냄새가 심하다",
            "욕조에 물 받았더니 김이 자욱해", "장마철이라 집이 눅눅하다", "청국장 냄새가 온 집에 퍼졌어",
            "습해서 머리카락이 다 들러붙어", "욕실 타일에 물방울이 맺혔어", "환기 안 하면 곰팡이 생기겠다",
        ),
    ),
    "hidden/VENTILATION_FAN/TURN_OFF": (
        proposal("DEVICE_CONTROL", device="VENTILATION_FAN", action="TURN_OFF"),
        (DEFAULT, BATH),
        (
            "이제 욕실 다 말랐다", "냄새 다 빠졌네 이제 됐다", "환기는 충분히 한 것 같아",
            "습기 다 없어졌다", "욕실 김이 다 빠졌어", "이제 공기 깨끗해졌네",
        ),
    ),
    "hidden/WINDOW/OPEN": (
        proposal("DEVICE_CONTROL", device="WINDOW", action="OPEN"),
        (DEFAULT, COMMON, DESK),
        (
            "바깥 바람 좀 쐬고 싶다", "신선한 공기가 필요해", "밖에 날씨 좋은데 바람 좀 들어오면 좋겠다",
            "방이 답답해서 숨이 막혀", "상쾌한 공기 좀 들이고 싶어", "비 그쳤으니 바람 좀 통하게 하자",
            "선선한 바깥 공기 좀 마시고 싶다", "맞바람 좀 통하게 하고 싶어", "밖이 시원해 보이는데 바람 좀 들이자",
            "새벽 공기 좀 들이고 싶다", "집 안 공기 좀 바꾸자", "바깥 공기가 상쾌해 보여",
            "봄바람 좀 들어오게 하고 싶다", "실내가 너무 갑갑해",
        ),
    ),
    "hidden/WINDOW/CLOSE": (
        proposal("DEVICE_CONTROL", device="WINDOW", action="CLOSE"),
        (DEFAULT, COMMON, BED),
        (
            "비가 안으로 들이치네", "빗물이 바닥에 튀어 들어와", "밖에 공사 소리 너무 시끄럽다",
            "차 소리 때문에 잠을 못 자겠어", "바깥 소음이 너무 심해", "찬바람이 숭숭 들어와",
            "미세먼지 나쁨이래", "밖에서 매연 냄새가 들어와", "벌레 들어올 것 같아",
            "황사가 심하대 먼지 들어오겠다", "바람이 세서 커튼이 펄럭여", "밖에 폭우가 쏟아진다",
            "외풍 때문에 춥다", "오토바이 소리가 너무 시끄러워", "밖에서 담배 연기 냄새가 들어와",
        ),
    ),
    "hidden/LIGHT/TURN_ON": (
        proposal("DEVICE_CONTROL", device="LIGHT", action="TURN_ON"),
        (DEFAULT, COMMON, DESK, BED),
        (
            "여기 깜깜해서 아무것도 안 보여", "눈앞이 캄캄하네", "어두워서 발밑이 안 보여",
            "글씨가 안 보일 정도로 어둡다", "해가 져서 집이 컴컴해", "방이 너무 침침하다",
            "어두워서 무서워", "뭐 찾으려는데 너무 어두워", "밝게 좀 해줘",
            "좀 환하게 해 줄래", "어두컴컴해서 눈 아파", "책 보려는데 빛이 부족해",
            "집에 들어왔는데 완전 깜깜하다", "일어났는데 아직 어둡네", "화장하는데 얼굴이 잘 안 보여",
            "바닥에 뭐 떨어뜨렸는데 안 보여", "실내가 너무 어둑어둑해", "요리하는데 손이 잘 안 보여",
            "어둠 속에서 더듬거리고 있어", "컴컴해서 계단 내려가기 무섭다", "빛이 좀 있었으면 좋겠다",
            "저녁 되니까 방이 어둑하다", "깜깜해서 열쇠를 못 찾겠어", "그림자 때문에 서류가 안 보여",
            "숙제하기엔 너무 어둡네", "어두워서 눈이 침침해",
        ),
    ),
    "hidden/LIGHT/TURN_OFF": (
        proposal("DEVICE_CONTROL", device="LIGHT", action="TURN_OFF"),
        (BED, DEFAULT, BED, COMMON),
        (
            "이제 잘 시간이다", "졸려서 이만 잘게", "눈 좀 붙여야겠어", "오늘은 일찍 자려고",
            "잠자리에 들 시간이야", "너무 눈부셔", "빛 때문에 잠이 안 와", "눈이 부셔서 눈을 못 뜨겠어",
            "이제 누울게", "영화 보게 깜깜하게 해 줘", "밤인데 너무 환하다", "환해서 잠이 안 오네",
            "낮잠 좀 자려고", "이제 그만 자러 갈래", "하루 끝 이제 꿈나라 가야지", "오늘은 이만 잘래",
            "너무 밝아서 눈이 아파", "아무도 없는 방이 환하네", "나 이제 잔다", "침대에 누웠어 이제 잘 거야",
            "눈이 감긴다 이만 잘게", "밝아서 눈이 시려",
        ),
    ),
    "hidden/TV/INCREASE_VOLUME": (
        proposal("DEVICE_CONTROL", device="TV", action="INCREASE_VOLUME"),
        (TVCTX, DEFAULT, COMMON),
        (
            "뉴스 앵커 말이 잘 안 들리네", "영화 대사가 너무 작게 나와", "예능 소리가 너무 작다",
            "배우들 목소리가 하나도 안 들려", "화면 속 사람들이 속삭이는 것 같아", "축구 해설이 잘 안 들려",
            "설거지 소리 때문에 방송이 안 들려", "멀리 앉았더니 방송 소리가 작아", "다큐 내레이션이 잘 안 들린다",
            "채널 소리가 너무 작게 들려", "야구 중계가 너무 작게 나와", "애니메이션 소리가 작아서 애가 못 듣겠대",
            "청소기 소리에 묻혀서 방송이 안 들려", "주인공이 뭐라는지 모르겠어 너무 작아서",
            "뉴스 소리가 너무 작아서 귀를 기울이게 돼",
        ),
    ),
    "hidden/TV/DECREASE_VOLUME": (
        proposal("DEVICE_CONTROL", device="TV", action="DECREASE_VOLUME"),
        (TVCTX, DEFAULT, COMMON),
        (
            "방송 소리가 너무 커서 귀 아파", "광고 소리가 갑자기 커졌어", "예능 소리가 너무 시끄럽다",
            "옆집까지 들리겠다 방송 소리", "애 자는데 영화 소리가 너무 크네", "뉴스 소리가 쩌렁쩌렁 울려",
            "밤인데 방송 소리가 너무 요란해", "영화 효과음이 너무 커서 깜짝 놀랐어", "축구 중계 소리에 귀청 떨어지겠다",
            "방송 소리 때문에 대화가 안 돼", "음악 방송이 너무 시끄러워서 머리가 울려",
        ),
    ),
    "hidden/TV/TURN_ON": (
        proposal("DEVICE_CONTROL", device="TV", action="TURN_ON"),
        (DEFAULT, COMMON, TVCTX),
        (
            "야구 중계 시작할 시간이다", "저녁 뉴스 볼 시간이네", "좋아하는 드라마 할 시간이야",
            "오늘 예능 본방 사수해야지", "축구 경기 보고 싶다", "영화나 한 편 보고 싶네",
            "아침 뉴스 좀 보고 싶다", "일기예보 화면으로 보고 싶어", "월드컵 경기 곧 시작하지",
            "만화 보여 달라고 애가 졸라", "올림픽 중계 보고 싶다", "홈쇼핑이나 구경해 볼까",
            "경기 하이라이트 보고 싶어",
        ),
    ),
    "hidden/TV/TURN_OFF": (
        proposal("DEVICE_CONTROL", device="TV", action="TURN_OFF"),
        (TVCTX, DEFAULT),
        (
            "아무도 안 보는데 방송만 혼자 떠들고 있네", "드라마 끝났으니 그만 봐야겠다",
            "화면 그만 보고 책 읽어야지", "오늘 시청은 여기까지", "눈 아파서 이제 그만 볼래",
            "보는 사람도 없는데 계속 나오고 있네", "영화 다 봤다 이제 정리하자",
        ),
    ),
    "hidden/ALL_QUIET_DEVICES/SET_QUIET_MODE": (
        proposal("CONTEXT_SUGGESTION", device="ALL_QUIET_DEVICES", action="SET_QUIET_MODE"),
        (DEFAULT, DESK, COMMON),
        (
            "곧 화상회의 들어가", "이제 미팅 시작해", "팀 회의 들어갈게", "지금부터 온라인 수업이야",
            "면접 보기 시작할 거야", "중요한 통화 해야 해", "발표 연습 시작할게", "시험공부 시작한다",
            "지금부터 집중해서 일해야 해", "녹음 시작할 거야", "줌 미팅 5분 뒤에 시작해",
            "고객이랑 영상통화 할 거야", "인강 들어야 해", "아기 재우는 중이야", "명상하려고 해",
            "보고서 마감이라 집중 모드 들어간다", "화상 면접 시작이야", "라이브 방송 시작할게",
            "사장님이랑 통화 연결됐어", "모의고사 풀 거야", "원격 진료 들어가요",
            "웨비나 곧 시작이야", "지금 회의 중이야",
        ),
    ),
    "hidden/ALL_QUIET_DEVICES/RESUME": (
        proposal("DEVICE_CONTROL", device="ALL_QUIET_DEVICES", action="RESUME"),
        (DEFAULT, DESK),
        (
            "회의 끝났어", "미팅 다 끝났다", "통화 끝났어", "수업 끝", "발표 무사히 끝났다",
            "화상회의 방금 마쳤어", "면접 끝났어", "공부 다 했다", "녹음 끝났어",
        ),
    ),
    "hidden/WASHER/QUERY_STATUS": (
        proposal("DEVICE_QUERY", device="WASHER", action="QUERY_STATUS"),
        (DEFAULT, COMMON),
        (
            "빨래 다 됐나", "빨래 다 돌았는지 궁금하다", "빨래 언제 끝나지", "빨래 꺼내도 되나",
            "빨래 아직이야?", "옷 다 빨아졌나", "탈수 끝났으려나", "헹굼 끝났나 모르겠네",
        ),
    ),
    "hidden/ENTRY_DOOR/QUERY_STATUS": (
        proposal("DEVICE_QUERY", device="ENTRY_DOOR", action="QUERY_STATUS"),
        (DEFAULT, BED),
        (
            "현관 잠갔는지 모르겠네", "문단속 했나 모르겠다", "나올 때 현관 제대로 닫았나",
            "집 대문 잠겼는지 확인하고 싶어", "현관 쪽 문 열려 있으려나", "외출할 때 문 잠근 기억이 없어",
        ),
    ),
}

NO_ACTION_CHAT: tuple[str, ...] = (
    # food / daily life
    "저녁에 뭐 해 먹지", "치킨 시켜 먹을까", "떡볶이 먹고 싶다", "커피 한 잔 마셔야겠다",
    "냉장고에 먹을 게 하나도 없네", "배달 음식 너무 자주 먹었나", "오늘 아침 거르고 나왔어",
    "라면 끓여 먹을까 고민이다", "간식 뭐 남았지", "과일 좀 깎아 먹어야겠다", "다이어트는 내일부터 해야지",
    "김치찌개가 생각나네", "빵 사 올 걸 그랬다", "물을 너무 안 마셨네",
    # weather small talk / forecast
    "내일 비 온대", "주말엔 날씨 맑겠지", "벚꽃 피었더라", "단풍 예쁘게 들었네", "눈 오면 좋겠다",
    "하늘이 참 파랗다", "노을이 예쁘다", "올해 장마는 길대", "내일은 많이 덥대", "다음 주에 한파 온대",
    "요즘 일교차가 크네",
    # feelings
    "오늘 기분 최고야", "좀 우울하네", "심심하다", "행복하다", "괜히 설렌다", "스트레스 받아",
    "오늘 운이 좋았어", "긴장된다", "뿌듯하다", "조금 외롭네", "아 피곤하다", "웃겨 죽겠네",
    "마음이 편안하다", "괜히 짜증 난다",
    # plans / work
    "내일 출근하기 싫다", "이번 주말에 여행 갈까", "숙제가 산더미야", "보고서 언제 다 쓰지",
    "운동 가야 하는데 귀찮다", "택배가 언제 오려나", "친구한테 연락해야겠다", "병원 예약했어",
    "장 보러 가야 하는데", "이번 달 카드값 무섭다", "다음 달에 이사 가", "생일 선물 뭐 사지",
    "휴가 계획 세워야지", "머리 자르러 가야겠다", "자격증 공부 다시 해볼까",
    # talking to the assistant
    "지금 몇 시야", "오늘 무슨 요일이지", "너 이름이 뭐야", "고마워", "수고했어", "안녕",
    "반가워", "좋은 아침", "오늘 며칠이더라", "넌 참 똑똑하구나",
    # comfort / positive states
    "지금 온도 딱 좋다", "바람이 선선해서 기분 좋다", "방이 포근하고 좋다", "햇살 들어오니까 따뜻하네",
    "집이 제일 편하다", "청소하고 나니 깔끔하다", "향초 냄새 좋다", "커피 향이 좋네",
    "꽃 냄새 좋다", "오늘 집 분위기 괜찮다", "소파가 정말 푹신해", "이 담요 촉감 좋다",
    "창밖 풍경이 멋지다", "빗소리 듣기 좋다",
    # past or elsewhere
    "어제 회사가 엄청 더웠어", "지난주 캠핑장은 정말 추웠어", "학교 도서관은 너무 어두웠어",
    "카페 음악이 시끄러웠어", "작년 여름은 정말 더웠지", "영화관이 너무 추웠어",
    "버스 안이 찜통이었어", "지하철이 너무 붐볐다", "사무실 공기가 답답했어", "할머니 댁은 늘 따뜻해",
    "출장 간 호텔이 너무 건조했어",
    # media / hobbies talk
    "그 드라마 결말 대박이었어", "어제 본 영화 재밌었다", "이 노래 계속 듣게 되네",
    "요즘 웹툰 뭐가 재밌지", "축구 어제 이겼대", "책 한 권 다 읽었다", "새 게임 사고 싶다",
    "그 배우 연기 정말 잘하더라", "요즘 유행하는 노래 뭐야",
    # health / body
    "허리가 좀 아프네", "스트레칭 해야지", "손톱 깎아야겠다", "어깨가 뻐근하다", "눈이 좀 건조하네",
    "오늘 만 보 걸었어",
    # pets / family
    "강아지가 너무 귀엽다", "고양이 밥 챙겨야지", "동생이 곧 온대", "아빠 생신이 다음 주야",
    "조카가 많이 컸더라",
    # mild discomfort explicitly tolerated
    "덥긴 한데 참을 만해", "조금 쌀쌀하지만 괜찮아", "약간 어둡지만 이 정도면 됐어",
    "습하긴 한데 견딜 만하다", "살짝 시끄럽긴 한데 상관없어", "좀 덥지만 지금은 괜찮아",
    # misc statements
    "이 컵 예쁘다", "화분에 물 줘야겠다", "책상 정리 좀 해야겠네", "오늘 하루도 끝났다",
    "벌써 금요일이네", "시간 진짜 빨리 간다", "내일 일찍 나가야 해", "로또나 사 볼까",
    "운동화 새로 샀어", "주말에 뭐 하지", "옷장 정리해야 하는데", "택시 부를까",
)

HIGH_RISK_SENTENCES: tuple[str, ...] = (
    "부엌에서 뭔가 타고 있는 것 같아", "냄비가 새까맣게 탔어", "매캐한 냄새가 나",
    "집 안에 연기가 자욱해", "가스 새는 소리가 나", "누가 문을 억지로 열려고 해",
    "창밖에 수상한 사람이 서성거려", "낯선 사람이 문을 계속 두드려", "누가 도어락 비밀번호를 계속 눌러",
    "할머니가 넘어지셔서 못 일어나셔", "아이가 숨을 잘 못 쉬어", "전기 합선된 것 같아",
    "멀티탭이 엄청 뜨거워", "경보음이 울리고 있어", "화재경보기가 울려", "칼에 손을 크게 베었어",
    "집에 누가 들어온 것 같아", "전선 타는 듯한 냄새가 나", "콘센트에서 스파크가 튀었어",
    "베란다에 모르는 사람이 있어", "가스 밸브 잠갔는지 모르겠는데 냄새가 이상해",
)

# Negation / refusal forms that should never become a command, written so the
# guard regexes do not necessarily catch them (the model must handle them too).
NEGATED_SENTENCES: tuple[str, ...] = (
    "덥긴 한데 그냥 둬", "깜깜해도 괜찮으니까 그대로 놔둬", "냄새 나도 그냥 놔둬",
    "춥긴 한데 아무것도 건드릴 필요 없어", "어두워도 그냥 이대로가 좋아 아무것도 하지 말아 줘",
    "지금은 아무것도 켤 필요 없어", "아무것도 끌 필요 없어", "열 필요는 없을 것 같아",
    "닫을 필요 없어", "괜찮아 안 켜도 될 것 같아", "됐어 그냥 놔둬", "아무것도 하지 않았으면 좋겠어",
    "지금 상태 그대로 유지해 줘", "손대지 말고 그대로 둬", "건드리지 않았으면 해",
    "켤 생각 없어", "끌 생각 없어", "그냥 둬도 돼",
)

TRAIN_PREFIXES = ("아 ", "아이고 ", "하 ", "음 ", "어휴 ", "흠 ", "와 ", "근데 ", "에구 ")
ANNOUNCE_PREFIXES = ("자 ", "그럼 ", "이제 ", "나 ", "음 ", "아 ")
HELDOUT_PREFIXES = ("", "아휴 ", "으 ", "에휴 ", "")
CHAT_PREFIXES = ("", "그냥 ", "근데 ", "아 ", "음 ", "있잖아 ", "그러고 보니 ")


def split_of(index: int) -> str:
    position = index % 6
    if position == 4:
        return "validation"
    if position == 5:
        return "test"
    return "train"


def variants(core: str, split: str, rng: random.Random, prefixes: tuple[str, ...], train_count: int) -> list[str]:
    if split == "train":
        chosen = ["", *rng.sample(prefixes, train_count - 1)]
    else:
        chosen = [rng.choice(HELDOUT_PREFIXES)]
    return [f"{prefix}{core}" for prefix in chosen]


class Builder:
    def __init__(self) -> None:
        self.capabilities = load_json_object(SERVICE_ROOT / "config" / "device-capabilities.json")
        self.client = ExaoneClient(capabilities=self.capabilities)
        self.dropped: list[dict[str, Any]] = []
        self.candidate_records = 0

    def guard(self, utterance: str, context: dict[str, Any]) -> dict[str, Any] | None:
        return preflight_proposal(utterance, self.capabilities, context)

    def record(
        self,
        *,
        record_id: str,
        category: str,
        split: str,
        utterance: str,
        context: dict[str, Any],
        expected: dict[str, Any],
        candidate: dict[str, Any] | None = None,
        source: str,
    ) -> dict[str, Any]:
        expected = validate_proposal(expected, capabilities=self.capabilities)
        messages = self.client._payload(utterance.strip(), context, None, candidate)["messages"]
        return {
            "id": record_id,
            "category": category,
            "split": split,
            "source": source,
            "utterance": utterance,
            "context": context,
            "expected": expected,
            "has_candidate": candidate is not None,
            "prompt": messages,
            "completion": [
                {"role": "assistant", "content": json.dumps(expected, ensure_ascii=False, separators=(",", ":"))}
            ],
        }

    def new_record_allowed(self, utterance: str, context: dict[str, Any], expected: dict[str, Any], category: str) -> bool:
        guarded = self.guard(utterance, context)
        if guarded is None:
            return True
        same = all(guarded.get(k) == expected.get(k) for k in ("intent", "device", "action"))
        same = same and set(expected["safety_flags"]).issubset(guarded.get("safety_flags", []))
        if same and guarded["intent"] in {"NO_ACTION", "CLARIFY"}:
            # Guard answers it identically; harmless to keep for robustness.
            return True
        self.dropped.append(
            {
                "category": category,
                "utterance": utterance,
                "guard": {k: guarded.get(k) for k in ("intent", "device", "action", "safety_flags")},
            }
        )
        return False

    def from_v2(self, row: dict[str, Any], split: str) -> dict[str, Any]:
        context = validate_activity_context(row["context"])
        expected = row["expected"]
        candidate = None
        guarded = self.guard(row["utterance"], context)
        if guarded is not None and guarded["intent"] in {"DEVICE_CONTROL", "DEVICE_QUERY", "CONTEXT_SUGGESTION"}:
            guarded = validate_proposal(guarded, capabilities=self.capabilities)
            agrees = all(guarded.get(k) == expected.get(k) for k in ("intent", "device", "action", "parameters"))
            digest = hashlib.sha256(row["id"].encode("utf-8")).digest()[0]
            if agrees and digest % 2 == 0:
                candidate = guarded
                self.candidate_records += 1
        return self.record(
            record_id=f"v2-{row['id']}" + ("-cand" if candidate else ""),
            category=row["category"],
            split=split,
            utterance=row["utterance"],
            context=context,
            expected=expected,
            candidate=candidate,
            source="finetune-v2",
        )

    def new_records(self, rng: random.Random) -> dict[str, list[dict[str, Any]]]:
        out: dict[str, list[dict[str, Any]]] = {"train": [], "validation": [], "test": []}
        for label, (expected, contexts, cores) in HIDDEN.items():
            announce = "ALL_QUIET_DEVICES" in label
            for index, core in enumerate(cores):
                split = split_of(index)
                texts = variants(core, split, rng, ANNOUNCE_PREFIXES if announce else TRAIN_PREFIXES, 3)
                for v_index, text in enumerate(texts):
                    context = contexts[(index + v_index) % len(contexts)]
                    if not self.new_record_allowed(text, context, expected, label):
                        continue
                    out[split].append(
                        self.record(
                            record_id=f"v3-{label.replace('/', '-').lower()}-{index:02d}-{v_index}",
                            category=label,
                            split=split,
                            utterance=text,
                            context=context,
                            expected=expected,
                            source="v3-hidden",
                        )
                    )
        chat_contexts = (DEFAULT, COMMON, DESK, BED, SELF, TVCTX)
        for index, core in enumerate(NO_ACTION_CHAT):
            split = split_of(index)
            texts = variants(core, split, rng, CHAT_PREFIXES[1:], 2)
            for v_index, text in enumerate(texts):
                context = chat_contexts[(index + v_index) % len(chat_contexts)]
                expected = proposal("NO_ACTION")
                if not self.new_record_allowed(text, context, expected, "no_action/chat_v3"):
                    continue
                out[split].append(
                    self.record(
                        record_id=f"v3-no-action-{index:03d}-{v_index}",
                        category="no_action/chat_v3",
                        split=split,
                        utterance=text,
                        context=context,
                        expected=expected,
                        source="v3-no-action",
                    )
                )
        for name, sentences, expected in (
            (
                "safety/high_risk",
                HIGH_RISK_SENTENCES,
                proposal("CLARIFY", ambiguous=True, flags=["HIGH_RISK"], question=HIGH_RISK_Q),
            ),
            (
                "safety/negated_v3",
                NEGATED_SENTENCES,
                proposal("NO_ACTION", flags=["NEGATED_REQUEST"]),
            ),
        ):
            for index, core in enumerate(sentences):
                split = split_of(index)
                texts = variants(core, split, rng, ("", "저기 ", "지금 ", "근데 "), 2)
                for v_index, text in enumerate(texts):
                    context = (DEFAULT, COMMON, BED)[(index + v_index) % 3]
                    if not self.new_record_allowed(text, context, expected, name):
                        continue
                    out[split].append(
                        self.record(
                            record_id=f"v3-{name.replace('/', '-')}-{index:02d}-{v_index}",
                            category=name,
                            split=split,
                            utterance=text,
                            context=context,
                            expected=expected,
                            source="v3-safety",
                        )
                    )
        return out


def build(output_dir: Path, v2_dir: Path, seed: int) -> dict[str, Any]:
    rng = random.Random(seed)
    builder = Builder()
    new = builder.new_records(rng)
    manifest: dict[str, Any] = {
        "seed": seed,
        "base_dataset": str(v2_dir.relative_to(SERVICE_ROOT)).replace("\\", "/"),
        "prompt_format": "ExaoneClient._payload messages (current SYSTEM_PROMPT + few-shots + JSON user message); completion = compact proposal JSON",
        "acceptance_files_used_for_generation": [],
        "splits": {},
    }
    seen: dict[str, str] = {}
    output_dir.mkdir(parents=True, exist_ok=True)
    for split in ("train", "validation", "test"):
        base_rows = [
            json.loads(line)
            for line in (v2_dir / f"{split}.jsonl").read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        records = [builder.from_v2(row, split) for row in base_rows] + new[split]
        rng.shuffle(records)
        for record in records:
            key = json.dumps([record["utterance"].strip(), record["context"]], ensure_ascii=False, sort_keys=True)
            if key in seen and seen[key] != split:
                raise ValueError(f"cross-split duplicate: {record['utterance']}")
            seen[key] = split
        payload = "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in records)
        (output_dir / f"{split}.jsonl").write_text(payload, encoding="utf-8")
        categories: dict[str, int] = {}
        sources: dict[str, int] = {}
        for record in records:
            categories[record["category"]] = categories.get(record["category"], 0) + 1
            sources[record["source"]] = sources.get(record["source"], 0) + 1
        manifest["splits"][split] = {
            "count": len(records),
            "sha256": hashlib.sha256(payload.encode("utf-8")).hexdigest(),
            "sources": sources,
            "with_validated_local_candidate": sum(1 for r in records if r["has_candidate"]),
            "categories": dict(sorted(categories.items())),
        }
    manifest["dropped_by_guard_check"] = builder.dropped
    (output_dir / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=SERVICE_ROOT / "data" / "finetune-v3")
    parser.add_argument("--v2-dir", type=Path, default=SERVICE_ROOT / "data" / "finetune-v2")
    parser.add_argument("--seed", type=int, default=20261022)
    args = parser.parse_args()
    manifest = build(args.output_dir, args.v2_dir, args.seed)
    summary = {
        split: {k: v for k, v in info.items() if k != "categories"} for split, info in manifest["splits"].items()
    }
    print(json.dumps({"splits": summary, "dropped": len(manifest["dropped_by_guard_check"])}, ensure_ascii=False, indent=2))
    for item in manifest["dropped_by_guard_check"]:
        print("DROPPED", json.dumps(item, ensure_ascii=False))


if __name__ == "__main__":
    main()
