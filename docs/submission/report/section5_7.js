// 5. 차별성 (3쪽), 6. 파급력 (2쪽), 7. 일정·업무 분장 (2쪽), Thank you
// Plan figures come from the team's 예선 개발계획서 v2 (2026-08-23); results follow the same 실기/PC rule as the rest.
const { TEAM, WORK, HEX, RULE, MUTED, THEME, content, text, box, chip, badge, tableRows } = require("./lib");

const S5 = "5. 개발 결과물의 차별성";
const S6 = "6. 개발 결과물의 파급력 및 기대효과";
const S7 = "7. 개발 일정 및 업무 분장";
const AMBER = "B07D12";

// ------------------------------------------------------------------ 5. 차별성
async function comparison(pres) {
  pres.addSection({ title: S5 });
  const s = content(pres, S5, "기존 스마트홈과 무엇이 다른가");
  const ours = (t) => ({ text: t, options: { bold: true, color: HEX.dk2, fill: { color: "E2F0F2" } } });
  const rows = [
    ["반응 시점", "말이 끝난 뒤 처리 시작", "정해 둔 조건·시간에 실행", ours("말 중간에 대상 기기를 미리 준비(부분 발화)")],
    ["이해 범위", "정해진 명령 문장 위주", "미리 만든 조건만", ours("“좀 덥네” 같은 숨은 의도까지(EXAONE)")],
    ["상황 인지", "명령이 있어야 동작", "센서 하나의 조건", ours("소리 상황 + 카메라 위치를 합쳐 회의·취침 판단")],
    ["개인정보", "음성을 클라우드에서 처리하는 경우가 많음", "앱·서버 계정에 기록", ours("음성·영상 원본은 집 밖으로 나가지 않음")],
    ["인터넷이 끊기면", "음성 기능 대부분 중단", "클라우드 연동 기능 중단", ours("집 안 Pi에서 그대로 동작")],
    ["실행 안전", "알아들으면 바로 실행", "조건이 맞으면 바로 실행", ours("AI는 제안만, core 정책과 사람 승인을 거쳐 실행")],
  ];
  const table = tableRows(["구분", "음성 비서형 스마트홈", "앱·루틴 자동화", WORK], rows);
  table[0][3].options.fill = { color: HEX.accent2 };
  s.addTable(table, {
    x: 0.5, y: 1.28, w: 9.0, colW: [1.4, 2.35, 2.2, 3.05], fontSize: 10, fontFace: THEME.bodyFontFace,
    rowH: [0.36, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5], margin: [0.03, 0.1, 0.03, 0.1],
    border: { type: "solid", pt: 0.75, color: RULE }, objectName: "comparison-table",
  });
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 0.5, y: 4.72, w: 9.0, h: 0.42, rectRadius: 0.08, fill: { color: HEX.dk2 }, line: { type: "none" }, objectName: "message" });
  text(s, [
    { text: "명령을 기다리는 집", options: { color: "B8D3D8" } },
    { text: "  →  ", options: { color: HEX.accent4, bold: true } },
    { text: "먼저 알아채되, 실행은 안전하게 확인하는 집", options: { bold: true } },
  ], { x: 0.5, y: 4.72, w: 9.0, h: 0.42, fontSize: 12.5, color: HEX.lt1, align: "center", valign: "middle" });
  s.addNotes("기존 제품 설명은 일반적인 동작 방식 기준(제품명은 쓰지 않음). 계획서의 차별점(Task 생명주기·우선순위)을 현재 구현 기준으로 다시 정리.");
}

