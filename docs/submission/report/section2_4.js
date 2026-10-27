// 2. 개발 환경 (4쪽), 3. 개발 프로그램 설명 (3쪽), 3-1. 가산점 (1쪽), 4. 장애요인과 해결방안 (2쪽)
// Numbers: "실기" only where docs/handoff/PROGRESS.md records a real-device run; everything else says PC.
const path = require("path");
const { HEX, RULE, MUTED, THEME, content, text, box, arrow, chip, badge, tableRows } = require("./lib");

const S2 = "2. 개발 환경 설명";
const S3 = "3. 개발 프로그램 설명";
const S31 = "3-1. 가산점 항목 관련 내용";
const S4 = "4. 개발 중 발생한 장애요인과 해결방안";

// ------------------------------------------------------------------ 2. 개발 환경
async function hardwareMap(pres) {
  pres.addSection({ title: S2 });
  const s = content(pres, S2, "집 공유기 하나로 묶인 다섯 대의 기기");
  const top = [
    { x: 0.5, title: "Raspberry Pi 5 · 인식", id: "NODE-RPI5-02", lines: ["USB 마이크 · USB 웹캠 2대(방·현관)", "음성 인식 · 소리 상황 · 영상 위치"] },
    { x: 3.65, title: "Raspberry Pi 4 · 백엔드", id: "NODE-RPI4-01", lines: ["MQTT 브로커(인증·ACL) · core", "검증 · 정책 · 작업 관리 · 휴대폰 API"] },
    { x: 6.8, title: "Raspberry Pi 5 · UI·LLM", id: "NODE-RPI5-01", lines: ["7인치 터치 디스플레이", "EXAONE 4.0 1.2B · 카메라 영상 보기"] },
  ];
  const bottom = [
    { x: 0.5, title: "ESP32 · 현관문·창문", id: "CTRL-ESP32-ACT-01", lines: ["현관문 GPIO19 · 창문 GPIO21", "열기/닫기 모터, 명령 → 실행 → ACK"] },
    { x: 3.65, title: "ESP8266 · 조명·팬", id: "CTRL-ESP8266-LED-01", lines: ["D0 거실 조명 · D1 에어컨 표시등", "D2 화장실 환기팬(MOSFET)"] },
    { x: 6.8, title: "휴대폰 앱 (Android)", id: "Flutter", lines: ["QR 페어링 · 토큰 인증", "기기 제어 · 승인/거절 · 알림"] },
  ];
  const card = (c, y, h, osChip) => {
    box(s, pres, { x: c.x, y, w: 2.7, h, fill: HEX.lt2, title: c.title, titleSize: 12, lines: [], name: c.title });
    chip(s, pres, c.x + 0.12, y + 0.46, osChip ? 1.38 : 1.62, 0.24, osChip ? "LG AI Native OS" : c.id, osChip ? HEX.accent3 : "D5DEE1", osChip ? HEX.lt1 : HEX.dk2, 8.5);
    if (osChip) text(s, c.id, { x: c.x + 1.58, y: y + 0.46, w: 1.05, h: 0.24, fontSize: 8, color: HEX.accent5, valign: "middle" });
    text(s, c.lines.map((l, i) => ({ text: l, options: { breakLine: i < c.lines.length - 1 } })), { x: c.x + 0.12, y: y + 0.8, w: 2.46, h: h - 0.86, fontSize: 9.5, color: MUTED, paraSpaceAfter: 2 });
  };
  top.forEach((c) => card(c, 1.28, 1.42, true));
  bottom.forEach((c) => card(c, 3.78, 1.32, false));
  // router
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 0.5, y: 3.0, w: 9.0, h: 0.5, rectRadius: 0.08, fill: { color: HEX.dk2 }, line: { type: "none" }, objectName: "router" });
  text(s, [
    { text: "집 공유기", options: { bold: true } },
    { text: "   유선 LAN(Pi) · 2.4 GHz Wi-Fi(ESP·휴대폰) · MQTT pfn.v1 계약(Pi4 브로커)" },
  ], { x: 0.5, y: 3.0, w: 9.0, h: 0.5, fontSize: 11, color: HEX.lt1, align: "center", valign: "middle" });
  for (const x of [1.85, 5.0, 8.15]) {
    arrow(s, pres, x, 2.7, x, 3.0, { noHead: true, color: HEX.accent3, width: 2 });
    arrow(s, pres, x, 3.5, x, 3.78, { noHead: true, color: HEX.accent4, width: 2, dash: "dash" });
  }
  text(s, "LAN", { x: 1.92, y: 2.74, w: 0.5, h: 0.2, fontSize: 8, bold: true, color: HEX.accent3 });
  text(s, "Wi-Fi", { x: 1.92, y: 3.54, w: 0.6, h: 0.2, fontSize: 8, bold: true, color: "B07D12" });
  s.addNotes("구성은 registry v4(2026-10-25): ESP32 문·창문, ESP8266 조명·에어컨 표시등·팬. Pi는 모두 LG AI Native OS(Atlas).");
}

