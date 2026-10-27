// HomeMind TaskFlow demo video: diagram animations of how the system works (not footage of the real devices).
// Everything is a pure function of time: window.renderAt(scene, t) resets the world and applies that scene at t,
// so render.js can step frame by frame and the output is the same on every run.
import * as THREE from "three";
import { RoundedBoxGeometry } from "three/addons/geometries/RoundedBoxGeometry.js";
import { CSS2DRenderer, CSS2DObject } from "three/addons/renderers/CSS2DRenderer.js";

const W = 1280, H = 720;
const COLOR = {
  bg: 0x0a1c24, floor: 0x0d242c, house: 0x12313a, edge: 0x2a6b77, case: 0xe9eef0, teal: 0x2a8c9a, lan: 0x39b5c4,
  wifi: 0xf2b33d, orange: 0xe8743b, pcb: 0x1d3b33, purple: 0x9d84ff,
};

// ------------------------------------------------------------------ helpers
const clamp01 = (x) => Math.min(1, Math.max(0, x));
const prog = (t, a, b) => clamp01((t - a) / (b - a));
const smooth = (x) => x * x * (3 - 2 * x);
const inOut = (x) => (x < 0.5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2);
const outBack = (x) => { const c1 = 1.70158, c3 = c1 + 1; return 1 + c3 * Math.pow(x - 1, 3) + c1 * Math.pow(x - 1, 2); };
// 0 -> 1 over [a, a+f], 1 -> 0 over [b-f, b]
const win = (t, a, b, f = 0.35) => (t < a || t > b ? 0 : Math.min(prog(t, a, a + f), 1 - prog(t, b - f, b)));
const v3 = (x, y, z) => new THREE.Vector3(x, y, z);

// ------------------------------------------------------------------ renderer, scene, camera
const renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true });
renderer.setPixelRatio(1);
renderer.setSize(W, H);
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.05;
document.getElementById("stage").appendChild(renderer.domElement);
const labels = new CSS2DRenderer();
labels.setSize(W, H);
labels.domElement.id = "labels";
document.getElementById("stage").appendChild(labels.domElement);

const scene = new THREE.Scene();
scene.background = new THREE.Color(COLOR.bg);
scene.fog = new THREE.Fog(COLOR.bg, 22, 42);
const camera = new THREE.PerspectiveCamera(34, W / H, 0.1, 200);
scene.add(new THREE.HemisphereLight(0xdff3f6, 0x0a1c24, 1.5));
const sun = new THREE.DirectionalLight(0xffffff, 2.0);
sun.position.set(5, 11, 7);
scene.add(sun);

function radialTexture(inner, outer) {
  const c = document.createElement("canvas");
  c.width = c.height = 128;
  const g = c.getContext("2d");
  const grad = g.createRadialGradient(64, 64, 0, 64, 64, 64);
  grad.addColorStop(0, inner);
  grad.addColorStop(1, outer);
  g.fillStyle = grad;
  g.fillRect(0, 0, 128, 128);
  return new THREE.CanvasTexture(c);
}
const shadowTex = radialTexture("rgba(0,0,0,0.55)", "rgba(0,0,0,0)");
const haloTex = radialTexture("rgba(255,255,255,1)", "rgba(255,255,255,0)");

// floor, grid and the house platform
const floor = new THREE.Mesh(new THREE.PlaneGeometry(80, 80), new THREE.MeshStandardMaterial({ color: COLOR.floor, roughness: 1 }));
floor.rotation.x = -Math.PI / 2;
scene.add(floor);
const grid = new THREE.GridHelper(60, 60, 0x1c4652, 0x143843);
grid.material.transparent = true;
grid.material.opacity = 0.35;
grid.position.y = 0.002;
scene.add(grid);
const house = new THREE.Mesh(new RoundedBoxGeometry(10.6, 0.12, 7.4, 3, 0.05), new THREE.MeshStandardMaterial({ color: COLOR.house, roughness: 0.9 }));
house.position.set(0, 0.06, 0.45);
scene.add(house);
const houseEdge = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.BoxGeometry(10.6, 0.12, 7.4)), new THREE.LineBasicMaterial({ color: COLOR.edge, transparent: true, opacity: 0.7 }));
houseEdge.position.copy(house.position);
scene.add(houseEdge);
const BASE = 0.12;

function label(html, cls, pos, parent = scene, center = [0.5, 1]) {
  const el = document.createElement("div");
  el.className = cls;
  el.innerHTML = html;
  const obj = new CSS2DObject(el);
  obj.center.set(center[0], center[1]);
  obj.position.copy(pos);
  parent.add(obj);
  return { obj, el };
}

const shadows = [];
function contactShadow(x, z, w, d) {
  const m = new THREE.Mesh(new THREE.PlaneGeometry(w, d), new THREE.MeshBasicMaterial({ map: shadowTex, transparent: true, depthWrite: false }));
  m.rotation.x = -Math.PI / 2;
  m.position.set(x, BASE + 0.003, z);
  scene.add(m);
  shadows.push(m);
  return m;
}

// ------------------------------------------------------------------ devices
const devices = {};
function device(key, { title, sub, x, z, w, h, d, body = COLOR.case, accent = COLOR.teal }) {
  const group = new THREE.Group();
  group.position.set(x, BASE, z);
  const mat = new THREE.MeshStandardMaterial({ color: body, roughness: 0.55, metalness: 0.05 });
  const box = new THREE.Mesh(new RoundedBoxGeometry(w, h, d, 4, Math.min(w, h, d) * 0.18), mat);
  box.position.y = h / 2;
  group.add(box);
  const topMat = new THREE.MeshStandardMaterial({ color: accent, roughness: 0.35, emissive: accent, emissiveIntensity: 0.1 });
  const top = new THREE.Mesh(new RoundedBoxGeometry(w * 0.9, 0.05, d * 0.9, 2, 0.02), topMat);
  top.position.y = h + 0.012;
  group.add(top);
  const glow = new THREE.Mesh(new THREE.PlaneGeometry(w * 2.6, d * 2.6), new THREE.MeshBasicMaterial({ map: haloTex, color: accent, transparent: true, opacity: 0, depthWrite: false, blending: THREE.AdditiveBlending }));
  glow.rotation.x = -Math.PI / 2;
  glow.position.y = 0.01;
  group.add(glow);
  scene.add(group);
  contactShadow(x, z, w * 1.5, d * 1.5);
  const tag = label(`<b>${title}</b>${sub ? `<span>${sub}</span>` : ""}`, "tag", v3(0, h + 0.35, 0), group);
  devices[key] = { group, topMat, glow, tag, port: v3(x, BASE + h + 0.03, z), h };
  return devices[key];
}

device("pi4", { title: "Raspberry Pi 4", sub: "백엔드 · MQTT 브로커 · core", x: 0, z: -1.95, w: 1.35, h: 0.42, d: 0.92, accent: 0x0e4a55 });
device("pi502", { title: "Raspberry Pi 5", sub: "인식 · 음성 · 소리 · 영상", x: -3.45, z: -1.45, w: 1.2, h: 0.42, d: 0.85 });
device("pi501", { title: "Raspberry Pi 5", sub: "UI · EXAONE LLM", x: 3.45, z: -1.35, w: 1.2, h: 0.42, d: 0.85, accent: 0x6f5bd6 });
device("esp32", { title: "ESP32", sub: "현관문 · 창문", x: -2.5, z: 2.05, w: 0.95, h: 0.14, d: 0.62, body: COLOR.pcb, accent: COLOR.orange });
device("esp8266", { title: "ESP8266", sub: "조명 · 에어컨 표시등 · 팬", x: -0.55, z: 2.0, w: 0.85, h: 0.14, d: 0.58, body: COLOR.pcb, accent: COLOR.orange });
device("phone", { title: "휴대폰 앱", sub: "승인 · 제어 · 알림", x: 3.7, z: 2.15, w: 0.001, h: 0.001, d: 0.001, body: 0x000000, accent: COLOR.teal });
devices.phone.tag.obj.position.y = 1.55;