async function planVsResult(pres) {
  const s = content(pres, S5, "개발계획서 목표 대비 결과");
  const verdict = (t, color) => ({ text: t, options: { bold: true, color, align: "center" } });
  const rows = [
    ["인텐트 인식 정확도\n90% 이상", "부분 발화 분류 98.3%(독립 60문장) · EXAONE 자연 문장 100/100 · 숨은 의도 20/22", verdict("달성(PC)", HEX.accent6)],
    ["엣지 추론 지연\n300 ms 이하", "부분 발화 분류 P95 0.63 ms(PC). 실기: 음성 인식 3.0초(3초 발화), EXAONE 한 문장 15~20초", verdict("부분 달성", AMBER)],
    ["보류 Task 재개\n정확도 90% 이상", "core 작업 상태에 보류·재개 동작 구현. 세탁기 모형이 빠져 재개 정확도 시험은 하지 않음", verdict("미측정", HEX.accent5)],
    ["오탐률 5% 이하", "조용 모드 오탐 4.89% · 부분 발화가 실제 실행으로 이어진 오탐 0%", verdict("달성(PC)", HEX.accent6)],
    ["안전 개입 지연\n1초 이하", "안전 알림 수신·저장·확인 경로 구현. 가스 센서가 모형에서 빠져 지연 시험은 하지 않음", verdict("미측정", HEX.accent5)],
  ];
  s.addTable(tableRows(["계획서 목표", "결과", "판정"], rows), {
    x: 0.5, y: 1.28, w: 6.0, colW: [1.55, 3.45, 1.0], fontSize: 9.5, fontFace: THEME.bodyFontFace,
    rowH: [0.32, 0.68, 0.68, 0.68, 0.6, 0.68], margin: [0.03, 0.08, 0.03, 0.08],
    border: { type: "solid", pt: 0.5, color: RULE }, objectName: "plan-table",
  });
  box(s, pres, { x: 6.7, y: 1.28, w: 2.8, h: 3.86, fill: HEX.lt2, name: "changes" });
  text(s, "계획에서 바뀐 것", { x: 6.85, y: 1.38, w: 2.5, h: 0.28, fontSize: 12, bold: true, color: HEX.dk2 });
  const changes = [
    ["LLM", "PC GPU 노드(7.8B~32B) → Pi5 안 EXAONE 1.2B LoRA", "PC·인터넷 없이 집 안에서만 동작"],
    ["허브", "Node.js → C++ 네이티브 서비스", "LG AI Native OS 앱 환경"],
    ["영상", "ESP32-CAM → Pi5 + USB 웹캠 2대", "플랫폼 Camera1 API 사용"],
    ["시연", "세탁 보류→재개 → 회의 조용 모드·정정", "모형 기기 정리(계약 v4)"],
  ];
  const runs = [];
  changes.forEach(([head, what, why], i) => {
    runs.push({ text: head, options: { bold: true, color: HEX.accent2, breakLine: true } });
    runs.push({ text: what, options: { color: HEX.dk1, breakLine: true } });
    runs.push({ text: `이유: ${why}`, options: { color: MUTED, fontSize: 8.5, breakLine: i < changes.length - 1 } });
  });
  text(s, runs, { x: 6.85, y: 1.74, w: 2.55, h: 3.3, fontSize: 9.5, paraSpaceAfter: 2 });
  s.addNotes("목표 5개는 예선 개발계획서 v2의 정량 목표 그대로. 수치 근거: partial-intent metrics-final-pc-v3, EXAONE v3 평가(PROGRESS 10-22), context-fusion README, 실기 STT·LLM(PROGRESS 10-23·10-25).");
}

async function strengths(pres) {
  const s = content(pres, S5, "작품의 우수성");
  const cols = [
    { icon: "MdWifiOff", head: "인터넷 없이, 집 안에서", items: [
      ["클라우드 서버 0대", "STT·LLM·소리 상황·영상 위치 모두 집 안 Pi"],
      ["EXAONE 4.0 1.2B LoRA", "Q4_K_M 812 MB, Pi5에서 실행"],
      ["Whisper small LoRA", "q5_0 175 MB, 한국어 집 안 명령 학습"],
      ["부분 발화 분류", "ONNX 148 KB"],
    ] },
    { icon: "MdSpeed", head: "실기에서 잰 결과", items: [
      ["음성 인식 13.8초 → 3.0초", "“거실 불 켜줘” 3초 발화(입력 창 조정)"],
      ["명확한 명령은 LLM 생략", "규칙 파서가 바로 작업 요청"],
      ["EXAONE 한 문장 15~20초", "애매한 말에만 사용, 예열로 첫 요청 성공"],
      ["두 카메라 · 7인치 실시간 영상", "Camera1 API, 재부팅 뒤 자동 복구"],
    ] },
    { icon: "MdVerifiedUser", head: "안전하게 실행", items: [
      ["AI는 제안만", "Pi4 core의 정책이 최종 결정"],
      ["현관문 열기", "집 안 7인치에서만 승인, 2분 뒤 만료"],
      ["명령 한 번만", "중복 차단 · 만료 거절 · ACK 3회 재전송"],
      ["계정별 MQTT 권한", "인증 포트 · ACL · 서명된 설치 패키지"],
    ] },
  ];
  for (let i = 0; i < cols.length; i++) {
    const c = cols[i];
    const x = 0.5 + i * 3.05;
    box(s, pres, { x, y: 1.28, w: 2.9, h: 3.86, fill: i === 1 ? "E2F0F2" : HEX.lt2, name: c.head });
    await badge(s, pres, c.icon, x + 0.16, 1.42, 0.5, i === 2 ? HEX.accent2 : HEX.accent3, HEX.lt1);
    text(s, c.head, { x: x + 0.78, y: 1.42, w: 2.05, h: 0.5, fontSize: 13, bold: true, color: HEX.dk2, valign: "middle" });
    c.items.forEach(([head, body], k) => {
      const y = 2.12 + k * 0.74;
      text(s, head, { x: x + 0.18, y, w: 2.6, h: 0.26, fontSize: 10.5, bold: true, color: HEX.dk1 });
      text(s, body, { x: x + 0.18, y: y + 0.27, w: 2.6, h: 0.4, fontSize: 9, color: MUTED });
    });
  }
  s.addNotes("가운데 열은 모두 실기 기록(PROGRESS 10-23 STT, 10-25 LLM·카메라). LLM 15~20초는 1.8 GHz 제한 상태.");
}