async function hardwareTable(pres) {
  const s = content(pres, S2, "HW 구성과 기능");
  const rows = tableRows(["기기", "역할", "연결 장치", "실행 환경"], [
    ["Raspberry Pi 4\nNODE-RPI4-01", "백엔드: MQTT 브로커, core(검증·정책·작업), DB, 휴대폰 API", "없음(유선 LAN)", "LG AI Native OS (Headless)"],
    ["Raspberry Pi 5\nNODE-RPI5-01", "7인치 UI, EXAONE LLM, 카메라 영상 보기", "7인치 터치 디스플레이", "LG AI Native OS"],
    ["Raspberry Pi 5\nNODE-RPI5-02", "음성 인식, 소리 상황, 영상 위치, 상황 융합", "USB 마이크, USB 웹캠 2대", "LG AI Native OS"],
    ["ESP32\nCTRL-ESP32-ACT-01", "현관문·창문 열기/닫기", "열기/닫기 모터 2개(GPIO19·21)", "Arduino 펌웨어\n(PlatformIO)"],
    ["ESP8266\nCTRL-ESP8266-LED-01", "거실 조명, 에어컨 표시등, 화장실 환기팬", "LED 2개(D0·D1), 12V 팬(D2·MOSFET)", "Arduino 펌웨어\n(PlatformIO)"],
  ]);
  s.addTable(rows, {
    x: 0.5, y: 1.28, w: 6.35, colW: [1.8, 1.8, 1.5, 1.25], fontSize: 9.5, fontFace: THEME.bodyFontFace,
    rowH: [0.34, 0.62, 0.56, 0.56, 0.56, 0.62], margin: [0.03, 0.08, 0.03, 0.08],
    border: { type: "solid", pt: 0.75, color: RULE }, objectName: "hw-table",
  });
  s.addImage({ path: path.join(__dirname, "assets", "model-photo.jpg"), x: 7.0, y: 1.28, w: 2.5, h: 1.6, altText: "실제 모형 사진", objectName: "model-photo" });
  text(s, "실제 모형 (2026-10-27 촬영 영상의 한 장면)", { x: 7.0, y: 2.92, w: 2.5, h: 0.26, fontSize: 8.5, color: MUTED, align: "center", valign: "middle" });
  box(s, pres, {
    x: 7.0, y: 3.3, w: 2.5, h: 1.8, title: "전원", titleSize: 11.5, size: 9.5,
    lines: ["12V SMPS → 퓨즈 → 강압 5.1V(모터) · 5.0V(ESP)", "Pi5: 추론 중 1.8 GHz 제한 → 실행 중 최저 4.887 V(실기 측정)"],
  });
  s.addNotes("모형 사진은 2026-10-27 실기 촬영 영상(footage/raw, 21_32 (3) 26초)에서 뽑은 정지 화면. 4.887 V는 2026-10-25 실기 측정(PROGRESS).");
}

