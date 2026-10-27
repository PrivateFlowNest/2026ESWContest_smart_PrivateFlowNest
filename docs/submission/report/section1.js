// 표지 + 1. 개발 개요 (3쪽)
const { TEAM, WORK, TAGLINE, GITHUB, YOUTUBE, HEX, GRAY_CARD, RULE, MUTED, THEME, icon, content, text } = require("./lib");

async function cover(pres) {
  pres.addSection({ title: "표지" });
  const s = pres.addSlide({ masterName: "COVER", sectionTitle: "표지" });
  s.addText("제24회 임베디드SW경진대회 개발완료보고서", { placeholder: "kicker" });
  s.addText(WORK, { placeholder: "title" });
  s.addText([
    { text: "말이 끝나기 전에 준비하고,", options: { breakLine: true } },
    { text: "상황을 보고 먼저 제안하는 온디바이스 AI 스마트홈" },
  ], { placeholder: "subtitle" });
  s.addText(`스마트 가전 부문   |   ${TEAM}`, { placeholder: "meta" });
  s.addShape(pres.shapes.OVAL, { x: 7.25, y: 1.35, w: 2.3, h: 2.3, fill: { color: HEX.accent3, transparency: 35 }, line: { type: "none" }, objectName: "cover-circle" });
  s.addImage({ data: await icon("MdHome", HEX.lt1), x: 7.75, y: 1.85, w: 1.3, h: 1.3, objectName: "cover-home", altText: "집 아이콘" });
  s.addNotes("표지. 팀번호는 제출 전에 팀이 채운다.");
}

async function overview(pres) {
  pres.addSection({ title: "1. 개발 개요" });
  const s = content(pres, "1. 개발 개요", "개발 개요");
  const label = (t, color) => ({ text: t, options: { bold: true, color: color || HEX.dk2, fill: { color: HEX.lt2 }, align: "center", valign: "middle" } });
  const body = (t, extra) => ({ text: t, options: Object.assign({ color: HEX.dk1, valign: "middle" }, extra || {}) });
  const rows = [
    [label("작품명"), body([{ text: WORK, options: { bold: true, color: HEX.dk2 } }, { text: ` — ${TAGLINE}` }])],
    [label("개발 배경"), body("지금의 스마트홈은 정해진 명령을 끝까지 말하거나 앱을 눌러야 움직이고, 더 똑똑해지려면 집 안의 음성·영상을 클라우드로 보내야 합니다.")],
    [label("동기"), body("‘아 좀 덥네’ 같은 혼잣말이나 회의 중이라는 상황에 사용자의 의도가 담겨 있습니다. 이것을 가전이 먼저 알아채되, 사생활은 집 밖으로 나가지 않게 하고 싶었습니다.")],
    [label("목표"), body("① 부분 발화·숨은 의도 이해(온디바이스 STT·EXAONE)  ② 소리·영상으로 상황을 인지해 먼저 제안  ③ 원본 음성·영상은 집 밖으로 보내지 않음  ④ 모든 실행은 정책과 사람 승인을 거쳐 안전하게")],
    [label("필요성"), body("1인 가구·고령자·재택근무자처럼 손이 바쁘거나 조작이 어려운 사용자, 커지는 개인정보 우려, 인터넷이 끊겨도 동작해야 하는 가전 — 세 가지를 함께 풀어야 합니다.")],
    [label("소스코드 링크", HEX.accent3), body(GITHUB, { color: HEX.accent2, bold: true })],
    [label("시연 동영상 링크", HEX.accent3), body([
      { text: YOUTUBE, options: { color: HEX.accent2, bold: true } },
      { text: `   제목: 2026ESWContest_스마트가전_${TEAM}_시연동영상`, options: { color: HEX.accent5 } },
    ])],
  ];
  s.addTable(rows, {
    x: 0.5, y: 1.28, w: 9.0, colW: [1.65, 7.35], fontSize: 11.5, fontFace: THEME.bodyFontFace,
    rowH: [0.46, 0.56, 0.56, 0.56, 0.56, 0.4, 0.4], margin: [0.04, 0.12, 0.04, 0.12],
    border: { type: "solid", pt: 0.75, color: RULE }, objectName: "overview-table",
  });
  s.addNotes("링크 두 줄은 GitHub 사용자 이름과 유튜브 주소가 정해지면 채운다.");
}