// ------------------------------------------------------------------ 6. 파급력
async function targets(pres) {
  pres.addSection({ title: S6 });
  const s = content(pres, S6, "누가 쓰는가: 활용 대상과 시장성");
  const cards = [
    { icon: "MdPerson", head: "1인 가구", stat: "35.5%", source: "전체 가구 중 1인 가구 (2023, 통계청 「2024 통계로 보는 1인가구」)", body: "혼잣말 같은 짧은 말에도 집이 먼저 반응하고, 휴대폰으로 밖에서도 확인·승인" },
    { icon: "MdElderly", head: "고령자", stat: "20.0%", source: "65세 이상 주민등록 인구 (2024.12, 행정안전부) → 초고령사회", body: "앱을 몰라도 말로 조작, 현관문처럼 위험한 동작은 화면 승인으로 한 번 더 확인" },
    { icon: "MdHeadsetMic", head: "재택근무·학습", stat: "회의 감지", source: "소리 상황 + 카메라 위치", body: "회의·통화가 시작되면 가전 소음과 알림을 자동으로 줄이고, 틀리면 한마디로 되돌림" },
  ];
  for (let i = 0; i < cards.length; i++) {
    const c = cards[i];
    const x = 0.5 + i * 3.05;
    box(s, pres, { x, y: 1.28, w: 2.9, h: 2.72, fill: HEX.lt2, name: c.head });
    await badge(s, pres, c.icon, x + 0.16, 1.42, 0.48, HEX.accent3, HEX.lt1);
    text(s, c.head, { x: x + 0.76, y: 1.42, w: 2.0, h: 0.48, fontSize: 13, bold: true, color: HEX.dk2, valign: "middle" });
    text(s, c.stat, { x: x + 0.16, y: 2.0, w: 2.6, h: 0.5, fontSize: c.stat.length > 6 ? 20 : 26, bold: true, color: HEX.accent2, valign: "middle" });
    text(s, c.source, { x: x + 0.16, y: 2.5, w: 2.6, h: 0.4, fontSize: 8, color: HEX.accent5 });
    text(s, c.body, { x: x + 0.16, y: 2.96, w: 2.6, h: 0.86, fontSize: 9.5, color: MUTED });
  }
  box(s, pres, { x: 0.5, y: 4.16, w: 9.0, h: 0.96, fill: "F7FAFA", line: "DCE4E7", name: "market" });
  chip(s, pres, 0.68, 4.3, 1.1, 0.3, "시장성", HEX.dk2, HEX.lt1, 10);
  text(s, [
    { text: "기존 가전 플랫폼 위에 얹는 ‘생활 Task 관리 계층’", options: { bold: true, color: HEX.dk2, breakLine: true } },
    { text: "새 가전은 MQTT 계약(pfn.v1)에 기기와 동작만 등록하면 같은 인식·판단·승인 경로를 그대로 사용합니다. LG AI Native OS의 설치 패키지(IPK)로 배포되고, 추론 서버가 없어 판매 뒤 운영비 부담이 적습니다." },
  ], { x: 1.95, y: 4.27, w: 7.4, h: 0.8, fontSize: 10, color: MUTED, paraSpaceAfter: 2 });
  s.addNotes("통계 출처: 통계청 「2024 통계로 보는 1인가구」(2023년 782.9만 가구, 35.5%), 행정안전부 2024-12-24 발표(65세 이상 1,024만 명, 주민등록 인구의 20.00%).");
}