async function softwareStack(pres) {
  const s = content(pres, S2, "SW 구성: 기기마다 C++ 네이티브 서비스");
  const cols = [
    { x: 0.5, head: "Pi5-02 · 인식", heights: [1.0, 0.58, 0.78], cards: [
      ["homemind-perception", "마이크 캡처 · 발화 구간(VAD) · 소리 상황(YAMNet ONNX) · 상황 융합 · 음성 규칙 파서 · 부분 의도 힌트"],
      ["homemind-whisper", "Whisper small(LoRA) · whisper.cpp"],
      ["homemind-vision", "Camera1 D-Bus 두 카메라 · 위치 구역 판정 · 집 안 미리보기"],
    ] },
    { x: 3.6, head: "Pi5-01 · UI·LLM", heights: [1.0, 1.46], cards: [
      ["homemind-ui (Flutter)", "7인치 화면 · 승인 · 사생활 설정 · 카메라 영상 · 휴대폰 연결"],
      ["homemind-llm", "안전 가드 + EXAONE 4.0 1.2B LoRA(llama.cpp, Q4). 제안만 하고 실행은 core가 결정"],
    ] },
    { x: 6.7, head: "Pi4 · 백엔드", heights: [0.58, 1.0, 0.78], cards: [
      ["Mosquitto", "인증 포트 1884 · 계정별 ACL"],
      ["homemind-core", "스키마·공식 ID 검증 · 작업 상태 · 승인 정책 · 시나리오 · 상황 규칙 · SQLite"],
      ["homemind-mobile-api", "HTTPS · QR 토큰 · 실시간 알림(SSE)"],
    ] },
  ];
  for (const col of cols) {
    chip(s, pres, col.x, 1.28, 2.8, 0.34, col.head, HEX.dk2, HEX.lt1, 11);
    let y = 1.72;
    col.cards.forEach((card, i) => {
      box(s, pres, { x: col.x, y, w: 2.8, h: col.heights[i], title: card[0], titleSize: 10.5, titleH: 0.24, size: 9, lines: [card[1]], name: card[0] });
      y += col.heights[i] + 0.1;
    });
  }
  box(s, pres, { x: 0.5, y: 4.4, w: 3.2, h: 0.78, fill: "FCEEE6", title: "ESP32 · ESP8266 펌웨어 (C++)", titleSize: 10.5, titleH: 0.24, size: 9, lines: ["중복 명령 차단 · 만료 거절 · ACK · 5초 상태 신호"] });
  box(s, pres, {
    x: 3.85, y: 4.4, w: 5.65, h: 0.78, fill: "F3F5F6", title: "개발 환경", titleSize: 10.5, titleH: 0.24, size: 9,
    lines: ["Windows PC(RTX 3090) 학습·빌드 · Docker buildx arm64 → IPK(서명) · PlatformIO · Flutter 3.29.3 · 메시지 계약 pfn.v1(JSON Schema)"],
  });
  s.addNotes("기기 위 서비스는 모두 C++ 네이티브(IPK, systemd). Python은 PC 기준 구현·학습에만 쓴다.");
}

async function functionDesign(pres) {
  const s = content(pres, S2, "기능 설계도: 입력에서 실행까지");
  text(s, "명령 경로", { x: 0.5, y: 1.22, w: 2, h: 0.22, fontSize: 10, bold: true, color: HEX.accent2 });
  const stages = [
    ["입력", "마이크 · 카메라 · 7인치 화면 · 휴대폰"],
    ["인식 (Pi5-02)", "음성 인식 · 소리 상황 · 영상 위치"],
    ["판단", "규칙 파서 → 안 되면 EXAONE(Pi5-01)"],
    ["검증·승인 (Pi4)", "기기·정책 확인 → 필요하면 7인치·휴대폰 승인"],
    ["실행·확인", "ESP32·ESP8266 실행 → ACK → 화면 갱신"],
  ];
  const w = 1.62, gap = 0.225;
  stages.forEach((st, i) => {
    const x = 0.5 + i * (w + gap);
    box(s, pres, { x, y: 1.48, w, h: 1.12, fill: i === 3 ? "DCEEF1" : HEX.lt2, title: st[0], titleSize: 11, size: 9.5, lines: [st[1]], align: "center" });
    if (i < stages.length - 1) arrow(s, pres, x + w + 0.02, 2.04, x + w + gap - 0.02, 2.04, { color: HEX.accent2, width: 1.75 });
  });
  text(s, "상황 경로", { x: 0.5, y: 2.9, w: 2, h: 0.22, fontSize: 10, bold: true, color: HEX.accent2 });
  const ctx = [
    ["소리 상황 + 카메라 위치", 1.9],
    ["상황 융합 (Pi5-02)", 1.8],
    ["상황 규칙 (Pi4 core)", 1.8],
    ["조용 모드 자동 · “상황이 달라요”로 해제", 2.9],
  ];
  let x = 0.5;
  ctx.forEach(([label, bw], i) => {
    box(s, pres, { x, y: 3.16, w: bw, h: 0.56, fill: "F3F5F6", title: label, titleSize: 10, titleH: 0.32, align: "center" });
    if (i < ctx.length - 1) arrow(s, pres, x + bw + 0.02, 3.44, x + bw + 0.18, 3.44, { color: HEX.accent3, width: 1.5 });
    x += bw + 0.2;
  });
  text(s, "안전 원칙", { x: 0.5, y: 4.0, w: 2, h: 0.22, fontSize: 10, bold: true, color: HEX.accent2 });
  await badge(s, pres, "MdBlock", 0.5, 4.32, 0.5, HEX.accent2, HEX.lt1);
  text(s, [{ text: "실패 시 닫힘", options: { bold: true, breakLine: true } }, { text: "형식 오류 · 불명확한 대상 · 부정 명령 · 미지원 기기 · 주입 문장은 실행하지 않음" }], { x: 1.1, y: 4.28, w: 3.6, h: 0.9, fontSize: 9.5, color: MUTED });
  await badge(s, pres, "MdHomeWork", 5.0, 4.32, 0.5, HEX.accent3, HEX.lt1);
  text(s, [{ text: "Edge First", options: { bold: true, breakLine: true } }, { text: "원본 음성·영상은 집 밖으로 보내지 않고 저장하지 않음. 카메라 미리보기는 집 안 Pi5-01에만" }], { x: 5.6, y: 4.28, w: 3.9, h: 0.9, fontSize: 9.5, color: MUTED });
  s.addNotes("LLM 출력은 장치로 바로 가지 않는다. 어떤 경로든 최종 승인은 Pi4 core.");
}