// router
const router = new THREE.Group();
router.position.set(0, BASE, 0.55);
const routerBody = new THREE.Mesh(new RoundedBoxGeometry(1.0, 0.16, 0.62, 3, 0.05), new THREE.MeshStandardMaterial({ color: 0x223b44, roughness: 0.5 }));
routerBody.position.y = 0.08;
router.add(routerBody);
for (const dx of [-0.32, 0.32]) {
  const ant = new THREE.Mesh(new THREE.CylinderGeometry(0.025, 0.025, 0.55), new THREE.MeshStandardMaterial({ color: 0x2c4a54 }));
  ant.position.set(dx, 0.42, -0.2);
  router.add(ant);
}
const routerLeds = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.02, 0.02), new THREE.MeshBasicMaterial({ color: 0x7fe3ee }));
routerLeds.position.set(0, 0.12, 0.315);
router.add(routerLeds);
scene.add(router);
contactShadow(0, 0.55, 1.6, 1.0);
const routerTag = label("<b>집 공유기</b><span>유선 LAN · 2.4 GHz Wi-Fi</span>", "tag", v3(0, 0.95, 0), router);
const HUB = v3(0, BASE + 0.2, 0.55);

// Pi5-02 peripherals: USB microphone and two webcams
const mic = new THREE.Group();
mic.position.set(-2.35, BASE, -0.95);
mic.add(new THREE.Mesh(new THREE.CylinderGeometry(0.16, 0.18, 0.05, 24), new THREE.MeshStandardMaterial({ color: 0x2b3b41 })));
const micStem = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.03, 0.42), new THREE.MeshStandardMaterial({ color: 0x3d4f56 }));
micStem.position.y = 0.23;
mic.add(micStem);
const micHead = new THREE.Mesh(new THREE.CapsuleGeometry(0.075, 0.12, 6, 16), new THREE.MeshStandardMaterial({ color: 0x9aa9ae, roughness: 0.4 }));
micHead.position.y = 0.52;
mic.add(micHead);
scene.add(mic);
const micTag = label("USB 마이크", "mini", v3(0, 0.8, 0), mic);

function webcam(x, z, lookAt, name) {
  const g = new THREE.Group();
  g.position.set(x, BASE, z);
  const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.025, 0.025, 1.1), new THREE.MeshStandardMaterial({ color: 0x3d4f56 }));
  pole.position.y = 0.55;
  g.add(pole);
  const head = new THREE.Group();
  head.position.y = 1.15;
  const bodyMesh = new THREE.Mesh(new RoundedBoxGeometry(0.34, 0.18, 0.16, 2, 0.04), new THREE.MeshStandardMaterial({ color: 0x1b2328 }));
  head.add(bodyMesh);
  const lens = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.05, 0.04, 20), new THREE.MeshStandardMaterial({ color: 0x0b0f12, emissive: 0x39b5c4, emissiveIntensity: 0.4 }));
  lens.rotation.x = Math.PI / 2;
  lens.position.z = 0.09;
  head.add(lens);
  g.add(head);
  scene.add(g);
  const world = v3(x, BASE + 1.15, z);
  head.lookAt(lookAt);
  // view cone from the lens toward the target
  const dir = new THREE.Vector3().subVectors(lookAt, world);
  const len = dir.length();
  const cone = new THREE.Mesh(new THREE.ConeGeometry(0.75, len, 32, 1, true), new THREE.MeshBasicMaterial({ color: COLOR.lan, transparent: true, opacity: 0, depthWrite: false, side: THREE.DoubleSide }));
  cone.position.copy(world).addScaledVector(dir, 0.5);
  cone.quaternion.setFromUnitVectors(v3(0, -1, 0), dir.clone().normalize());
  scene.add(cone);
  const tag = label(name, "mini", v3(0, 1.45, 0), g);
  return { cone, tag, g };
}
const PERSON = v3(-1.55, BASE, -0.15);
const camRoom = webcam(-4.75, -0.35, v3(PERSON.x, 0.6, PERSON.z), "웹캠 · 방");
const camEntry = webcam(-4.85, 1.45, v3(-4.0, 0.5, 3.2), "웹캠 · 현관");

// a person (the demo doll) at a desk
const person = new THREE.Group();
person.position.copy(PERSON);
const desk = new THREE.Mesh(new RoundedBoxGeometry(1.0, 0.45, 0.55, 2, 0.04), new THREE.MeshStandardMaterial({ color: 0x5b4a3a, roughness: 0.8 }));
desk.position.set(0.0, 0.225, 0.55);
person.add(desk);
const torso = new THREE.Mesh(new THREE.CapsuleGeometry(0.2, 0.42, 8, 16), new THREE.MeshStandardMaterial({ color: 0xe8743b, roughness: 0.6 }));
torso.position.y = 0.5;
person.add(torso);
const head = new THREE.Mesh(new THREE.SphereGeometry(0.17, 24, 16), new THREE.MeshStandardMaterial({ color: 0xf1d2b6, roughness: 0.7 }));
head.position.y = 1.02;
person.add(head);
scene.add(person);
contactShadow(PERSON.x, PERSON.z + 0.2, 1.4, 1.2);
const bubble = label("", "bubble", v3(0.15, 1.35, 0), person, [0.12, 1]);
const MOUTH = v3(PERSON.x, BASE + 1.0, PERSON.z);
// sound rings
const rings = [];
for (let i = 0; i < 3; i++) {
  const r = new THREE.Mesh(new THREE.TorusGeometry(1, 0.012, 8, 64), new THREE.MeshBasicMaterial({ color: 0x7fe3ee, transparent: true, opacity: 0 }));
  r.position.copy(MOUTH);
  scene.add(r);
  rings.push(r);
}

// 7-inch display next to Pi5-01 and the phone; their screens show the recorded UI (see ui/record.js)
function screenMesh(w, h, px, py) {
  const c = document.createElement("canvas");
  c.width = px;
  c.height = py;
  const tex = new THREE.CanvasTexture(c);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = 4;
  const mesh = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ map: tex, toneMapped: false }));
  return { mesh, canvas: c, tex, key: null };
}
const display = new THREE.Group();
display.position.set(3.45, BASE, -2.45);
const displayFrame = new THREE.Mesh(new RoundedBoxGeometry(2.15, 1.27, 0.1, 3, 0.04), new THREE.MeshStandardMaterial({ color: 0x14191c, roughness: 0.5 }));
displayFrame.position.y = 1.18;
display.add(displayFrame);
const stand = new THREE.Mesh(new THREE.BoxGeometry(0.12, 0.55, 0.08), new THREE.MeshStandardMaterial({ color: 0x2b3438 }));
stand.position.set(0, 0.28, -0.05);
display.add(stand);
const ui = screenMesh(2.0, 1.125, 1280, 720);
ui.mesh.position.set(0, 1.18, 0.053);
display.add(ui.mesh);
display.rotation.x = -0.08;
scene.add(display);
label("7인치 터치 디스플레이", "mini", v3(0, 1.92, 0), display);

const phone = new THREE.Group();
phone.position.set(3.7, BASE + 0.05, 2.15);
const phoneBody = new THREE.Mesh(new RoundedBoxGeometry(0.62, 1.3, 0.07, 3, 0.06), new THREE.MeshStandardMaterial({ color: 0x0f1416, roughness: 0.4 }));
phoneBody.position.y = 0.64;
phone.add(phoneBody);
const pscreen = screenMesh(0.56, 1.212, 390, 844);
pscreen.mesh.position.set(0, 0.64, 0.037);
phone.add(pscreen.mesh);
phone.rotation.set(-0.22, -0.28, 0);
scene.add(phone);
contactShadow(3.7, 2.2, 0.9, 0.5);
devices.phone.port = v3(3.85, BASE + 0.12, 1.92);