async function problem(pres) {
  const s = content(pres, "1. 개발 개요", "명령을 기다리는 집에서, 먼저 알아채는 집으로");
  const colY = 1.32;
  const head = (x, value, fill, color) => {
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y: colY, w: 4.0, h: 0.42, rectRadius: 0.08, fill: { color: fill }, line: { type: "none" }, objectName: `head-${value}` });
    text(s, value, { x, y: colY, w: 4.0, h: 0.42, fontSize: 14, bold: true, color, align: "center", valign: "middle" });
  };
  head(0.5, "지금의 스마트홈", "DDE3E6", HEX.dk1);
  head(5.5, WORK, HEX.dk2, HEX.lt1);
  const left = [
    ["MdKeyboardVoice", "명령을 끝까지, 정확하게", "말이 끝나야 처리를 시작하고, 정해진 문장이 아니면 알아듣지 못합니다."],
    ["MdHelpOutline", "상황을 모른다", "회의 중인지, 잠들려는지 몰라 사람이 매번 직접 바꿔야 합니다."],
    ["MdCloudUpload", "음성·영상이 클라우드로", "똑똑해지려면 집 안의 소리와 영상을 외부 서버로 보내야 합니다."],
  ];
  const right = [
    ["MdAutoAwesome", "말 중간에 먼저 준비", "부분 발화로 대상 기기를 미리 파악하고, ‘좀 덥네’ 같은 숨은 의도까지 온디바이스 EXAONE이 이해합니다."],
    ["MdSensors", "상황을 보고 먼저 제안", "소리 상황과 카메라 위치를 합쳐 회의·취침을 알아채고 조용 모드 등을 먼저 실행·제안합니다."],
    ["MdShield", "집 안에서 처리, 안전하게 실행", "STT·LLM·비전 모두 집 안 Pi에서. 실행은 정책과 사람 승인을 거칩니다."],
  ];
  const rowY = [1.9, 3.0, 4.1];
  const card = async (x, item, y, fill, iconColor, circle) => {
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w: 4.0, h: 0.98, rectRadius: 0.08, fill: { color: fill }, line: { type: "none" }, objectName: `card-${item[1]}` });
    s.addShape(pres.shapes.OVAL, { x: x + 0.18, y: y + 0.21, w: 0.56, h: 0.56, fill: { color: circle }, line: { type: "none" } });
    s.addImage({ data: await icon(item[0], iconColor), x: x + 0.29, y: y + 0.32, w: 0.34, h: 0.34, altText: item[1] });
    text(s, item[1], { x: x + 0.9, y: y + 0.1, w: 2.95, h: 0.3, fontSize: 13, bold: true });
    text(s, item[2], { x: x + 0.9, y: y + 0.4, w: 2.95, h: 0.52, fontSize: 10.5, color: MUTED });
  };
  for (let i = 0; i < 3; i++) {
    await card(0.5, left[i], rowY[i], GRAY_CARD, "6B7780", "E3E8EA");
    await card(5.5, right[i], rowY[i], "E2F0F2", HEX.lt1, HEX.accent3);
  }
  s.addShape(pres.shapes.RIGHT_ARROW, { x: 4.62, y: 2.95, w: 0.76, h: 0.62, fill: { color: HEX.accent2 }, line: { type: "none" }, objectName: "arrow" });
  s.addNotes("부분 발화 선제 준비: 구현·PC 시험 완료, 실기 설정은 아직 꺼짐(PROGRESS). 제출 전에 실기에서 켜고 확인할 것.");
}