// ------------------------------------------------------------------ 3. 개발 프로그램
async function fileLayout(pres) {
  pres.addSection({ title: S3 });
  const s = content(pres, S3, "파일 구성과 주요 함수");
  const tree = [
    ["contracts/pfn.v1", "MQTT 메시지 계약(JSON Schema)"],
    ["native/libs", "hm_common · hm_audio · hm_stt · hm_context · hm_parser · hm_guard · hm_partial · hm_vision"],
    ["native/services", "perception · vision · llm · mobile-api · clock-sync"],
    ["team/…/src", "homemind-core (Pi4)"],
    ["team/…/homemind_flutter_test", "7인치 UI (Flutter)"],
    ["team/homemind-application", "휴대폰 앱 (Flutter)"],
    ["firmware", "ESP32 · ESP8266 (PlatformIO)"],
    ["deploy", "설치 · 서명 · 배포 스크립트"],
    ["services", "PC 기준 구현(Python) · 모델 학습"],
  ];
  const runs = [];
  tree.forEach(([dir, what], i) => {
    runs.push({ text: dir, options: { bold: true, color: HEX.dk2, breakLine: true } });
    runs.push({ text: what, options: { color: MUTED, fontSize: 8.5, breakLine: i < tree.length - 1 } });
  });
  box(s, pres, { x: 0.5, y: 1.28, w: 3.55, h: 3.3, fill: HEX.lt2, name: "tree" });
  text(s, runs, { x: 0.65, y: 1.36, w: 3.3, h: 3.15, fontSize: 9.5, paraSpaceAfter: 1 });
  const stats = [["C++", "약 3.1만 줄"], ["Dart", "약 1.1만 줄"], ["계약 JSON", "57개"], ["시험 파일", "154개"]];
  stats.forEach(([k, v], i) => {
    const x = 0.5 + (i % 2) * 1.8, y = 4.68 + Math.floor(i / 2) * 0.24;
    text(s, [{ text: `${k} `, options: { bold: true, color: HEX.accent2 } }, { text: v, options: { color: HEX.dk1 } }], { x, y, w: 1.75, h: 0.22, fontSize: 9 });
  });
  const rows = tableRows(["함수 (모듈)", "하는 일"], [
    ["preflight_proposal\n(hm_guard)", "규칙으로 먼저 해석하고 위험·주입 문장을 막음"],
    ["ExaoneClient::payload ·\nvalidate_proposal (hm_guard)", "EXAONE 요청을 만들고, 출력 형식·대상을 검증"],
    ["ActionMap::resolve\n(hm_common)", "AI 동작어 → 기기 명령. 모형에 없는 기기는 거부"],
    ["PerceptionService::process_window\n(perception)", "4초 창마다 소리 상황 판단, 상황 이벤트 발행"],
    ["accept_task_request (core)", "출처·기기·정책 확인 → 승인 대기 또는 바로 실행"],
    ["apply_context_rules (core)", "상황 이벤트 → 조용 모드 등 작업 생성"],
    ["maintain_time_sensitive_state\n(core)", "ACK 재전송(최대 3회) · 오프라인 판정 · 제안 만료"],
    ["Node::handleCommand\n(firmware)", "중복 차단 → 만료 → 검증 → 실행 → ACK"],
  ]);
  s.addTable(rows, {
    x: 4.25, y: 1.28, w: 5.25, colW: [2.35, 2.9], fontSize: 8.5, fontFace: THEME.bodyFontFace,
    rowH: [0.3, ...Array(8).fill(0.44)], margin: [0.02, 0.07, 0.02, 0.07],
    border: { type: "solid", pt: 0.5, color: RULE }, objectName: "function-table",
  });
  s.addNotes("줄 수는 저장소의 직접 작성 소스(서드파티·시험 제외) 2026-10-26 기준.");
}