// ESP32 room fittings: entrance door and window (open/close motors)
function hinged(x, z, w, h, color, name) {
  const g = new THREE.Group();
  g.position.set(x, BASE, z);
  const frameMat = new THREE.MeshStandardMaterial({ color: 0x9fb1b7, roughness: 0.6 });
  const post = (px, ph, py) => { const m = new THREE.Mesh(new THREE.BoxGeometry(0.06, ph, 0.08), frameMat); m.position.set(px, py, 0); g.add(m); };
  post(-w / 2 - 0.03, h, h / 2 + (name === "창문" ? 0.5 : 0));
  post(w / 2 + 0.03, h, h / 2 + (name === "창문" ? 0.5 : 0));
  const lintel = new THREE.Mesh(new THREE.BoxGeometry(w + 0.12, 0.06, 0.08), frameMat);
  lintel.position.y = h + (name === "창문" ? 0.5 : 0) + 0.03;
  g.add(lintel);
  if (name === "창문") {
    const sill = lintel.clone();
    sill.position.y = 0.47;
    g.add(sill);
    const wall = new THREE.Mesh(new THREE.BoxGeometry(w + 0.12, 0.47, 0.08), frameMat);
    wall.position.y = 0.235;
    g.add(wall);
  }
  const pivot = new THREE.Group();
  pivot.position.set(-w / 2, name === "창문" ? 0.5 : 0, 0);
  const panel = new THREE.Mesh(new THREE.BoxGeometry(w, h, 0.05), new THREE.MeshStandardMaterial({ color, roughness: 0.5, transparent: name === "창문", opacity: name === "창문" ? 0.75 : 1 }));
  panel.position.set(w / 2, h / 2, 0);
  pivot.add(panel);
  g.add(pivot);
  scene.add(g);
  contactShadow(x, z, w + 0.6, 0.5);
  const tag = label(name, "mini", v3(0, 0.02, 0.32), g, [0.5, 0]);
  return { pivot, tag, g };
}
const door = hinged(-3.85, 3.35, 0.7, 1.25, 0x8a5a3c, "현관문");
const windowSash = hinged(-1.45, 3.35, 0.85, 0.7, 0x9ad7e3, "창문");

// ESP8266 fittings: living light, aircon indicator, bathroom fan
function bulb(x, z, color, name, onBody) {
  const g = new THREE.Group();
  g.position.set(x, BASE, z);
  const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.025, 0.025, 0.85), new THREE.MeshStandardMaterial({ color: 0x3d4f56 }));
  pole.position.y = 0.42;
  g.add(pole);
  let anchorY = 0.95;
  if (onBody) {
    const unit = new THREE.Mesh(new RoundedBoxGeometry(0.95, 0.3, 0.26, 3, 0.06), new THREE.MeshStandardMaterial({ color: 0xeef2f3, roughness: 0.5 }));
    unit.position.y = 0.98;
    g.add(unit);
    const vent = new THREE.Mesh(new THREE.BoxGeometry(0.8, 0.03, 0.02), new THREE.MeshStandardMaterial({ color: 0xa9b5b9 }));
    vent.position.set(0, 0.88, 0.13);
    g.add(vent);
    anchorY = 1.02;
  }
  const mat = new THREE.MeshStandardMaterial({ color: 0x2a3236, emissive: color, emissiveIntensity: 0 });
  const led = new THREE.Mesh(new THREE.SphereGeometry(onBody ? 0.05 : 0.13, 20, 14), mat);
  led.position.set(onBody ? 0.36 : 0, anchorY, onBody ? 0.135 : 0);
  g.add(led);
  const halo = new THREE.Sprite(new THREE.SpriteMaterial({ map: haloTex, color, transparent: true, opacity: 0, depthWrite: false, blending: THREE.AdditiveBlending }));
  halo.scale.setScalar(onBody ? 0.5 : 1.1);
  halo.position.copy(led.position);
  g.add(halo);
  scene.add(g);
  contactShadow(x, z, 0.5, 0.5);
  const tag = label(name, "mini", v3(0, 0.02, 0.32), g, [0.5, 0]);
  return { mat, halo, color, tag, g };
}
const living = bulb(0.2, 3.3, 0xffd58a, "거실 조명", false);
const aircon = bulb(1.35, 3.3, 0x5fd0ff, "에어컨 표시등", true);
const fan = new THREE.Group();
fan.position.set(2.6, BASE, 3.3);
const fanPole = new THREE.Mesh(new THREE.CylinderGeometry(0.025, 0.025, 0.7), new THREE.MeshStandardMaterial({ color: 0x3d4f56 }));
fanPole.position.y = 0.35;
fan.add(fanPole);
const fanRing = new THREE.Mesh(new THREE.TorusGeometry(0.3, 0.025, 8, 40), new THREE.MeshStandardMaterial({ color: 0xbfc9cc }));
fanRing.position.y = 0.95;
fan.add(fanRing);
const blades = new THREE.Group();
blades.position.y = 0.95;
for (let i = 0; i < 4; i++) {
  const b = new THREE.Mesh(new THREE.BoxGeometry(0.24, 0.07, 0.01), new THREE.MeshStandardMaterial({ color: 0x8fa1a6 }));
  b.position.x = 0.13;
  const arm = new THREE.Group();
  arm.rotation.z = (i * Math.PI) / 2;
  arm.add(b);
  blades.add(arm);
}
fan.add(blades);
scene.add(fan);
contactShadow(2.6, 3.3, 0.5, 0.5);
label("화장실 환기팬", "mini", v3(0, 0.02, 0.32), fan, [0.5, 0]);

// ------------------------------------------------------------------ links (LAN for the Pis, Wi-Fi for ESP and phone)
const links = {};
function link(key, kind) {
  const a = devices[key].port;
  const mid = v3((a.x + HUB.x) / 2, Math.max(a.y, HUB.y) + 0.9, (a.z + HUB.z) / 2);
  const curve = new THREE.QuadraticBezierCurve3(a.clone(), mid, HUB.clone());
  const tube = new THREE.Mesh(new THREE.TubeGeometry(curve, 64, kind === "lan" ? 0.028 : 0.018, 8, false),
    new THREE.MeshBasicMaterial({ color: kind === "lan" ? COLOR.lan : COLOR.wifi, transparent: true, opacity: kind === "lan" ? 0.85 : 0.6 }));
  scene.add(tube);
  links[key] = { curve, tube, count: tube.geometry.index.count };
}
for (const k of ["pi4", "pi502", "pi501"]) link(k, "lan");
for (const k of ["esp32", "esp8266", "phone"]) link(k, "wifi");

// packets: a glowing ball with a label chip, moving along device -> router -> (broker Pi4) -> device
const packets = [];
for (let i = 0; i < 6; i++) {
  const ball = new THREE.Mesh(new THREE.SphereGeometry(0.09, 16, 12), new THREE.MeshBasicMaterial({ color: 0xffffff }));
  const halo = new THREE.Sprite(new THREE.SpriteMaterial({ map: haloTex, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending }));
  halo.scale.setScalar(0.6);
  ball.add(halo);
  scene.add(ball);
  const tag = label("", "packet", v3(0, 0.22, 0), ball);
  packets.push({ ball, halo, tag });
}
const KIND_COLOR = { data: 0x7fe3ee, cmd: 0xffb27d, ack: 0x9be7a6, ai: 0xc8b6ff, block: 0xff7a7a };
function routeCurves(from, to) {
  const seg = [];
  const up = (k) => links[k].curve;
  const down = (k) => { const c = links[k].curve; return new THREE.QuadraticBezierCurve3(c.v2.clone(), c.v1.clone(), c.v0.clone()); };
  if (from !== "hub") seg.push(up(from));
  if (to !== "hub") seg.push(down(to));
  return seg;
}
function pointOn(segs, u) {
  const lens = segs.map((c) => c.getLength());
  const total = lens.reduce((a, b) => a + b, 0);
  let d = u * total;
  for (let i = 0; i < segs.length; i++) {
    if (d <= lens[i] || i === segs.length - 1) return segs[i].getPointAt(clamp01(d / lens[i]));
    d -= lens[i];
  }
}

