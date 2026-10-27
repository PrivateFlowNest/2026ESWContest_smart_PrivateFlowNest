// Render the diagram scenes to 1280x720 30 fps H.264 clips, one frame at a time (deterministic, no real-time playback).
//   node render.js                     all scenes -> out/<scene>.mp4
//   node render.js hidden quiet        only these scenes
//   node render.js --stills hidden:3,9,15 system:4    PNG stills for checking -> out/still-<scene>-<t>.png
const http = require("http");
const fs = require("fs");
const path = require("path");
const { spawn } = require("child_process");
const puppeteer = require("puppeteer");
const ffmpegPath = require("ffmpeg-static");

const ROOT = __dirname;
const OUT = path.join(ROOT, "out");
const FPS = 30;
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".mjs": "text/javascript", ".json": "application/json", ".png": "image/png" };

function serve() {
  const server = http.createServer((req, res) => {
    const file = path.normalize(path.join(ROOT, decodeURIComponent(new URL(req.url, "http://x").pathname)));
    if (!file.startsWith(ROOT) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { "Content-Type": TYPES[path.extname(file)] || "application/octet-stream" });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, "127.0.0.1", () => resolve(server)));
}

async function encode(page, name, duration) {
  const file = path.join(OUT, `${name}.mp4`);
  const ff = spawn(ffmpegPath, ["-y", "-loglevel", "error", "-f", "image2pipe", "-framerate", String(FPS), "-c:v", "png", "-i", "-",
    "-c:v", "libx264", "-preset", "medium", "-crf", "17", "-pix_fmt", "yuv420p", "-movflags", "+faststart", file], { stdio: ["pipe", "inherit", "inherit"] });
  const done = new Promise((resolve, reject) => ff.on("close", (code) => (code === 0 ? resolve() : reject(new Error(`ffmpeg ${code}`)))));
  const frames = Math.round(duration * FPS);
  const started = Date.now();
  for (let i = 0; i < frames; i++) {
    await page.evaluate((n, t) => window.renderAt(n, t), name, i / FPS);
    const png = await page.screenshot({ type: "png", optimizeForSpeed: true });
    if (!ff.stdin.write(png)) await new Promise((r) => ff.stdin.once("drain", r));
    if (i % 150 === 0) process.stdout.write(`  ${name} ${i}/${frames}\r`);
  }
  ff.stdin.end();
  await done;
  console.log(`${name}: ${frames} frames, ${duration}s -> ${path.relative(ROOT, file)} (${((Date.now() - started) / 1000).toFixed(0)}s)`);
}

async function main() {
  const args = process.argv.slice(2);
  fs.mkdirSync(OUT, { recursive: true });
  const server = await serve();
  const browser = await puppeteer.launch({ headless: true, args: ["--enable-unsafe-swiftshader", "--ignore-gpu-blocklist", "--force-device-scale-factor=1"] });
  try {
    const page = await browser.newPage();
    page.on("pageerror", (e) => console.error("page error:", e.message));
    page.on("console", (m) => { if (m.type() === "error" || m.type() === "warning") console.error("console:", m.text()); });
    await page.setViewport({ width: 1280, height: 720, deviceScaleFactor: 1 });
    await page.goto(`http://127.0.0.1:${server.address().port}/scenes/index.html`);
    await page.waitForFunction("window.ready === true", { timeout: 60000 });
    const meta = await page.evaluate(() => window.SCENES_META);
    const stillsAt = args.indexOf("--stills");
    if (stillsAt >= 0) {
      for (const spec of args.slice(stillsAt + 1)) {
        const [name, times] = spec.split(":");
        for (const t of times.split(",").map(Number)) {
          await page.evaluate((n, tt) => window.renderAt(n, tt), name, t);
          await page.screenshot({ path: path.join(OUT, `still-${name}-${t}.png`) });
        }
      }
      console.log("stills written to out/");
      return;
    }
    const names = args.length ? args : Object.keys(meta);
    for (const name of names) await encode(page, name, meta[name].duration);
  } finally {
    await browser.close();
    server.close();
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