async function scenarios(pres) {
  const s = content(pres, "1. 개발 개요", "세 가지 장면으로 보는 HomeMind");
  const cards = [
    { icon: "MdThermostat", tag: "숨은 의도 이해", quote: "“아 좀 덥네…”", steps: [
      "Pi5-02 마이크 → 집 안에서 음성 인식(Whisper)",
      "규칙에 없는 말 → Pi5-01 EXAONE이 ‘에어컨 켜기’ 제안",
      "7인치 화면·휴대폰에 “에어컨을 켤까요?”",
      "승인 → Pi4 core 검증 → ESP8266",
    ], result: "에어컨 표시등 켜짐" },
    { icon: "MdGroups", tag: "상황 인지", quote: "회의가 시작되면", steps: [
      "소리 상황(YAMNet) + 카메라 위치로 ‘회의 중’ 판단",
      "Pi4 core가 조용 모드 자동 실행",
      "7인치 화면에 조용 모드 표시",
      "“상황이 달라요” 한 번이면 즉시 해제",
    ], result: "에어컨 저소음 표시(깜빡임)" },
    { icon: "MdWindow", tag: "바로 실행 + 안전", quote: "“창문 열어줘”", steps: [
      "명확한 명령은 규칙 파서가 바로 해석(LLM 생략)",
      "Pi4 core가 기기·정책 확인",
      "ESP32 창문 모터 열기 → 완료 응답",
      "현관문 열기는 집 안 7인치 승인 필수",
    ], result: "창문 열림" },
  ];
  const w = 2.9, gap = 0.15, y = 1.3, h = 3.78;
  for (let i = 0; i < cards.length; i++) {
    const c = cards[i];
    const x = 0.5 + i * (w + gap);
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w, h, rectRadius: 0.1, fill: { color: HEX.lt2 }, line: { type: "none" }, objectName: `scenario-${i + 1}` });
    s.addShape(pres.shapes.OVAL, { x: x + 0.2, y: y + 0.2, w: 0.55, h: 0.55, fill: { color: HEX.accent3 }, line: { type: "none" } });
    s.addImage({ data: await icon(c.icon, HEX.lt1), x: x + 0.31, y: y + 0.31, w: 0.33, h: 0.33, altText: c.tag });
    text(s, c.tag, { x: x + 0.88, y: y + 0.2, w: w - 1.0, h: 0.26, fontSize: 10.5, bold: true, color: HEX.accent2 });
    text(s, c.quote, { x: x + 0.88, y: y + 0.46, w: w - 1.0, h: 0.32, fontSize: 14, bold: true, color: HEX.dk2 });
    c.steps.forEach((step, k) => {
      const sy = y + 0.98 + k * 0.56;
      s.addShape(pres.shapes.OVAL, { x: x + 0.2, y: sy + 0.04, w: 0.26, h: 0.26, fill: { color: HEX.lt1 }, line: { color: HEX.accent3, width: 1 } });
      text(s, String(k + 1), { x: x + 0.2, y: sy + 0.04, w: 0.26, h: 0.26, fontSize: 9, bold: true, color: HEX.accent3, align: "center", valign: "middle" });
      text(s, step, { x: x + 0.55, y: sy, w: w - 0.7, h: 0.5, fontSize: 10.5 });
    });
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: x + 0.2, y: y + h - 0.55, w: w - 0.4, h: 0.38, rectRadius: 0.08, fill: { color: HEX.accent2 }, line: { type: "none" } });
    text(s, c.result, { x: x + 0.2, y: y + h - 0.55, w: w - 0.4, h: 0.38, fontSize: 11.5, bold: true, color: HEX.lt1, align: "center", valign: "middle" });
  }
  s.addNotes("세 장면 모두 시스템 동작 설명. 실기 시연 여부는 PROGRESS 기준으로 보고서 15쪽에서 따로 표시.");
}

module.exports = async function section1(pres) {
  await cover(pres);
  await overview(pres);
  await problem(pres);
  await scenarios(pres);
};