// ------------------------------------------------------------------ screens
// Recorded UI (ui/record.js -> out/ui/<clip>/frames.json + PNGs): the real 7-inch UI and phone app, one clip per
// scenario. The UI scenes (ui-*) show them full size; the 3D scenes show them small on the display and the phone,
// with the 3D scene's moments mapped onto the clip: [3D time, clip time] keys, the clip runs on from a key and
// holds just before the next one. Scenes without a mapping show the first frame (idle).
const UI_SYNC = {
  hidden: { panel: ["panel-hidden", [[0, 0], [14.8, 3.0], [16.6, 6.4], [20.6, 8.0]]],
    phone: ["phone-hidden", [[0, 0], [14.9, 3.1], [16.1, 5.8], [20.8, 8.2]]] },
  quiet: { panel: ["panel-quiet", [[0, 0], [9.0, 1.0], [10.7, 2.2], [11.7, 3.0], [15.4, 6.0], [17.0, 7.2]]] },
  doors: { panel: ["panel-doors", [[0, 0], [3.9, 2.0], [4.9, 2.6], [8.9, 4.2], [13.8, 7.2], [16.9, 10.4], [17.3, 10.8], [21.5, 12.6]]],
    phone: ["phone-doors", [[0, 0], [8.9, 4.4], [13.9, 7.3], [17.4, 10.9], [21.5, 12.8]]] },
  partial: { panel: ["panel-partial", [[0, 0], [3.4, 1.6], [6.2, 4.0], [6.5, 4.3], [9.1, 5.8]]] },
};
const IDLE = { panel: "panel-hidden", phone: "phone-hidden" };
const clips = {};
async function loadClips() {
  for (const scenario of ["hidden", "quiet", "doors", "partial"]) {
    for (const name of [`panel-${scenario}`, `phone-${scenario}`]) {
      const r = await fetch(`/out/ui/${name}/frames.json`);
      if (r.ok) clips[name] = await r.json();
    }
  }
}
function clipTime(keys, t) {
  let i = 0;
  while (i + 1 < keys.length && t >= keys[i + 1][0]) i++;
  const [t0, c0] = keys[i], next = keys[i + 1];
  return next ? Math.min(c0 + (t - t0), next[1] - 1 / 30) : c0 + (t - t0);
}
// "<clip>/<file>" of the frame showing at clip time t, or null when the clip was not recorded
function frameKey(clip, t) {
  const meta = clips[clip];
  if (!meta) return null;
  return `${clip}/${meta.frames[Math.min(meta.frames.length - 1, Math.max(0, Math.floor(t * meta.fps + 1e-6)))]}`;
}
// on a 3D screen (canvas texture); frames are loaded when they change
async function showFrame(screen, key) {
  if (screen.key === (key || "none")) return;
  screen.key = key || "none";
  const g = screen.canvas.getContext("2d");
  if (!key) { g.fillStyle = "#0E1726"; g.fillRect(0, 0, screen.canvas.width, screen.canvas.height); screen.tex.needsUpdate = true; return; }
  const img = await createImageBitmap(await (await fetch(`/out/ui/${key}.png`)).blob());
  g.drawImage(img, 0, 0, screen.canvas.width, screen.canvas.height);
  img.close();
  screen.tex.needsUpdate = true;
}
async function showScreens(name, t) {
  const sync = UI_SYNC[name] || {};
  const at = (which) => (sync[which] ? frameKey(sync[which][0], clipTime(sync[which][1], t)) : frameKey(IDLE[which], 0));
  await showFrame(ui, at("panel"));
  await showFrame(pscreen, at("phone"));
}
// on the UI scenes' <img>
async function showImage(img, key) {
  if (img.dataset.key === key) return;
  img.dataset.key = key;
  img.src = key ? `/out/ui/${key}.png` : "";
  if (key) await img.decode();
}
// UI scene layout: the 7-inch UI with the phone beside it, or the 7-inch UI alone (bigger)
const FLAT = {
  both: { panel: [36, 104, 880, 495], phone: [962, 84, 277, 600] },
  panel: { panel: [160, 80, 960, 540] },
};
const flatLayout = (def) => FLAT[def.flat.phone ? "both" : "panel"];
async function showFlat(def, t) {
  const box = (el, r) => { el.style.display = r ? "" : "none"; if (r) Object.assign(el.style, { left: `${r[0]}px`, top: `${r[1]}px`, width: `${r[2]}px`, height: `${r[3]}px` }); };
  const layout = flatLayout(def);
  box($("flatPanel"), layout.panel);
  box($("flatPhone"), layout.phone);
  await showImage($("flatPanel").querySelector("img"), frameKey(def.flat.panel, t));
  if (def.flat.phone) await showImage($("flatPhone").querySelector("img"), frameKey(def.flat.phone, t));
  // where the scene cuts a wait (EXAONE), say so on the screen
  const cut = def.flat.cut;
  const showCut = cut && t >= cut[0] && t <= cut[1];
  $("cut").style.display = showCut ? "block" : "none";
  if (showCut) {
    $("cut").textContent = cut[2];
    $("cut").style.left = `${layout.panel[0] + layout.panel[2] / 2}px`;
    $("cut").style.top = `${layout.panel[1] + layout.panel[3] / 2}px`;
    $("cut").style.opacity = win(t, cut[0], cut[1], 0.2);
  }
}

// ------------------------------------------------------------------ overlay
const $ = (id) => document.getElementById(id);
function overlay({ chapter = "", caption = "", sub = "", capOpacity = 1, card = null, cardOpacity = 0, kind = true, kindText = "동작 원리 애니메이션", low = false, brand = true }) {
  $("kind").textContent = kindText;
  $("caption").className = low ? "low" : "";
  $("caption").style.left = "";
  $("caption").style.maxWidth = "";
  $("chapter").textContent = chapter;
  $("chapter").style.display = chapter ? "" : "none";
  $("caption").innerHTML = caption ? `${caption}${sub ? `<small>${sub}</small>` : ""}` : "";
  $("caption").style.display = caption ? "" : "none";
  $("caption").style.opacity = capOpacity;
  $("kind").style.display = kind ? "" : "none";
  $("brand").style.display = brand ? "" : "none";
  $("card").style.display = card && cardOpacity > 0 ? "flex" : "none";
  labels.domElement.style.visibility = card && cardOpacity > 0.05 ? "hidden" : "visible";
  $("card").style.opacity = cardOpacity;
  if (card) { $("card").querySelector("h1").textContent = card[0]; $("card").querySelector("h2").textContent = card[1] || ""; $("card").querySelector("p").textContent = card[2] || ""; }
}
// captions: [[t0, t1, text, sub]] -> the one showing at t, faded in and out
function captionAt(t, list) {
  for (const [a, b, text, sub] of list) if (t >= a && t <= b) return { caption: text, sub, capOpacity: win(t, a, b, 0.3) };
  return { caption: "" };
}

// what pops in with each controller in the system scene
const APPEAR = {
  pi4: [devices.pi4.group], router: [router],
  pi502: [devices.pi502.group, mic, person, camRoom.g, camEntry.g], pi501: [devices.pi501.group, display],
  esp32: [devices.esp32.group, door.g, windowSash.g], esp8266: [devices.esp8266.group, living.g, aircon.g, fan],
  phone: [devices.phone.group, phone],
};