async function valueAndRoadmap(pres) {
  const s = content(pres, S6, "판매 가치 · 기대효과 · 발전 가능성");
  const col = (x, w, head, iconName, lines) => {
    box(s, pres, { x, y: 1.28, w, h: 1.7, fill: HEX.lt2, name: head });
    return badge(s, pres, iconName, x + 0.14, 1.4, 0.44, HEX.accent3, HEX.lt1).then(() => {
      text(s, head, { x: x + 0.68, y: 1.4, w: w - 0.8, h: 0.44, fontSize: 12.5, bold: true, color: HEX.dk2, valign: "middle" });
      text(s, lines.map((l, i) => ({ text: l, options: { bullet: { indent: 12 }, breakLine: i < lines.length - 1 } })),
        { x: x + 0.16, y: 1.96, w: w - 0.3, h: 0.9, fontSize: 9.5, color: MUTED, paraSpaceAfter: 3 });
    });
  };
  await col(0.5, 4.4, "판매 가치", "MdSell", [
    "클라우드 추론 서버가 필요 없어 판매 뒤 운영비가 적음",
    "음성·영상이 집 밖으로 나가지 않아 개인정보 우려를 줄임",
    "정책·승인을 계약으로 분리해 가전이 늘어도 같은 구조로 확장",
  ]);
  await col(5.1, 4.4, "기대효과", "MdEmojiObjects", [
    "사용자: 조작 횟수 감소, 상황에 맞는 가전 동작, 사생활 보호",
    "제조사: 온디바이스 AI 가전으로 차별화, 서버 비용 절감",
    "사회: 1인·고령 가구의 생활 보조와 안전 확인",
  ]);
  text(s, "발전 가능성 · 로드맵", { x: 0.5, y: 3.2, w: 4, h: 0.26, fontSize: 11.5, bold: true, color: HEX.accent2 });
  const steps = [
    ["단기 · 대회 뒤", "실기 현장 시험(마이크·카메라 융합) · ESP 실기 연결 · 집 밖 휴대폰 원격 접속"],
    ["중기", "실제 가전 제어 보드 연동 · 보류→재개 Task와 안전 센서(가스·낙상) 다시 넣기"],
    ["장기", "가전 내장 NPU로 LLM 응답 단축 · 가족 구성원별 개인화"],
  ];
  s.addShape(pres.shapes.LINE, { x: 0.74, y: 3.74, w: 8.6, h: 0, line: { color: HEX.accent3, width: 2 } });
  steps.forEach(([head, body], i) => {
    const x = 0.5 + i * 3.05;
    s.addShape(pres.shapes.OVAL, { x: x + 0.12, y: 3.62, w: 0.24, h: 0.24, fill: { color: i === 0 ? HEX.accent2 : HEX.accent3 }, line: { color: HEX.lt1, width: 2 } });
    box(s, pres, { x, y: 3.98, w: 2.9, h: 1.1, fill: i === 0 ? "FCEEE6" : "F7FAFA", line: "DCE4E7", title: head, titleSize: 11, titleH: 0.26, size: 9.5, lines: [body], name: head });
  });
  s.addNotes("로드맵 단기 항목은 현재 남은 일(PROGRESS): 비전 빈 장면 촬영 후 융합 실기 시험, ESP 업로드, 원격 접속(사용자 결정: 맨 나중).");
}

// ------------------------------------------------------------------ 7. 일정·업무 분장
// Weekly grid like the guide's example: 7월~10월, 4 columns per month (week 4 = 22일~말일).
// plan: [from, to] week numbers from the 계획서 table; done: actual spans from file dates, PROGRESS and commits.
// done (week: evidence): 8 plan v2·소리 상황 시작 8/23 · 9 EXAONE LoRA 9/2, 부분 발화·ID 정의 9/3, Whisper LoRA·Pi4 core 0.5.2 9/4
// · 11 AI 현황 9/16 · 12 전장 확정 9/29 · 15 인수인계·계약 통합 10/21 · 16 C++ 이식·실기 설치·계약 v4·보고서 10/22~
const SCHEDULE = [
  { item: "요구사항 정의 · 아키텍처 설계", plan: [[1, 2]], done: [[8, 9]] },
  { item: "MQTT 계약 · Task 상태 모델 · 데이터 수집", plan: [[2, 4]], done: [[8, 9], [15, 15]] },
  { item: "HW 구성 · MQTT 백본(Pi4 core)", plan: [[3, 5]], done: [[9, 9], [12, 12]] },
  { item: "AI 모델 학습(EXAONE·Whisper·소리 상황)", plan: [[6, 7]], done: [[8, 11], [16, 16]] },
  { item: "판단·정책 · 상황 규칙(조용 모드)", plan: [[7, 9]], done: [[9, 11], [15, 16]] },
  { item: "C++ 네이티브 이식 · UI · 휴대폰 앱", plan: [[9, 14]], done: [[15, 16]] },
  { item: "실기 설치 · 현장 시험 · 하드웨어 변경 대응", plan: [[15, 16]], done: [[16, 16]] },
  { item: "측정 · 검증 · 보고서 · 시연 준비", plan: [[4, 4], [8, 8], [11, 12], [14, 16]], done: [[11, 11], [16, 16]] },
];

