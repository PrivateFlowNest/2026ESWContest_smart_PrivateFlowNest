// Shared pieces of the report deck: theme, layouts, icons and small drawing helpers.
const React = require("react");
const ReactDOMServer = require("react-dom/server");
const sharp = require("sharp");
const md = require("react-icons/md");
const fs = require("fs");
const pptxgen = require("pptxgenjs");

const TEAM = "PrivateFlowNest";
const WORK = "HomeMind TaskFlow";
const TAGLINE = "말이 끝나기 전에 준비하고, 상황을 보고 먼저 제안하는 온디바이스 AI 스마트홈";
const GITHUB = `github.com/〈사용자이름〉/2026ESWContest_smart_${TEAM}`;
const YOUTUBE = "〈유튜브 링크: 업로드 후 입력〉";

const THEME = {
  name: "HomeMind TaskFlow",
  headFontFace: "Noto Sans KR",
  bodyFontFace: "Noto Sans KR",
  colors: {
    dk1: "1F2933", lt1: "FFFFFF", dk2: "0E4A55", lt2: "EEF4F5",
    accent1: "0E4A55", accent2: "E8743B", accent3: "2A8C9A", accent4: "F2B33D",
    accent5: "8A98A3", accent6: "3B8B4F", hlink: "2A8C9A", folHlink: "6B7C87",
  },
};
const HEX = THEME.colors;
const GRAY_CARD = "F3F5F6";
const RULE = "C9D3D8";
const MUTED = "4A5560";

async function icon(name, color, size = 256) {
  const svg = ReactDOMServer.renderToStaticMarkup(React.createElement(md[name], { color: `#${color}`, size }));
  const png = await sharp(Buffer.from(svg)).png().toBuffer();
  return "image/png;base64," + png.toString("base64");
}

function makeDeck() {
  const pres = new pptxgen();
  pres.layout = "LAYOUT_16x9";
  pres.author = TEAM;
  pres.company = TEAM;
  pres.title = `2026ESWContest_스마트가전_${TEAM}_개발완료보고서`;
  pres.theme = { headFontFace: THEME.headFontFace, bodyFontFace: THEME.bodyFontFace };
  const C = pres.SchemeColor;

  pres.defineSlideMaster({
    title: "COVER",
    background: { color: HEX.dk2 },
    objects: [
      { placeholder: { options: { name: "kicker", type: "body", x: 0.6, y: 1.05, w: 6.4, h: 0.4, fontSize: 14, color: C.background2, margin: 0 }, text: "" } },
      { placeholder: { options: { name: "title", type: "title", x: 0.6, y: 1.5, w: 6.6, h: 0.95, fontSize: 40, bold: true, color: C.background1, margin: 0, valign: "middle", align: "left" }, text: "" } },
      { placeholder: { options: { name: "subtitle", type: "body", x: 0.6, y: 2.5, w: 6.4, h: 0.9, fontSize: 17, color: HEX.accent4, margin: 0, valign: "top" }, text: "" } },
      { placeholder: { options: { name: "meta", type: "body", x: 0.6, y: 4.35, w: 6.4, h: 0.4, fontSize: 13, color: C.background1, margin: 0 }, text: "" } },
    ],
  });
  pres.defineSlideMaster({
    title: "CONTENT",
    background: { color: HEX.lt1 },
    objects: [
      { placeholder: { options: { name: "section", type: "body", x: 0.5, y: 0.26, w: 7.5, h: 0.28, fontSize: 11, bold: true, color: C.accent2, margin: 0 }, text: "" } },
      { placeholder: { options: { name: "title", type: "title", x: 0.5, y: 0.52, w: 9.0, h: 0.56, fontSize: 22, bold: true, color: C.text2, margin: 0, valign: "middle", align: "left" }, text: "" } },
      { text: { text: `${WORK} · ${TEAM}`, options: { x: 0.5, y: 5.28, w: 5, h: 0.22, fontSize: 8, color: HEX.accent5, margin: 0 } } },
    ],
    slideNumber: { x: 9.05, y: 5.26, w: 0.45, h: 0.24, fontSize: 9, color: HEX.accent5, align: "right" },
  });
  pres.defineSlideMaster({
    title: "END",
    background: { color: HEX.dk2 },
    objects: [
      { placeholder: { options: { name: "title", type: "title", x: 0.6, y: 2.1, w: 8.8, h: 0.9, fontSize: 36, bold: true, color: C.background1, align: "center", margin: 0 }, text: "" } },
      { placeholder: { options: { name: "body", type: "body", x: 0.6, y: 3.0, w: 8.8, h: 0.5, fontSize: 14, color: C.background2, align: "center", margin: 0 }, text: "" } },
    ],
  });
  return { pres, C };
}

function content(pres, section, title) {
  const slide = pres.addSlide({ masterName: "CONTENT", sectionTitle: section });
  slide.addText(section, { placeholder: "section" });
  slide.addText(title, { placeholder: "title" });
  return slide;
}