// ------------------------------------------------------------------ per-frame state
function reset() {
  for (const list of Object.values(APPEAR)) for (const o of list) { o.scale.setScalar(1); o.visible = true; }
  for (const m of shadows) m.material.opacity = 1;
  for (const d of Object.values(devices)) { d.group.scale.setScalar(1); d.glow.material.opacity = 0; d.topMat.emissiveIntensity = 0.1; d.tag.el.style.opacity = 1; d.tag.obj.visible = true; }
  for (const l of Object.values(links)) { l.tube.geometry.setDrawRange(0, l.count); l.tube.visible = true; }
  for (const p of packets) { p.ball.visible = false; p.tag.obj.visible = false; }
  for (const r of rings) r.material.opacity = 0;
  for (const c of [camRoom, camEntry]) c.cone.material.opacity = 0;
  routerTag.obj.visible = true; micTag.obj.visible = true; camRoom.tag.obj.visible = true; camEntry.tag.obj.visible = true;
  shotNow = "";
  bubble.obj.visible = false;
  setLight(living, 0); setLight(aircon, 0);
  blades.rotation.z = 0;
  door.pivot.rotation.y = 0; windowSash.pivot.rotation.y = 0;
  dome.material.opacity = 0; dome.visible = false; cloud.visible = false; cloudTag.obj.visible = false;
  for (const n of notes) n.obj.visible = false;
}
function setLight(b, v) { b.mat.emissiveIntensity = 2.2 * v; b.halo.material.opacity = 0.9 * v; }

// state helpers used by the scenes
function packet(i, t, { from, to, t0, dur, text, kind = "data" }) {
  const p = packets[i];
  if (t < t0 || t > t0 + dur) return;
  const u = inOut(prog(t, t0, t0 + dur));
  const pos = pointOn(routeCurves(from, to), u);
  p.ball.visible = true;
  p.ball.position.copy(pos);
  const color = KIND_COLOR[kind];
  p.ball.material.color.setHex(color);
  p.halo.material.color.setHex(color);
  p.tag.obj.visible = !!text;
  p.tag.el.className = `packet ${kind === "data" ? "" : kind}`;
  p.tag.el.textContent = text || "";
  p.tag.el.style.opacity = Math.min(prog(t, t0, t0 + 0.2), 1 - prog(t, t0 + dur - 0.15, t0 + dur));
}
// route through the broker on the Pi4: from -> hub -> pi4, then pi4 -> hub -> to
function viaBroker(i, t, { from, to, t0, dur, text, kind }) {
  packet(i, t, { from, to: "pi4", t0, dur: dur / 2, text, kind });
  if (to !== "pi4") packet(i, t, { from: "pi4", to, t0: t0 + dur / 2, dur: dur / 2, text, kind });
}
function glow(key, v) { const d = devices[key]; d.glow.material.opacity = 0.75 * v; d.topMat.emissiveIntensity = 0.1 + 0.9 * v; }
function say(t, t0, t1, text, typed = false) {
  if (t < t0 || t > t1) return;
  bubble.obj.visible = true;
  const shown = typed ? text.slice(0, Math.max(1, Math.round(text.length * prog(t, t0, t0 + Math.min(1.6, (t1 - t0) * 0.6))))) : text;
  bubble.el.textContent = shown;
  bubble.el.style.opacity = win(t, t0, t1, 0.25);
}
function speakRings(t, t0, t1) {
  if (t < t0 || t > t1) return;
  rings.forEach((r, i) => {
    const phase = ((t - t0) * 0.9 + i / 3) % 1;
    r.scale.setScalar(0.2 + phase * 1.2);
    r.lookAt(camera.position);
    r.material.opacity = 0.8 * (1 - phase) * win(t, t0, t1, 0.3);
  });
}
const notes = [];
function note(i, t, t0, t1, text, at, cls = "note") {
  while (notes.length <= i) notes.push(label("", "note", v3(0, 0, 0), scene, [0.5, 1]));
  const n = notes[i];
  if (t < t0 || t > t1) return;
  n.obj.visible = true;
  n.obj.position.copy(at);
  n.el.className = cls;
  n.el.textContent = text;
  n.el.style.opacity = win(t, t0, t1, 0.25);
}
const above = (key, dy = 1.35) => devices[key].port.clone().add(v3(0, dy, 0));

// privacy dome and the cloud outside the house
const dome = new THREE.Mesh(new THREE.SphereGeometry(6.6, 48, 24, 0, Math.PI * 2, 0, Math.PI / 2),
  new THREE.MeshBasicMaterial({ color: COLOR.lan, transparent: true, opacity: 0, depthWrite: false, side: THREE.DoubleSide }));
dome.scale.set(1, 0.55, 0.78);
dome.position.set(0, BASE, 0.45);
scene.add(dome);
const CLOUD = v3(5.6, 2.6, -5.6);
const cloud = new THREE.Group();
for (const [x, y, r] of [[0, 0, 0.55], [0.55, -0.1, 0.42], [-0.55, -0.12, 0.4], [0.2, 0.3, 0.38]]) {
  const m = new THREE.Mesh(new THREE.SphereGeometry(r, 20, 14), new THREE.MeshStandardMaterial({ color: 0xc9d6da, roughness: 0.9 }));
  m.position.set(x, y, 0);
  cloud.add(m);
}
cloud.position.set(CLOUD.x, CLOUD.y, CLOUD.z);
scene.add(cloud);
const cloudTag = label("<b>외부 클라우드 서버</b>", "tag", v3(CLOUD.x, CLOUD.y - 0.75, CLOUD.z), scene, [0.5, 0]);

// ------------------------------------------------------------------ camera
const CAM = {
  wide: [[0, 7.6, 10.8], [0, 0.3, 1.25]],
  screen: [[3.2, 1.62, 0.15], [3.45, 1.3, -2.4]],
  phoneClose: [[3.3, 1.45, 4.5], [3.7, 0.8, 2.05]],
  esp8266Close: [[0.75, 3.0, 7.3], [0.75, 0.5, 3.05]],
  left: [[-2.6, 4.6, 6.4], [-2.2, 0.6, -0.6]],
  right: [[2.6, 4.8, 6.6], [2.4, 0.7, -0.6]],
  front: [[-0.8, 4.4, 8.6], [-0.9, 0.6, 1.8]],
  high: [[0, 11.5, 12.5], [0, 0.5, 0.2]],
  orbitStart: [[-9.5, 6.5, 8.5], [0, 0.4, 0.2]],
};
let shotNow = "";
const FOCUS = { left: ["pi502", "pi4"], screen: [], phoneClose: [], esp8266Close: ["esp8266"], front: ["pi502", "esp32", "esp8266"] };
function applyFocus() {
  const keep = FOCUS[shotNow];
  if (!keep) return;
  for (const [k, d] of Object.entries(devices)) if (!keep.includes(k)) d.tag.obj.visible = false;
  const close = ["screen", "phoneClose", "esp8266Close"].includes(shotNow);
  if (close) { routerTag.obj.visible = false; micTag.obj.visible = false; camRoom.tag.obj.visible = false; camEntry.tag.obj.visible = false; }
}
function cam(t, keys) {
  // keys: [[time, shot, seconds]]: arrive at `shot` at `time` after moving for `seconds` (default 2.4)
  let i = 0;
  while (i + 1 < keys.length && t >= keys[i + 1][0]) i++;
  const a = keys[i], b = keys[i + 1];
  const u = b ? inOut(prog(t, b[0] - (b[2] || 2.4), b[0])) : 0;
  shotNow = (u > 0.5 ? b : a)[1];
  const [pa, la] = CAM[a[1]], [pb, lb] = b ? CAM[b[1]] : CAM[a[1]];
  camera.position.copy(v3(...pa).lerp(v3(...pb), u));
  camera.lookAt(v3(...la).lerp(v3(...lb), u));
}