async function schedule(pres) {
  pres.addSection({ title: S7 });
  const s = content(pres, S7, "개발 일정 (계획 대비 실제)");
  const x0 = 0.5, labelW = 2.85, top = 1.28, headH = 0.26, rowH = 0.38;
  const cellW = (9.0 - labelW) / 16;
  const months = ["7월", "8월", "9월", "10월"];
  months.forEach((m, i) => {
    const x = x0 + labelW + i * 4 * cellW;
    s.addShape(pres.shapes.RECTANGLE, { x, y: top, w: 4 * cellW, h: headH, fill: { color: HEX.dk2 }, line: { color: HEX.lt1, width: 1 } });
    text(s, m, { x, y: top, w: 4 * cellW, h: headH, fontSize: 9.5, bold: true, color: HEX.lt1, align: "center", valign: "middle" });
    for (let w = 0; w < 4; w++) {
      text(s, `${w + 1}주`, { x: x + w * cellW, y: top + headH, w: cellW, h: 0.2, fontSize: 7, color: HEX.accent5, align: "center", valign: "middle" });
    }
  });
  s.addShape(pres.shapes.RECTANGLE, { x: x0, y: top, w: labelW, h: headH, fill: { color: HEX.dk2 }, line: { color: HEX.lt1, width: 1 } });
  text(s, "내용", { x: x0, y: top, w: labelW, h: headH, fontSize: 9.5, bold: true, color: HEX.lt1, align: "center", valign: "middle" });
  const gridTop = top + headH + 0.22;
  SCHEDULE.forEach((row, r) => {
    const y = gridTop + r * rowH;
    s.addShape(pres.shapes.RECTANGLE, { x: x0, y, w: 9.0, h: rowH, fill: { color: r % 2 ? "F7FAFA" : HEX.lt1 }, line: { color: "E3E8EA", width: 0.5 } });
    text(s, `${r + 1}. ${row.item}`, { x: x0 + 0.08, y, w: labelW - 0.1, h: rowH, fontSize: 9, color: HEX.dk1, valign: "middle" });
    for (const [a, b] of row.plan) {
      s.addShape(pres.shapes.RECTANGLE, { x: x0 + labelW + (a - 1) * cellW + 0.02, y: y + 0.05, w: (b - a + 1) * cellW - 0.04, h: 0.12, fill: { color: "F6D5C3" }, line: { type: "none" } });
    }
    for (const [a, b] of row.done) {
      s.addShape(pres.shapes.RECTANGLE, { x: x0 + labelW + (a - 1) * cellW + 0.02, y: y + 0.2, w: (b - a + 1) * cellW - 0.04, h: 0.13, fill: { color: HEX.accent3 }, line: { type: "none" } });
    }
  });
  for (let w = 0; w <= 16; w++) {
    const x = x0 + labelW + w * cellW;
    s.addShape(pres.shapes.LINE, { x, y: gridTop, w: 0, h: SCHEDULE.length * rowH, line: { color: w % 4 === 0 ? "C9D3D8" : "EEF1F2", width: w % 4 === 0 ? 0.75 : 0.5 } });
  }
  const legendY = gridTop + SCHEDULE.length * rowH + 0.12;
  s.addShape(pres.shapes.RECTANGLE, { x: x0, y: legendY + 0.05, w: 0.3, h: 0.12, fill: { color: "F6D5C3" }, line: { type: "none" } });
  text(s, "계획(예선 개발계획서)", { x: x0 + 0.38, y: legendY, w: 1.8, h: 0.22, fontSize: 8.5, color: MUTED, valign: "middle" });
  s.addShape(pres.shapes.RECTANGLE, { x: x0 + 2.2, y: legendY + 0.05, w: 0.3, h: 0.12, fill: { color: HEX.accent3 }, line: { type: "none" } });
  text(s, "실제(작업 기록이 있는 기간)", { x: x0 + 2.58, y: legendY, w: 2.0, h: 0.22, fontSize: 8.5, color: MUTED, valign: "middle" });
  text(s, "9/2 EXAONE LoRA · 9/4 Pi4 core 통합 · 9/29 전장 확정 · 10/23 실기 설치", { x: x0 + 4.6, y: legendY, w: 4.4, h: 0.22, fontSize: 8, color: HEX.accent5, valign: "middle", align: "right" });
  s.addNotes("실제 막대는 이 PC의 파일 날짜·PROGRESS·커밋에 기록이 있는 주만 칠했다. 7월~8월 3주와 9월 말~10월 중순의 팀원 작업(다른 PC·기기)은 기록이 없어 비어 있으니 팀이 채울 것.");
}