async function voiceFlow(pres) {
  const s = content(pres, S3, "흐름도 ①  음성 명령에서 실행까지");
  const node = (x, y, w, h, title, sub, fill) => box(s, pres, { x, y, w, h, fill: fill || HEX.lt2, title, titleSize: 10, titleH: 0.24, size: 8.5, lines: Array.isArray(sub) ? sub : sub ? [sub] : [], align: "center", pad: 0.08 });
  const diamond = (x, y, w, h, label) => {
    s.addShape(pres.shapes.FLOWCHART_DECISION, { x, y, w, h, fill: { color: "FCEEE6" }, line: { color: HEX.accent2, width: 1 } });
    text(s, label, { x, y, w, h, fontSize: 9, bold: true, color: HEX.dk1, align: "center", valign: "middle" });
  };
  // four columns; the core box spans rows A-B so both request sources visibly end in the same check
  const X = [0.5, 2.85, 5.2, 7.55], W = 1.95;
  const mid = (i) => X[i] + W / 2;
  const label = (value, x, y, color) => text(s, value, { x, y, w: 0.6, h: 0.2, fontSize: 9, bold: true, color });
  // row A: speech -> sentence -> rule parser -> core
  node(X[0], 1.28, W, 0.68, "마이크 · VAD", "발화 구간 찾기");
  arrow(s, pres, X[0] + W + 0.02, 1.62, X[1] - 0.02, 1.62, { color: HEX.accent2 });
  node(X[1], 1.28, W, 0.68, "Whisper 최종 문장", "Pi5-02 · 집 안에서 인식");
  arrow(s, pres, X[1] + W + 0.02, 1.62, X[2] - 0.02, 1.62, { color: HEX.accent2 });
  diamond(X[2], 1.2, W, 0.84, "규칙으로\n해석됨?");
  arrow(s, pres, X[2] + W + 0.02, 1.62, X[3] - 0.02, 1.62, { color: HEX.accent2 });
  label("예", X[2] + W + 0.1, 1.38, HEX.accent3);
  node(X[3], 1.28, W, 1.7, "Pi4 core 검증", ["작업 요청을 받아", "기기 · 출처 · 정책 확인", "출처가 LLM이면 항상 승인"], "DCEEF1");
  // row B: partial hint (never executes) and the LLM path
  arrow(s, pres, mid(0), 1.98, mid(0), 2.28, { color: HEX.accent5, dash: "dash" });
  node(X[0], 2.3, X[1] + W - X[0], 0.68, "부분 발화 → 준비 힌트", "말 중간에 대상 기기를 7인치에 미리 표시 · 실행은 하지 않음", "F3F5F6");
  arrow(s, pres, mid(2), 2.06, mid(2), 2.28, { color: HEX.accent2 });
  label("아니오", mid(2) + 0.06, 2.07, HEX.accent2);
  node(X[2], 2.3, W, 0.68, "EXAONE 제안", "Pi5-01 · 안전 가드 통과분만");
  arrow(s, pres, X[2] + W + 0.02, 2.64, X[3] - 0.02, 2.64, { color: HEX.accent2 });
  // row C: approval
  arrow(s, pres, mid(3), 3.0, mid(3), 3.22, { color: HEX.accent2 });
  diamond(X[3], 3.24, W, 0.86, "승인\n필요?");
  arrow(s, pres, X[3] - 0.02, 3.67, X[2] + W + 0.02, 3.67, { color: HEX.accent2 });
  label("예", X[2] + W + 0.1, 3.43, HEX.accent3);
  node(X[2], 3.33, W, 0.68, "7인치 · 휴대폰 승인", "거절 시 취소 · 기한 지나면 만료");
  box(s, pres, {
    x: X[0], y: 3.24, w: X[1] + W - X[0], h: 0.86, fill: HEX.lt1, line: RULE, dash: "dash",
    title: "승인이 필요한 것", titleSize: 10, titleH: 0.24, size: 9,
    lines: ["LLM 제안 전부 · 현관문 열기 · 집에 오기·잠들기 등 시나리오", "현관문 열기는 집 안 7인치에서만 승인"],
  });
  // row D: execute
  arrow(s, pres, mid(3), 4.12, mid(3), 4.4, { color: HEX.accent2 });
  label("아니오", mid(3) + 0.06, 4.15, HEX.accent2);
  arrow(s, pres, mid(2), 4.03, mid(2), 4.4, { color: HEX.accent2 });
  node(X[0], 4.42, 9.0, 0.66, "ESP 명령 → 실행 → ACK → 완료 표시", "ACK가 없으면 3초마다 최대 3회 재전송 · 만료된 명령은 거절 · 같은 명령은 한 번만 실행", "FCEEE6");
  s.addNotes("부분 발화 힌트는 실행 권한이 없다(execution_allowed=false). 실기에서는 아직 기본 꺼짐.");
}