// ------------------------------------------------------------------ scenes
const SCENES = {};

SCENES.intro = { duration: 7, at(t) {
  const a = (t * 0.06) - 0.4;
  camera.position.set(Math.sin(a) * 12, 7.5, Math.cos(a) * 12);
  camera.lookAt(0, 0.4, 0.3);
  overlay({ card: ["HomeMind TaskFlow", "말이 끝나기 전에 준비하고, 상황을 보고 먼저 제안하는 집", "제24회 임베디드SW경진대회 · 스마트 가전 · PrivateFlowNest"], cardOpacity: Math.min(prog(t, 0, 0.8), 1 - 0.25 * prog(t, 6.2, 7)), kind: false, brand: false });
} };

SCENES.system = { duration: 20, at(t) {
  cam(t, [[0, "high"], [8.5, "wide", 3], [16, "wide"]]);
  const order = [["pi4", 1.0], ["pi502", 1.6], ["pi501", 2.2], ["router", 2.8], ["esp32", 3.4], ["esp8266", 4.0], ["phone", 4.6]];
  for (const [k, t0] of order) {
    const s = outBack(prog(t, t0, t0 + 0.6));
    for (const o of APPEAR[k]) { o.scale.setScalar(Math.max(0.001, s)); o.visible = t > t0 + 0.05; }
  }
  for (const m of shadows) m.material.opacity = prog(t, 4.6, 5.6);
  const linkOrder = ["pi4", "pi502", "pi501", "esp32", "esp8266", "phone"];
  linkOrder.forEach((k, i) => { const l = links[k]; const u = prog(t, 5.4 + i * 0.35, 6.4 + i * 0.35); l.tube.geometry.setDrawRange(0, Math.floor(l.count * u)); l.tube.visible = u > 0; });
  // status heartbeats every few seconds once connected
  for (let i = 0; i < 3; i++) {
    const t0 = 9.5 + i * 3.2;
    packet(0, t, { from: "esp32", to: "pi4", t0, dur: 1.6, text: i === 0 ? "상태 신호 · 5초마다" : "", kind: "ack" });
    packet(1, t, { from: "esp8266", to: "pi4", t0: t0 + 0.5, dur: 1.6, text: "", kind: "ack" });
    packet(2, t, { from: "pi502", to: "pi4", t0: t0 + 1.0, dur: 1.4, text: "", kind: "ack" });
  }
  note(0, t, 10.5, 19.5, "LG AI Native OS · C++ 서비스", above("pi4", 1.25));
  overlay({ chapter: "시스템 구성", ...captionAt(t, [
    [0.3, 8.4, "집 안 기기들이 공유기 하나로 연결됩니다", "Raspberry Pi 5 두 대 · Pi 4 · ESP32 · ESP8266 · 휴대폰"],
    [8.6, 14.0, "모든 메시지는 Pi 4의 MQTT 브로커를 거칩니다", "기기마다 계정과 권한(ACL)이 따로 있습니다"],
    [14.2, 19.8, "ESP 두 대는 명령을 실행하고 ACK로 완료를 알립니다", "5초마다 상태 신호를 보내 연결 상태를 알립니다"],
  ]) });
} };

SCENES.hidden = { duration: 26, at(t) {
  cam(t, [[0, "left"], [6.5, "wide", 2.5], [14.6, "right", 2.0], [18.4, "esp8266Close", 1.6], [22.0, "wide", 2.0]]);
  say(t, 0.4, 4.2, "“아 좀 덥네…”", true);
  speakRings(t, 0.6, 3.4);
  if (t > 1.2 && t < 5.0) glow("pi502", win(t, 1.2, 5.0));
  note(0, t, 2.6, 6.0, "Whisper: ‘아 좀 덥네’ → 규칙에 없음", above("pi502", 1.2));
  viaBroker(0, t, { from: "pi502", to: "pi501", t0: 4.6, dur: 2.6, text: "LLM 요청", kind: "data" });
  if (t > 7.0 && t < 10.6) glow("pi501", win(t, 7.0, 10.6) * (0.75 + 0.25 * Math.sin(t * 9)));
  note(1, t, 7.2, 10.8, "EXAONE: ‘에어컨 켜기’ 제안", above("pi501", 1.2), "note ai");
  packet(1, t, { from: "pi501", to: "pi4", t0: 10.4, dur: 1.4, text: "제안", kind: "ai" });
  if (t > 11.6 && t < 14.4) glow("pi4", win(t, 11.6, 14.4));
  note(2, t, 11.8, 14.6, "정책: LLM 제안은 항상 승인", above("pi4", 1.25));
  packet(2, t, { from: "pi4", to: "pi501", t0: 13.6, dur: 1.2, text: "승인 요청", kind: "data" });
  packet(3, t, { from: "pi4", to: "phone", t0: 13.6, dur: 1.3, text: "승인 요청", kind: "data" });
  note(3, t, 15.8, 17.6, "휴대폰에서 승인", devices.phone.port.clone().add(v3(0, 1.4, 0)), "note warn");
  packet(4, t, { from: "pi4", to: "esp8266", t0: 16.9, dur: 1.5, text: "명령 · 에어컨 표시등 켜기", kind: "cmd" });
  setLight(aircon, prog(t, 18.4, 18.8));
  packet(5, t, { from: "esp8266", to: "pi4", t0: 18.9, dur: 1.4, text: "ACK", kind: "ack" });
  packet(0, t, { from: "pi4", to: "pi501", t0: 20.3, dur: 1.0, text: "완료", kind: "ack" });
  overlay({ chapter: "숨은 의도 이해", ...captionAt(t, [
    [0.3, 4.4, "“아 좀 덥네…” 같은 혼잣말도 듣습니다", "음성 인식(Whisper)은 집 안 Pi 5에서"],
    [4.6, 10.8, "규칙에 없는 말은 EXAONE이 의도를 해석합니다", "LLM도 집 안 Pi 5에서 동작 · 결과는 ‘제안’일 뿐"],
    [11.0, 16.6, "Pi 4가 정책을 확인하고 사람에게 승인을 묻습니다", "7인치 화면이나 휴대폰에서 승인"],
    [16.8, 22.0, "승인되면 ESP8266이 실행하고 ACK로 알립니다", ""],
    [22.2, 25.8, "에어컨 표시등이 켜지고 화면에 완료가 표시됩니다", ""],
  ]) });
} };

SCENES["ui-hidden"] = { duration: 12, flat: { panel: "panel-hidden", phone: "phone-hidden", cut: [1.5, 2.95, "EXAONE 처리 중 · 실기 15~20초 · 중간 생략"] }, at(t) {
  overlay({ chapter: "숨은 의도 이해 · 화면", kindText: "실제 앱 화면 · PC에서 재현", low: true, ...captionAt(t, [
    [0.2, 2.8, "7인치 화면의 ‘말하기’를 누르고 “아 좀 덥네…”", "실제 7인치 UI와 휴대폰 앱 코드 화면입니다"],
    [2.9, 5.6, "EXAONE의 제안이 7인치 화면과 휴대폰에 함께 뜹니다", "고르기 전에는 아무것도 움직이지 않습니다"],
    [5.7, 8.6, "휴대폰에서 승인하면 실행합니다", "7인치 화면에서는 ‘실행’으로 승인할 수 있습니다"],
    [8.7, 11.8, "ESP8266의 ACK를 받으면 완료로 표시합니다", "휴대폰 장치 목록도 함께 바뀝니다"],
  ]) });
} };