async function roles(pres) {
  const s = content(pres, S7, "업무 분장");
  // [구분, 성명, 담당 분야, 업무 분장, card icon, card label]
  const rows = [
    ["팀장", "박영만", "하드웨어 구성, 펌웨어, 센서·제어 노드", "전장 구성·배선, ESP32(현관문·창문) · ESP8266(조명·팬) 펌웨어, 모형 제작", "MdMemory", "HW·펌웨어"],
    ["팀원", "안형민", "로컬 AI 추론, 개인화, 사용자 UI", "EXAONE LoRA · Whisper · 소리 상황 · 영상 위치, 7인치 UI", "MdPsychology", "AI·UI"],
    ["팀원", "김기원", "허브 소프트웨어, 엣지 AI 통합, 하드웨어 연동", "Pi4 core(검증·정책·작업), MQTT 브로커·ACL, 휴대폰 API, 기기 설치·배포", "MdHub", "허브·연동"],
    ["팀원", "박준우", "기획, 시스템 설계, 문제 정의, 프로젝트 관리", "시나리오·문제 정의, 메시지 계약·ID 정의, 일정 관리, 보고서·시연", "MdAssignment", "기획·관리"],
    ["팀원", "박정빈", "모바일 애플리케이션", "휴대폰 앱(QR 페어링 · 장치 상태·제어 · 승인 요청 알림)", "MdPhoneIphone", "모바일 앱"],
  ];
  const body = rows.map((r, i) => [String(i + 1), r[0], r[1], r[2], r[3]]);
  s.addTable(tableRows(["No.", "구분", "성명", "담당 분야", "참여 인원의 업무 분장"], body, { firstBold: false }), {
    x: 0.5, y: 1.28, w: 9.0, colW: [0.5, 0.8, 1.0, 2.5, 4.2], fontSize: 10, fontFace: THEME.bodyFontFace,
    rowH: [0.34, ...rows.map(() => 0.5)], margin: [0.03, 0.1, 0.03, 0.1], align: "center",
    border: { type: "solid", pt: 0.75, color: RULE }, objectName: "roles-table",
  });
  for (let i = 0; i < rows.length; i++) {
    const x = 0.5 + i * 1.83;
    box(s, pres, { x, y: 4.28, w: 1.71, h: 0.84, fill: i === 0 ? "FCEEE6" : HEX.lt2, name: rows[i][5] });
    await badge(s, pres, rows[i][4], x + 0.11, 4.45, 0.5, i === 0 ? HEX.accent2 : HEX.accent3, HEX.lt1);
    text(s, rows[i][5], { x: x + 0.68, y: 4.36, w: 1.0, h: 0.32, fontSize: 10.5, bold: true, color: HEX.dk2, valign: "middle" });
    text(s, `${rows[i][0]}  ${rows[i][1]}`, { x: x + 0.68, y: 4.68, w: 1.0, h: 0.28, fontSize: 9, color: MUTED, valign: "middle" });
  }
  s.addNotes("담당 분야는 예선 개발계획서의 팀 구성표(4명), 박정빈은 사용자가 알려 준 담당(모바일 애플리케이션). 업무 분장은 실제 구현물 기준으로 정리.");
}

async function thanks(pres) {
  const s = pres.addSlide({ masterName: "END", sectionTitle: S7 });
  s.addText("감사합니다", { placeholder: "title" });
  s.addText(`${WORK}  ·  ${TEAM}`, { placeholder: "body" });
}

module.exports = async function sections5to7(pres) {
  await comparison(pres);
  await planVsResult(pres);
  await strengths(pres);
  await targets(pres);
  await valueAndRoadmap(pres);
  await schedule(pres);
  await roles(pres);
  await thanks(pres);
};
