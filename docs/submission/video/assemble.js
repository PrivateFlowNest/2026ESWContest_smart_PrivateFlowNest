// Join the rendered diagram clips and the real-device footage into one 1280x720 30 fps video (limit: 3 minutes).
//   node assemble.js            -> out/2026ESWContest_스마트가전_PrivateFlowNest_시연동영상.mp4
// Real footage goes in footage/<name>.mp4 (filmed on the model); a clip that is missing is skipped, so the same
// command makes the diagram-only draft now and the final cut once the footage is in.
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");
const ffmpeg = require("ffmpeg-static");

const ROOT = __dirname;
const OUT = path.join(ROOT, "out");
const ORDER = [
  "out/intro.mp4",
  "out/system.mp4",
  "footage/model-tour.mp4",
  "out/hidden.mp4",
  "out/ui-hidden.mp4",
  "footage/hidden-intent.mp4",
  "out/quiet.mp4",
  "out/ui-quiet.mp4",
  "footage/quiet-mode.mp4",
  "out/doors.mp4",
  "out/ui-doors.mp4",
  "footage/window-door.mp4",
  "out/partial.mp4",
  "out/ui-partial.mp4",
  "footage/partial-hint.mp4",
  "out/privacy.mp4",
  "footage/camera-page.mp4",
  "out/outro.mp4",
];
const LIMIT = 180;

const parts = ORDER.map((p) => path.join(ROOT, p)).filter((p) => fs.existsSync(p));
const missing = ORDER.filter((p) => !fs.existsSync(path.join(ROOT, p)));
// every input is scaled and padded to 1280x720 30 fps; footage keeps its own sound (the spoken commands),
// the diagram clips get silence, then everything is concatenated
const inputs = parts.flatMap((p) => ["-i", p]);
const chains = parts.map((p, i) =>
  `[${i}:v]scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:color=0x0A1C24,fps=30,format=yuv420p,setsar=1[v${i}]`);
const audio = parts.map((p, i) => {
  const { seconds, sound } = probe(p);
  return sound
    ? `[${i}:a]aresample=48000,aformat=channel_layouts=stereo,apad,atrim=0:${seconds.toFixed(3)}[a${i}]`
    : `anullsrc=channel_layout=stereo:sample_rate=48000,atrim=0:${seconds.toFixed(3)}[a${i}]`;
});
const concat = `${parts.map((_, i) => `[v${i}][a${i}]`).join("")}concat=n=${parts.length}:v=1:a=1[v][a]`;
const file = path.join(OUT, "2026ESWContest_스마트가전_PrivateFlowNest_시연동영상.mp4");
const run = spawnSync(ffmpeg, ["-y", "-loglevel", "error", ...inputs, "-filter_complex", [...chains, ...audio, concat].join(";"),
  "-map", "[v]", "-map", "[a]", "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart", file], { stdio: "inherit" });
if (run.status !== 0) process.exit(run.status || 1);
const total = probe(file).seconds;
console.log(`${path.relative(ROOT, file)}: ${total.toFixed(1)} s from ${parts.length} clips${total > LIMIT ? `  (OVER the ${LIMIT} s limit)` : ""}`);
if (missing.length) console.log(`not found (skipped): ${missing.join(", ")}`);

function probe(p) {
  // ffmpeg prints the input's "Duration: hh:mm:ss.xx" and its streams; ffmpeg-static has no ffprobe
  const info = spawnSync(ffmpeg, ["-hide_banner", "-i", p], { encoding: "utf8" }).stderr;
  const m = /Duration: (\d+):(\d+):(\d+\.\d+)/.exec(info);
  return { seconds: m ? Number(m[1]) * 3600 + Number(m[2]) * 60 + Number(m[3]) : 0, sound: /Stream #\S+.*Audio:/.test(info) };
}