SCENES.quiet = { duration: 24, at(t) {
  cam(t, [[0, "left"], [9.5, "wide", 2.5], [12.6, "esp8266Close", 1.8], [15.2, "right", 1.8], [17.8, "esp8266Close", 1.6], [22.0, "wide", 2.0]]);
  speakRings(t, 0.3, 10.5);
  camRoom.cone.material.opacity = 0.16 * win(t, 0.6, 9.5, 0.5);
  say(t, 0.5, 3.6, "(화상 회의 중)");
  note(0, t, 1.0, 9.0, "소리 상황: 대화 · 회의(YAMNet)", mic.position.clone().add(v3(0.5, 1.7, 0.3)), "note");
  note(1, t, 1.6, 9.0, "카메라: 책상 구역", v3(-4.75, BASE + 1.85, -0.35), "note");
  const n = t < 5.0 ? 0 : t < 6.0 ? 1 : t < 7.0 ? 2 : 3;
  note(2, t, 4.6, 9.4, `상황 융합 · 회의 ${n}/3`, above("pi502", 1.25), n === 3 ? "note warn" : "note");
  if (t > 4.6 && t < 9.4) glow("pi502", win(t, 4.6, 9.4));
  packet(0, t, { from: "pi502", to: "pi4", t0: 7.6, dur: 1.4, text: "상황 이벤트 · 회의", kind: "data" });
  if (t > 9.0 && t < 11.6) glow("pi4", win(t, 9.0, 11.6));
  note(3, t, 9.2, 11.8, "상황 규칙 → 조용 모드", above("pi4", 1.25));
  packet(1, t, { from: "pi4", to: "esp8266", t0: 10.6, dur: 1.3, text: "에어컨 저소음 표시", kind: "cmd" });
  packet(2, t, { from: "pi4", to: "pi501", t0: 10.6, dur: 1.1, text: "조용 모드", kind: "data" });
  if (t > 11.9 && t < 18.1) setLight(aircon, 0.25 + 0.75 * (0.5 + 0.5 * Math.cos((t - 11.9) * Math.PI * 1.2)));
  note(4, t, 15.0, 17.4, "‘상황이 달라요’ 누름", devices.pi501.port.clone().add(v3(0, 2.1, -1.0)), "note warn");
  packet(3, t, { from: "pi501", to: "pi4", t0: 15.9, dur: 1.1, text: "피드백 · 상황이 달라요", kind: "data" });
  packet(4, t, { from: "pi4", to: "esp8266", t0: 16.9, dur: 1.2, text: "해제", kind: "cmd" });
  overlay({ chapter: "상황 인지 · 조용 모드", ...captionAt(t, [
    [0.3, 4.5, "소리와 카메라로 지금 상황을 봅니다", "소리 상황 분류(YAMNet) + 카메라 위치 구역"],
    [4.7, 9.6, "같은 판단이 3번 이어지면 ‘회의 중’으로 확정합니다", "말 사이 짧은 침묵에는 해제하지 않습니다(15번 연속 필요)"],
    [9.8, 14.8, "Pi 4가 조용 모드를 켭니다", "에어컨 표시등이 저소음 표시로 깜빡이고 화면에 알립니다"],
    [15.0, 19.8, "틀렸다면 ‘상황이 달라요’ 한 번이면 바로 해제", "그 회의가 끝날 때까지 다시 켜지 않습니다"],
    [20.0, 23.8, "명령 없이도 상황에 맞춰 먼저 움직이는 집", ""],
  ]) });
} };

SCENES["ui-quiet"] = { duration: 11, flat: { panel: "panel-quiet" }, at(t) {
  overlay({ chapter: "상황 인지 · 화면", kindText: "실제 앱 화면 · PC에서 재현", low: true, ...captionAt(t, [
    [0.2, 2.9, "회의 소리를 듣고 ‘조용히 해야 하는 상황’으로 판단합니다", "소리 상황 · 카메라 위치 구역"],
    [3.0, 5.9, "조용 모드 켜짐 · 에어컨 표시등은 저소음 표시", "명령하지 않아도 상황 규칙이 먼저 움직입니다"],
    [6.0, 10.8, "틀렸다면 ‘상황이 달라요’ 한 번이면 해제", "이 회의가 끝날 때까지 다시 켜지 않습니다"],
  ]) });
} };

SCENES.doors = { duration: 23, at(t) {
  cam(t, [[0, "front"], [13.8, "right", 2.0], [18.6, "front", 1.6]]);
  say(t, 0.3, 3.0, "“창문 열어 줘”", true);
  speakRings(t, 0.4, 2.4);
  note(0, t, 2.0, 5.0, "규칙 파서 · LLM 생략", above("pi502", 1.2));
  packet(0, t, { from: "pi502", to: "pi4", t0: 2.6, dur: 1.3, text: "작업 요청 · 창문 열기", kind: "data" });
  if (t > 3.9 && t < 6.0) glow("pi4", win(t, 3.9, 6.0));
  packet(1, t, { from: "pi4", to: "esp32", t0: 4.9, dur: 1.4, text: "명령 · 창문 열기", kind: "cmd" });
  windowSash.pivot.rotation.y = 1.15 * inOut(prog(t, 6.3, 7.6));
  packet(2, t, { from: "esp32", to: "pi4", t0: 7.6, dur: 1.3, text: "ACK", kind: "ack" });
  say(t, 9.4, 12.2, "“현관문 열어 줘”", true);
  speakRings(t, 9.5, 11.4);
  packet(3, t, { from: "pi502", to: "pi4", t0: 11.6, dur: 1.2, text: "작업 요청 · 현관문 열기", kind: "data" });
  packet(4, t, { from: "pi4", to: "pi501", t0: 12.8, dur: 1.0, text: "승인 요청", kind: "data" });
  packet(5, t, { from: "pi4", to: "phone", t0: 12.8, dur: 1.1, text: "알림", kind: "data" });
  note(1, t, 14.0, 16.6, "휴대폰으로는 승인할 수 없음", devices.phone.port.clone().add(v3(0, 1.4, 0)), "note warn");
  note(2, t, 16.2, 17.9, "집 안 7인치 화면에서 승인", devices.pi501.port.clone().add(v3(0, 2.1, -1.0)), "note warn");
  packet(0, t, { from: "pi4", to: "esp32", t0: 17.4, dur: 1.4, text: "명령 · 현관문 열기", kind: "cmd" });
  door.pivot.rotation.y = 1.25 * inOut(prog(t, 18.8, 20.2));
  packet(1, t, { from: "esp32", to: "pi4", t0: 20.2, dur: 1.3, text: "ACK", kind: "ack" });
  overlay({ chapter: "바로 실행 · 안전한 승인", ...captionAt(t, [
    [0.2, 5.0, "분명한 명령은 규칙 파서가 바로 처리합니다", "LLM을 거치지 않아 빠르고 예측 가능합니다"],
    [5.2, 9.2, "ESP32가 창문 모터를 열고 ACK로 알립니다", "ACK가 없으면 3초마다 최대 3번 다시 보냅니다"],
    [9.4, 16.8, "현관문 열기는 위험한 동작이라 승인이 필요합니다", "집 안 7인치 화면에서만 승인 · 2분이 지나면 만료"],
    [17.0, 22.8, "승인한 뒤에야 현관문이 열립니다", "만료된 명령과 같은 명령의 반복은 ESP가 거절합니다"],
  ]) });
} };