async function contextFlow(pres) {
  const s = content(pres, S3, "흐름도 ②  상황 인지와 기술적 차별성");
  const steps = [
    ["마이크 4초 창 · 1초 간격  +  카메라 2대 위치(구역)", HEX.lt2],
    ["YAMNet 특징 + 활동 분류(두 분류기 앙상블)", HEX.lt2],
    ["상황 융합: 3번 연속이면 켜고, 15번 연속이면 해제", "DCEEF1"],
    ["Pi4 core 상황 규칙 → 조용 모드 자동 실행", HEX.lt2],
    ["“상황이 달라요” → 즉시 해제, 그 상황 동안 다시 안 켬", "FCEEE6"],
  ];
  steps.forEach(([label, fill], i) => {
    const y = 1.3 + i * 0.76;
    box(s, pres, { x: 0.5, y, w: 4.3, h: 0.56, fill, title: label, titleSize: 10, titleH: 0.32, name: `ctx-${i}` });
    if (i < steps.length - 1) arrow(s, pres, 2.65, y + 0.57, 2.65, y + 0.75, { color: HEX.accent3, width: 1.5 });
  });
  const cards = [
    ["MdRule", "규칙 먼저, LLM은 보조", "명확한 명령은 규칙으로 바로, 애매하거나 숨은 의도만 EXAONE"],
    ["MdVerifiedUser", "LLM은 제안만", "Pi4 core의 결정론적 정책이 최종 승인. 실패하면 닫힘"],
    ["MdHomeWork", "온디바이스 · Edge First", "STT·LLM·비전 모두 집 안. 원본 음성·영상은 밖으로 안 나감"],
    ["MdDataObject", "계약 기반 C++ 네이티브", "JSON Schema 계약 · Python 기준 구현과 출력 동일성 대조"],
  ];
  text(s, "기술적 차별성", { x: 5.2, y: 1.26, w: 3, h: 0.26, fontSize: 12, bold: true, color: HEX.accent2 });
  for (let i = 0; i < cards.length; i++) {
    const [ic, head, body] = cards[i];
    const y = 1.6 + i * 0.88;
    box(s, pres, { x: 5.2, y, w: 4.3, h: 0.78, fill: "F7FAFA", line: "DCE4E7", name: head });
    await badge(s, pres, ic, 5.32, y + 0.14, 0.5, HEX.accent3, HEX.lt1);
    text(s, head, { x: 5.95, y: y + 0.08, w: 3.45, h: 0.26, fontSize: 11, bold: true, color: HEX.dk2 });
    text(s, body, { x: 5.95, y: y + 0.34, w: 3.45, h: 0.42, fontSize: 9, color: MUTED });
  }
  s.addNotes("켜기 3번·해제 15번 = 정책 v6.3(2026-10-22). 카메라 위치 입력은 비전 빈 장면 촬영 뒤 실기에서 켜짐.");
}

