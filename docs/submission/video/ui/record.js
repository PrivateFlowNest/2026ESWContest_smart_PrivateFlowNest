// Record the real 7-inch UI and phone app screens for the video (frame by frame under the flutter_test clock).
//   node ui/record.js            both apps -> out/ui/<clip>/ (PNG frames + frames.json) and out/ui/<clip>.mp4
//   node ui/record.js panel      only the 7-inch UI (or: phone)
// Each app is copied to .ui-build/ with the recorder added under test/video/, so the apps in team/ stay untouched.
// FLUTTER=<path to flutter(.bat)> overrides the SDK (default: the PC's Flutter 3.47).
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");
const ffmpeg = require("ffmpeg-static");

const VIDEO = path.join(__dirname, "..");
const REPO = path.join(VIDEO, "..", "..", "..");
const OUT = path.join(VIDEO, "out", "ui");
const BUILD = path.join(VIDEO, ".ui-build");
const FLUTTER = process.env.FLUTTER || "C:/Users/alexi/dev/flutter/bin/flutter.bat";
const FLUTTER_ROOT = path.dirname(path.dirname(FLUTTER));
const NOTO = path.join(REPO, "team/homemind-core-ui-2026-10-20/homemind_flutter_test/assets/fonts/NotoSansKR-VariableFont_wght.ttf");
const APPS = {
  panel: { src: "team/homemind-core-ui-2026-10-20/homemind_flutter_test", test: "panel_test.dart" },
  phone: { src: "team/homemind-application-2026-10-21", test: "phone_test.dart" },
};
const KEEP = ["pubspec.yaml", "pubspec.lock", "analysis_options.yaml", "lib", "assets"];
// Test-engine differences, applied to the copy only. The phone header's RichText has no font family: a phone
// draws it in the system font (Roboto), the test engine in its box-shaped test font, so the copy names Roboto.
const PATCHES = {
  phone: [["lib/widgets/common_widgets.dart",
    "style: TextStyle(fontSize: 21, color: HomeMindColors.textPrimary),",
    "style: TextStyle(fontFamily: 'Roboto', fontSize: 21, color: HomeMindColors.textPrimary),"]],
};

function flutter(cwd, args) {
  const run = spawnSync(`"${FLUTTER}"`, args, { cwd, stdio: "inherit", shell: true });
  if (run.status !== 0) throw new Error(`flutter ${args[0]} failed in ${cwd}`);
}

function record(name) {
  const app = APPS[name];
  const dir = path.join(BUILD, name);
  fs.rmSync(dir, { recursive: true, force: true });
  for (const item of KEEP) {
    const from = path.join(REPO, app.src, item);
    if (fs.existsSync(from)) fs.cpSync(from, path.join(dir, item), { recursive: true });
  }
  for (const [file, from, to] of PATCHES[name] || []) {
    const text = fs.readFileSync(path.join(dir, file), "utf8");
    if (!text.includes(from)) throw new Error(`patch target not found in ${file}`);
    fs.writeFileSync(path.join(dir, file), text.replace(from, to));
  }
  fs.mkdirSync(path.join(dir, "test", "video"), { recursive: true });
  for (const file of ["recorder.dart", app.test]) fs.copyFileSync(path.join(__dirname, file), path.join(dir, "test", "video", file));
  fs.mkdirSync(OUT, { recursive: true });
  flutter(dir, ["pub", "get"]);
  const define = (k, v) => `--dart-define=${k}=${v.replace(/\\/g, "/")}`;
  flutter(dir, ["test", `test/video/${app.test}`, "-r", "expanded",
    define("HM_UI_OUT", OUT), define("HM_FLUTTER_ROOT", FLUTTER_ROOT), define("HM_NOTO_KR", NOTO)]);
}

// out/ui/<clip>/frames.json -> out/ui/<clip>.mp4 (for placing the screens next to the real footage)
function encode(clip) {
  const dir = path.join(OUT, clip);
  const meta = JSON.parse(fs.readFileSync(path.join(dir, "frames.json"), "utf8"));
  const lines = [];
  let i = 0;
  while (i < meta.frames.length) {
    let j = i;
    while (j + 1 < meta.frames.length && meta.frames[j + 1] === meta.frames[i]) j++;
    lines.push(`file '${meta.frames[i]}.png'`, `duration ${((j - i + 1) / meta.fps).toFixed(6)}`);
    i = j + 1;
  }
  lines.push(`file '${meta.frames[meta.frames.length - 1]}.png'`);
  fs.writeFileSync(path.join(dir, "concat.txt"), lines.join("\n"));
  const file = path.join(OUT, `${clip}.mp4`);
  const run = spawnSync(ffmpeg, ["-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", path.join(dir, "concat.txt"),
    "-vf", `fps=${meta.fps},format=yuv420p`, "-t", String(meta.seconds), "-c:v", "libx264", "-preset", "medium", "-crf", "16",
    "-movflags", "+faststart", file], { stdio: "inherit" });
  if (run.status !== 0) throw new Error(`ffmpeg failed for ${clip}`);
  console.log(`${path.relative(VIDEO, file)}: ${meta.width}x${meta.height}, ${meta.seconds}s, ${new Set(meta.frames).size} distinct frames`);
}

const which = process.argv.slice(2).filter((a) => APPS[a]);
for (const name of which.length ? which : Object.keys(APPS)) record(name);
for (const clip of fs.readdirSync(OUT)) if (fs.existsSync(path.join(OUT, clip, "frames.json"))) encode(clip);