SCENES["ui-doors"] = { duration: 15.5, flat: { panel: "panel-doors", phone: "phone-doors" }, at(t) {
  overlay({ chapter: "바로 실행 · 안전한 승인 · 화면", kindText: "실제 앱 화면 · PC에서 재현", low: true, ...captionAt(t, [
    [0.2, 4.6, "“창문 열어 줘” → 규칙 파서가 바로 실행", "ESP32의 ACK가 오면 완료로 표시합니다"],
    [4.7, 10.2, "“현관문 열어 줘”는 승인이 필요합니다", "휴대폰에서는 승인 버튼이 꺼져 있습니다"],
    [10.3, 15.3, "집 안 7인치 화면에서 ‘실행’을 눌러야 열립니다", "2분 안에 승인하지 않으면 만료됩니다"],
  ]) });
} };

SCENES.partial = { duration: 15, at(t) {
  cam(t, [[0, "wide"], [3.8, "right", 1.8], [7.0, "esp8266Close", 1.6], [10.8, "wide", 2.0]]);
  const text = t < 4.4 ? "“거실 불 켜…”" : "“거실 불 켜 줘”";
  say(t, 0.3, 6.4, text, t < 4.4);
  speakRings(t, 0.4, 5.2);
  packet(0, t, { from: "pi502", to: "pi4", t0: 1.4, dur: 1.0, text: "부분 발화 · 조명", kind: "data" });
  packet(0, t, { from: "pi4", to: "pi501", t0: 2.4, dur: 1.0, text: "준비 힌트 (실행 안 함)", kind: "data" });
  note(0, t, 3.6, 7.6, "말 중간에 대상 기기를 미리 표시", devices.pi501.port.clone().add(v3(0, 2.1, -1.0)));
  packet(1, t, { from: "pi502", to: "pi4", t0: 5.0, dur: 1.2, text: "작업 요청 · 거실 조명 켜기", kind: "data" });
  packet(2, t, { from: "pi4", to: "esp8266", t0: 6.2, dur: 1.3, text: "명령", kind: "cmd" });
  setLight(living, prog(t, 7.5, 7.9));
  packet(3, t, { from: "esp8266", to: "pi4", t0: 7.9, dur: 1.2, text: "ACK", kind: "ack" });
  overlay({ chapter: "말 중간 준비", ...captionAt(t, [
    [0.3, 4.6, "말이 끝나기 전에 무엇을 하려는지 알아챕니다", "부분 발화 분류 · 7인치 화면에 ‘거실 조명 준비’"],
    [4.8, 9.6, "말이 끝나면 그때 실행합니다", "부분 발화만으로는 절대 실행하지 않습니다"],
    [9.8, 14.8, "준비는 먼저, 실행은 확실할 때", ""],
  ]) });
} };

SCENES["ui-partial"] = { duration: 9, flat: { panel: "panel-partial" }, at(t) {
  overlay({ chapter: "말 중간 준비 · 화면", kindText: "실제 앱 화면 · PC에서 재현", low: true, ...captionAt(t, [
    [0.2, 3.9, "말하는 도중에 ‘조명 준비’를 미리 표시합니다", "부분 발화만으로는 실행하지 않습니다"],
    [4.0, 8.8, "말이 끝나면 실행 → 거실 조명 켜짐", ""],
  ]) });
} };

SCENES.privacy = { duration: 15, at(t) {
  cam(t, [[0, "wide"], [5, "orbitStart", 4.0], [12, "high", 3.0]]);
  dome.visible = true;
  dome.material.opacity = 0.12 * prog(t, 1.0, 3.0);
  cloud.visible = t < 10.5;
  cloud.scale.setScalar(Math.max(0.001, outBack(prog(t, 1.6, 2.4)) * (1 - prog(t, 9.6, 10.4))));
  cloudTag.obj.visible = t > 2.2 && t < 9.8;
  for (let i = 0; i < 3; i++) packet(i, t, { from: ["pi502", "esp8266", "esp32"][i], to: "pi4", t0: 1.0 + i * 0.9, dur: 1.5, text: "", kind: "data" });
  // a raw audio/video packet heads for the cloud and is stopped at the dome
  const p = packets[4];
  if (t > 4.5 && t < 9.5) {
    const u = inOut(prog(t, 4.5, 6.6));
    const from = devices.pi502.port.clone().add(v3(0, 0.3, 0));
    const stop = v3(1.92, 1.73, -3.88);  // where the line from the Pi 5 to the cloud meets the dome
    p.ball.visible = true;
    p.ball.position.copy(from.lerp(stop, u));
    const c = t < 6.6 ? KIND_COLOR.data : KIND_COLOR.block;
    p.ball.material.color.setHex(c);
    p.halo.material.color.setHex(c);
    p.tag.obj.visible = true;
    p.tag.el.className = t < 6.6 ? "packet" : "packet block";
    p.tag.el.textContent = t < 6.6 ? "원본 음성 · 영상" : "전송 안 함 ✕";
    p.tag.el.style.opacity = win(t, 4.5, 9.5, 0.3);
  }
  note(0, t, 7.2, 14.6, "STT · LLM · 영상 처리 모두 집 안", above("pi4", 1.25));
  overlay({ chapter: "Edge First · 사생활", ...captionAt(t, [
    [0.3, 4.4, "모든 AI는 집 안 Pi에서 동작합니다", "음성 인식 · EXAONE · 소리 상황 · 영상 위치"],
    [4.6, 9.6, "원본 음성과 영상은 집 밖으로 보내지 않고 저장하지 않습니다", "카메라 영상은 집 안 7인치 화면에서만 볼 수 있습니다"],
    [9.8, 14.8, "인터넷이 끊겨도 그대로 동작합니다", "클라우드 서버 없이"],
  ]) });
} };

SCENES.outro = { duration: 7, at(t) {
  const a = 0.6 + t * 0.05;
  camera.position.set(Math.sin(a) * 12.5, 8, Math.cos(a) * 12.5);
  camera.lookAt(0, 0.4, 0.3);
  setLight(living, 1); setLight(aircon, 1);
  overlay({ card: ["HomeMind TaskFlow", "먼저 알아채고, 안전하게 확인하는 온디바이스 AI 스마트홈", "PrivateFlowNest"], cardOpacity: prog(t, 0.2, 1.2), kind: false, brand: false });
} };

// ------------------------------------------------------------------ entry points for render.js
// Playback speed per scene, to fit the 3-minute limit together with the real footage: a scene's clock runs
// SPEED times faster than the video's (captions, camera and packets alike), so it lasts duration / SPEED.
const SPEED = {
  intro: 1.4, system: 1.43, hidden: 1.45, "ui-hidden": 1.15, quiet: 1.5, "ui-quiet": 1.15,
  doors: 1.5, "ui-doors": 1.15, partial: 1.5, "ui-partial": 1.15, privacy: 1.4, outro: 1.4,
};
const speedOf = (name) => SPEED[name] || 1;
window.SCENES_META = Object.fromEntries(Object.entries(SCENES).map(([k, s]) => [k, { duration: +(s.duration / speedOf(k)).toFixed(3) }]));
window.renderAt = async (name, videoT) => {
  const def = SCENES[name];
  const t = videoT * speedOf(name);
  $("flat").style.display = def.flat ? "block" : "none";
  $("stage").style.visibility = def.flat ? "hidden" : "visible";
  if (def.flat) {
    overlay({});
    await showFlat(def, t);
    def.at(t);
    labels.domElement.style.visibility = "hidden";
    // the caption sits under the 7-inch screen, clear of the phone
    const panel = flatLayout(def).panel;
    $("caption").style.left = `${panel[0] + panel[2] / 2}px`;
    $("caption").style.maxWidth = `${panel[2]}px`;
    return true;
  }
  reset();
  overlay({});
  await showScreens(name, t);
  def.at(t);
  applyFocus();
  blades.rotation.z = -t * 9;
  renderer.render(scene, camera);
  labels.render(scene, camera);
  return true;
};
document.fonts.load('700 32px "Noto Sans KR"').then(() => document.fonts.load('500 32px "Noto Sans KR"')).then(loadClips).then(() => {
  window.ready = true;
});