// ------------------------------------------------------------------ 3-1. 가산점
async function bonus(pres) {
  pres.addSection({ title: S31 });
  const s = content(pres, S31, "가산점 (1) 3종 하드웨어 연동  ·  (2) 멀티모달 상황 인지");
  // (1)
  box(s, pres, { x: 0.5, y: 1.28, w: 4.4, h: 3.62, fill: HEX.lt2, name: "bonus-1" });
  chip(s, pres, 0.65, 1.4, 1.0, 0.28, "가산점 (1)", HEX.accent2, HEX.lt1, 9.5);
  text(s, "LG 플랫폼 기반 3종 하드웨어 연동", { x: 1.75, y: 1.4, w: 3.1, h: 0.28, fontSize: 11.5, bold: true, color: HEX.dk2, valign: "middle" });
  chip(s, pres, 0.9, 1.9, 3.6, 0.32, "LG AI Native OS  +  MQTT pfn.v1", HEX.dk2, HEX.lt1, 10);
  const hw = [["Raspberry Pi 4", "백엔드"], ["Raspberry Pi 5 ×2", "인식 · UI·LLM"], ["ESP32 (+ESP8266)", "문·창문 · 조명·팬"]];
  hw.forEach(([name, role], i) => {
    const x = 0.65 + i * 1.42;
    arrow(s, pres, x + 0.62, 2.24, x + 0.62, 2.42, { noHead: true, color: HEX.accent3, width: 1.5 });
    box(s, pres, { x, y: 2.44, w: 1.3, h: 0.72, fill: HEX.lt1, line: HEX.accent3, title: name, titleSize: 9, titleH: 0.24, size: 8.5, lines: [role], align: "center", pad: 0.06 });
  });
  text(s, [
    { text: "Pi4·Pi5 모두 LG AI Native OS 위에서 C++ 네이티브 서비스(IPK·systemd)로 실행", options: { bullet: true, breakLine: true } },
    { text: "카메라는 플랫폼 Camera1 D-Bus API로 사용", options: { bullet: true, breakLine: true } },
    { text: "세 종류 기기가 같은 MQTT 계약과 계정별 ACL로 연동", options: { bullet: true, breakLine: true } },
    { text: "ESP는 core 명령 → 실행 → ACK, 5초마다 상태 신호", options: { bullet: true } },
  ], { x: 0.65, y: 3.36, w: 4.15, h: 1.5, fontSize: 10, color: MUTED, paraSpaceAfter: 4 });
  // (2)
  box(s, pres, { x: 5.1, y: 1.28, w: 4.4, h: 3.62, fill: HEX.lt2, name: "bonus-2" });
  chip(s, pres, 5.25, 1.4, 1.0, 0.28, "가산점 (2)", HEX.accent2, HEX.lt1, 9.5);
  text(s, "멀티모달 상황 인지", { x: 6.35, y: 1.4, w: 3.1, h: 0.28, fontSize: 11.5, bold: true, color: HEX.dk2, valign: "middle" });
  const inputs = [["MdRecordVoiceOver", "음성"], ["MdGraphicEq", "소리"], ["MdVideocam", "영상"], ["MdDevices", "기기 상태"]];
  for (let i = 0; i < inputs.length; i++) {
    const x = 5.32 + i * 1.02;
    await badge(s, pres, inputs[i][0], x + 0.21, 1.86, 0.48, HEX.accent3, HEX.lt1);
    text(s, inputs[i][1], { x, y: 2.37, w: 0.9, h: 0.22, fontSize: 9, bold: true, color: HEX.dk1, align: "center" });
  }
  chip(s, pres, 5.5, 2.66, 3.6, 0.3, "상황 융합 → 회의·취침 판단 → 조용 모드", HEX.dk2, HEX.lt1, 9.5);
  const stat = (x, value, label) => {
    text(s, value, { x, y: 3.1, w: 1.35, h: 0.5, fontSize: 22, bold: true, color: HEX.accent2, align: "center", valign: "middle" });
    text(s, label, { x, y: 3.6, w: 1.35, h: 0.4, fontSize: 8.5, color: MUTED, align: "center" });
  };
  stat(5.2, "91.3%", "조용 모드 재현율");
  stat(6.6, "4.9%", "오탐률");
  stat(8.0, "40→4", "30분 회의 중 켜짐/꺼짐 반복");
  text(s, "PC 시험: AI Hub 그룹 분리 test 375개(재현율·오탐률), 실제 회의 녹음 30분(반복 횟수, 정책 v6.3)", { x: 5.25, y: 4.25, w: 4.1, h: 0.55, fontSize: 8.5, color: HEX.accent5 });
  s.addNotes("수치는 PC 시험(context-fusion README). 실기 마이크 현장 시험은 별도로 필요. 대회 예시는 RPi Zero였으나 우리 구성은 Pi4·Pi5·ESP32(팀 확인 사항).");
}

// ------------------------------------------------------------------ 4. 장애요인
function obstacleTable(s, rows) {
  const body = rows.map((r) => [r[0], r[1], r[2], { text: r[3], options: { color: r[3].startsWith("실기") ? HEX.accent6 : HEX.dk1, bold: r[3].startsWith("실기") } }]);
  s.addTable(tableRows(["문제", "원인", "해결", "결과"], body), {
    x: 0.5, y: 1.28, w: 9.0, colW: [1.75, 2.2, 3.15, 1.9], fontSize: 9, fontFace: THEME.bodyFontFace,
    rowH: [0.3, ...rows.map(() => (rows.length > 5 ? 0.5 : 0.72))], margin: [0.03, 0.08, 0.03, 0.08],
    border: { type: "solid", pt: 0.5, color: RULE }, objectName: "obstacle-table",
  });
}

