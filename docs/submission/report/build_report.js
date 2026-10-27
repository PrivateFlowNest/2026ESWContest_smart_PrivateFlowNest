// 개발완료보고서 (제24회 임베디드SW경진대회, 스마트 가전) — pptxgenjs generator.
//   bash build.sh  ->  out/2026ESWContest_스마트가전_PrivateFlowNest_개발완료보고서.pptx + .pdf + page images
// Layout: 16:9 (10 x 5.625 in). Theme fonts: Noto Sans KR (OFL, copyright-free as the guide asks).
// Content rules: real-hardware results are stated as such only when PROGRESS.md records them; PC-only results say
// "PC". Team number and members stay blank (user 2026-10-26).
const path = require("path");
const fs = require("fs");
const { TEAM, THEME, makeDeck, koreanWordWrap } = require("./lib");
const section1 = require("./section1");
const sections2to4 = require("./section2_4");
const sections5to7 = require("./section5_7");

// The theme colors are written by the pptx skill's apply_theme.js (PPTX_SKILL_DIR). Without it the scheme colors of
// the placeholders fall back to Office's stock palette; everything else is fixed hex.
const applyTheme = process.env.PPTX_SKILL_DIR
  ? require(path.join(process.env.PPTX_SKILL_DIR, "scripts/apply_theme.js")).applyTheme
  : async () => console.warn("PPTX_SKILL_DIR not set: theme colors not applied");

async function main() {
  const { pres } = makeDeck();
  await section1(pres);
  await sections2to4(pres);
  await sections5to7(pres);
  const outDir = path.join(__dirname, "out");
  fs.mkdirSync(outDir, { recursive: true });
  const file = path.join(outDir, `2026ESWContest_스마트가전_${TEAM}_개발완료보고서.pptx`);
  await pres.writeFile({ fileName: file });
  await applyTheme(file, THEME);
  await koreanWordWrap(file);
  console.log(file);
}

main().catch((error) => { console.error(error); process.exit(1); });