// A plain text box (margin 0) — every text that is not a placeholder goes through here.
function text(s, value, o) {
  s.addText(value, Object.assign({ margin: 0, isTextBox: true, color: HEX.dk1, fontSize: 11, valign: "top" }, o));
}

// Rounded box with a bold heading and optional body lines underneath.
function box(s, pres, o) {
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: o.x, y: o.y, w: o.w, h: o.h, rectRadius: o.radius ?? 0.08,
    fill: { color: o.fill || HEX.lt2 }, line: o.line ? { color: o.line, width: o.lineWidth || 1, dashType: o.dash || "solid" } : { type: "none" },
    objectName: o.name,
  });
  const pad = o.pad ?? 0.12;
  let y = o.y + pad;
  if (o.title) {
    text(s, o.title, { x: o.x + pad, y, w: o.w - 2 * pad, h: o.titleH || 0.28, fontSize: o.titleSize || 12, bold: true, color: o.titleColor || HEX.dk2, align: o.align || "left", valign: "middle" });
    y += (o.titleH || 0.28) + 0.04;
  }
  if (o.lines) {
    const runs = o.lines.map((line, i) => (typeof line === "string"
      ? { text: line, options: { breakLine: i < o.lines.length - 1 } }
      : { text: line.text, options: Object.assign({ breakLine: i < o.lines.length - 1 }, line.options) }));
    text(s, runs, { x: o.x + pad, y, w: o.w - 2 * pad, h: o.y + o.h - y - pad * 0.6, fontSize: o.size || 10, color: o.color || MUTED, align: o.align || "left", paraSpaceAfter: o.gap ?? 2 });
  }
}

// Straight arrow from (x1,y1) to (x2,y2).
function arrow(s, pres, x1, y1, x2, y2, o = {}) {
  s.addShape(pres.shapes.LINE, {
    x: Math.min(x1, x2), y: Math.min(y1, y2), w: Math.abs(x2 - x1), h: Math.abs(y2 - y1),
    flipH: x2 < x1, flipV: y2 < y1,
    line: { color: o.color || HEX.accent5, width: o.width || 1.25, endArrowType: o.noHead ? undefined : "triangle", dashType: o.dash || "solid" },
  });
}

// Small filled pill with centered text.
function chip(s, pres, x, y, w, h, value, fill, color, size = 9.5) {
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w, h, rectRadius: h / 2, fill: { color: fill }, line: { type: "none" } });
  text(s, value, { x, y, w, h, fontSize: size, bold: true, color, align: "center", valign: "middle" });
}

// Icon inside a filled circle.
async function badge(s, pres, name, x, y, d, circle, color) {
  s.addShape(pres.shapes.OVAL, { x, y, w: d, h: d, fill: { color: circle }, line: { type: "none" } });
  const inset = d * 0.2;
  s.addImage({ data: await icon(name, color), x: x + inset, y: y + inset, w: d - 2 * inset, h: d - 2 * inset, altText: name });
}

// Header row + body rows for addTable, in the deck's table style.
function tableRows(header, rows, o = {}) {
  const head = header.map((h) => ({ text: h, options: { bold: true, color: HEX.lt1, fill: { color: HEX.dk2 }, align: "center", valign: "middle" } }));
  const body = rows.map((row, r) => row.map((cell, c) => {
    const base = { color: HEX.dk1, valign: "middle", fill: { color: r % 2 ? "F7FAFA" : HEX.lt1 } };
    if (c === 0 && o.firstBold !== false) Object.assign(base, { bold: true, color: HEX.dk2 });
    return typeof cell === "string" ? { text: cell, options: base } : { text: cell.text, options: Object.assign(base, cell.options) };
  }));
  return [head, ...body];
}

// PowerPoint breaks Korean between any two syllables unless a paragraph says eaLnBrk="0" and the run is tagged Korean:
// set both on every paragraph of the slides and layouts so lines wrap at spaces.
async function koreanWordWrap(file) {
  const JSZip = require("jszip");
  const zip = await JSZip.loadAsync(fs.readFileSync(file));
  const parts = Object.keys(zip.files).filter((name) => /^ppt\/(slides|slideLayouts)\/[^/]+\.xml$/.test(name));
  for (const name of parts) {
    let xml = await zip.file(name).async("string");
    xml = xml.replace(/<a:pPr(?![^>]*eaLnBrk)/g, '<a:pPr eaLnBrk="0"').replace(/<a:p>(?!<a:pPr)/g, '<a:p><a:pPr eaLnBrk="0"/>');
    xml = xml.replace(/ lang="en-US"/g, ' lang="ko-KR" altLang="en-US"');
    zip.file(name, xml);
  }
  fs.writeFileSync(file, await zip.generateAsync({ type: "nodebuffer", compression: "DEFLATE" }));
}

module.exports = {
  TEAM, WORK, TAGLINE, GITHUB, YOUTUBE, THEME, HEX, GRAY_CARD, RULE, MUTED,
  icon, makeDeck, content, text, box, arrow, chip, badge, tableRows, koreanWordWrap,
};