async function obstaclesHw(pres) {
  pres.addSection({ title: S4 });
  const s = content(pres, S4, "장애요인과 해결 ①  하드웨어·플랫폼");
  obstacleTable(s, [
    ["카메라 모듈이 인식되지 않음", "Atlas 커널에 CSI 카메라 드라이버가 없음(세 기기 모두)", "현관 카메라를 USB 웹캠으로 바꾸고 두 카메라 모두 Camera1 D-Bus로 통일", "실기: 두 카메라 촬영, 7인치에 실시간 영상"],
    ["Pi5 두 대가 서로 통신하지 못함", "모든 Pi5가 같은 장치 트리로 부팅돼 eth0 MAC이 같음", "펌웨어가 넘기는 보드 고유 MAC(macaddr)을 부팅 때 적용하는 서비스", "실기: ping 손실 0%, 재부팅 뒤에도 유지"],
    ["LLM 실행 중 Pi5가 꺼짐", "2.4 GHz 네 코어 추론에서 5V 3A 전원 전압 부족", "추론 노드를 1.8 GHz로 제한, 전용 전원. 전압 측정 도구 직접 작성(메일박스)", "실기: 실행 중 최저 4.887 V, 꺼짐 없음"],
    ["재부팅 뒤 카메라 목록이 빔", "카메라 서비스가 부팅 초반에 함께 켜져 플러그인 목록 생성 실패", "필요할 때 D-Bus로 깨우도록 시작 순서를 바꿈", "실기: 재부팅 뒤 카메라 자동 복구"],
    ["서보 모터 고장", "MG90S 1개 고장(문 3개 → 2개)", "화장실 문 철거, 현관문·창문은 열기/닫기 모터. Pi는 열림/닫힘만 보내고 ESP가 모터 값으로 변환(계약 v4)", "계약·펌웨어 시험 통과(PC)"],
  ]);
  s.addNotes("모두 2026-10-23~26 PROGRESS 기록. ESP 펌웨어는 보드 업로드 후 실기 결과로 바꿀 것.");
}

async function obstaclesSw(pres) {
  const s = content(pres, S4, "장애요인과 해결 ②  소프트웨어·AI");
  obstacleTable(s, [
    ["기기에서 Python을 쓸 수 없음", "Atlas 앱 환경은 C++ 네이티브 서비스 중심", "모든 기기 서비스를 C++로 옮기고 Python 기준 구현과 같은 출력인지 대조", "PC: 가드 149,339/149,339 등 전 항목 일치"],
    ["LLM 첫 질문이 시간 초과", "캐시가 없는 첫 요청이 입력 전체를 읽음(30초 이상)", "부팅할 때 예열해 공통 앞부분을 캐시에 올림", "실기: 첫 문장부터 성공, 한 문장 약 15~20초"],
    ["프롬프트 순서를 바꾸자 답이 바뀜", "학습 때와 입력 순서가 달라짐(“거실 좀 어둡다” → 끄기)", "학습 순서 유지. 바꿀 때는 100문장 재평가를 규칙으로", "실기: 다시 “켜기”로 정상"],
    ["조용 모드가 말 사이마다 깜빡임", "말 사이 침묵 한 번에 해제됨", "해제 조건을 15번 연속으로 늘림(정책 v6.3)", "PC: 30분 회의 녹음 40번 → 4번"],
    ["음성 인식이 13.8초로 느림", "Pi5 CPU에서 기본 설정의 긴 입력 창", "Whisper 입력 창(audio_ctx)을 512로 조정", "실기: 6.1초, 문자 오류율 12%"],
    ["설치 중 DB 백업이 끝나지 않음", "245 MB 운영 DB에 core가 계속 써서 온라인 백업이 처음부터 반복(실기)", "core를 잠깐 멈추고 DB 파일 복사 → 다시 시작", "PC: 설치 리허설 통과"],
    ["연결된 마이크·카메라가 오프라인 표시", "Pi5 서비스가 상태 신호를 보내지 않아 15초 뒤 오프라인 판정", "인식·LLM 서비스가 5초마다 상태 신호 전송", "PC: 시험 통과"],
  ]);
  s.addNotes("LLM 한 문장 약 15~20초는 1.8 GHz 실측(출력이 메모리 대역폭 한계). STT 6.1초는 합성 음성 5문장 실측(2026-10-23). 마지막 두 줄은 수정본 실기 설치 뒤 결과를 실기로 바꿀 것.");
}

module.exports = async function sections2to4(pres) {
  await hardwareMap(pres);
  await hardwareTable(pres);
  await softwareStack(pres);
  await functionDesign(pres);
  await fileLayout(pres);
  await voiceFlow(pres);
  await contextFlow(pres);
  await bonus(pres);
  await obstaclesHw(pres);
  await obstaclesSw(pres);
};
