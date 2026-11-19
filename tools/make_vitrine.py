# A page to look at the VR models outside the game: every vr_assets/glb/*.glb (tools/export_glb.py) in one file,
# vitrine.html beside them, opened straight in a browser (three.js from a CDN). Each gun's parts move as the game
# moves them: the slide or bolt on a shot, the magazine out and in, the pump racked, a shotgun's shell into its port;
# the game's gloved hands hold it, their fingers curled as the Touch controllers would.
#   python tools/make_vitrine.py <glb dir>
import base64
import glob
import json
import os
import sys

NAMES = {
    "g_pst": "Pistola", "g_gk1": "Glock", "g_mgn": "Revólver", "g_shg": "Escopeta (bomba)",
    "g_rm1": "Escopeta Remington", "g_snr": "Sniper", "g_smg": "Uzi", "g_sm3": "MAC", "g_p90": "P90",
    "g_mp1": "MP5", "g_asr": "Fuzil G36", "g_m16": "M16", "g_ak1": "AK-47",
}

PAGE = r"""<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Vitrine VR Black</title>
<style>
:root { --bg:#16181b; --panel:#1f2226; --line:#2e3238; --text:#e6e8eb; --dim:#9aa1aa; --accent:#e0403a; }
* { box-sizing: border-box; }
html, body { margin:0; height:100%; background:var(--bg); color:var(--text); font:14px/1.4 system-ui, sans-serif; }
#app { display:flex; height:100%; }
#side { width:300px; flex:none; background:var(--panel); border-right:1px solid var(--line); overflow:auto; padding:14px; }
#view { flex:1; position:relative; min-width:0; }
canvas { display:block; width:100%; height:100%; }
h1 { font-size:16px; margin:0 0 10px; }
h2 { font-size:12px; text-transform:uppercase; letter-spacing:.06em; color:var(--dim); margin:16px 0 6px; }
.list button { display:block; width:100%; text-align:left; margin:2px 0; padding:6px 8px; background:transparent;
  color:var(--text); border:1px solid transparent; border-radius:6px; cursor:pointer; font:inherit; }
.list button:hover { border-color:var(--line); }
.list button.on { background:#2a2e34; border-color:var(--accent); }
label { display:flex; align-items:center; justify-content:space-between; gap:8px; margin:6px 0; color:var(--text); }
label span { flex:none; width:120px; }
input[type=range] { flex:1; min-width:0; accent-color:var(--accent); }
.row { display:flex; flex-wrap:wrap; gap:6px; margin:6px 0; }
.row button { flex:1 1 auto; padding:7px 8px; background:#2a2e34; color:var(--text); border:1px solid var(--line);
  border-radius:6px; cursor:pointer; font:inherit; }
.row button:hover, .row button.on { border-color:var(--accent); }
.row button:disabled { opacity:.35; cursor:default; }
#info { color:var(--dim); font-size:12px; }
#hint { position:absolute; left:12px; bottom:10px; color:var(--dim); font-size:12px; pointer-events:none; }
@media (max-width: 700px) {
  #app { flex-direction:column; }
  #side { width:100%; height:46%; border-right:0; border-bottom:1px solid var(--line); }
}
</style>
<script type="importmap">
{ "imports": { "three": "https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.module.js",
               "three/addons/": "https://cdn.jsdelivr.net/npm/three@0.160.0/examples/jsm/" } }
</script>
</head>
<body>
<div id="app">
  <div id="side">
    <h1>Vitrine VR — Black</h1>
    <div class="list" id="list"></div>
    <h2>Arma</h2>
    <div id="info"></div>
    <label><span>Ferrolho / bomba</span><input id="slide" type="range" min="0" max="1" step="0.01" value="0"></label>
    <label><span>Pente para fora</span><input id="mag" type="range" min="0" max="1" step="0.01" value="0"></label>
    <div class="row">
      <button id="fire">Atirar</button><button id="eject">Ejetar pente</button><button id="insert">Inserir pente</button>
    </div>
    <div class="row"><button id="pump">Bombear</button><button id="shell">Carregar cartucho</button></div>
    <div id="origbox">
    <h2>Original do jogo</h2>
    <div id="originfo"></div>
    <label><span>Mostrar original</span><input id="orig" type="checkbox" checked></label>
    <label><span>Sobrepor ao modelo VR</span><input id="over" type="checkbox"></label>
    <div class="row" id="clips"></div>
    <label><span>Velocidade</span><input id="speed" type="range" min="0.1" max="1.5" step="0.05" value="1"></label>
    </div>
    <h2>Mãos</h2>
    <label><span>Mostrar mãos</span><input id="hands" type="checkbox" checked></label>
    <label><span>Duas mãos</span><input id="two" type="checkbox"></label>
    <label><span>Gatilho (indicador)</span><input id="trigger" type="range" min="0" max="1" step="0.01" value="0"></label>
    <label><span>Grip esquerdo</span><input id="grip" type="range" min="0" max="1" step="0.01" value="0.3"></label>
    <label><span>Indicador esquerdo</span><input id="lindex" type="range" min="0" max="1" step="0.01" value="0.2"></label>
    <label><span>Polegar no botão</span><input id="thumb" type="checkbox" checked></label>
    <h2>Vista</h2>
    <div class="row"><button id="side_v">Lado</button><button id="top_v">Cima</button><button id="eye_v">Primeira pessoa</button></div>
  </div>
  <div id="view"><div id="hint">Arraste para girar · roda para zoom · botão direito para mover</div></div>
</div>
<script type="module">
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";

const MODELS = __MODELS__;
const NAMES = __NAMES__;

const view = document.getElementById("view");
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.7;
view.appendChild(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x8c9298);
const camera = new THREE.PerspectiveCamera(40, 1, 0.01, 50);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
scene.add(new THREE.HemisphereLight(0xdfe7f0, 0x30302c, 1.4));
const sun = new THREE.DirectionalLight(0xffffff, 2.2); sun.position.set(0.6, 1.2, 0.8); scene.add(sun);
const fill = new THREE.DirectionalLight(0xffffff, 0.8); fill.position.set(-1, 0.3, -0.6); scene.add(fill);
const grid = new THREE.GridHelper(2, 20, 0x6c7278, 0x7c8288); grid.position.y = -0.25; scene.add(grid);

function resize() { const w = view.clientWidth, h = view.clientHeight; renderer.setSize(w, h); camera.aspect = w / h; camera.updateProjectionMatrix(); }
addEventListener("resize", resize); resize();

const loader = new GLTFLoader();
function load(b64) {
  const bin = Uint8Array.from(atob(b64), c => c.charCodeAt(0));
  return new Promise((ok, no) => loader.parse(bin.buffer, "", ok, no));
}

// the gun shown and the hands
let gun = null, gunParts = {}, gunInfo = {}, hands = null, bones = {}, restQ = {};
const flash = new THREE.Mesh(new THREE.SphereGeometry(0.035, 12, 8), new THREE.MeshBasicMaterial({ color: 0xffd27a }));
flash.visible = false; scene.add(flash);
const state = { slide: 0, mag: 0, shellT: -1, anim: null };
// the game's own animations of the gun shown, read from its file (orig_<gun>, tools/game_anim.py)
const ORIG_UP = 0.8;  // metres the game's original sits above the VR model
let orig = null, mixer = null, origClips = [], origAction = null, origPalm = [0, 0, 0];
const clock = new THREE.Clock();
const clipLabel = n => n.charAt(0).toUpperCase() + n.slice(1);
async function showOrig(name) {
  if (orig) { scene.remove(orig); orig = null; mixer = null; }
  const key = "orig_" + name.slice(2);
  const box = document.getElementById("origbox"), clipsEl = document.getElementById("clips");
  clipsEl.innerHTML = "";
  if (!MODELS[key]) {
    document.getElementById("originfo").innerHTML = "Sem animações do jogo para esta arma.";
    return;
  }
  const g = await load(MODELS[key]);
  orig = g.scene; scene.add(orig);
  orig.traverse(o => { if (o.isMesh) o.frustumCulled = false; if (o.userData && o.userData.palm) origPalm = o.userData.palm; });
  mixer = new THREE.AnimationMixer(orig);
  origClips = g.animations;
  document.getElementById("originfo").innerHTML = `${origClips.length} animações do jogo, lidas do arquivo da arma:`;
  for (const c of origClips) {
    const b = document.createElement("button"); b.textContent = clipLabel(c.name); b.dataset.clip = c.name;
    b.onclick = () => playOrig(c.name); clipsEl.appendChild(b);
  }
  playOrig("parado");
}
function playOrig(name) {
  if (!mixer) return;
  const c = origClips.find(x => x.name === name); if (!c) return;
  if (origAction) origAction.stop();
  origAction = mixer.clipAction(c); origAction.reset().play();
  for (const b of document.querySelectorAll("#clips button")) b.classList.toggle("on", b.dataset.clip === name);
  // the VR model does the same, to compare
  if (name.startsWith("tiro")) document.getElementById("fire").onclick();
  if (name.startsWith("recarga") && gunParts.magazine) play([[500, t => set("mag", t * t)], [700, t => {}], [600, t => set("mag", 1 - t)]]);
}
function placeOrig() {
  if (!orig) return;
  orig.visible = document.getElementById("orig").checked;
  const up = document.getElementById("over").checked ? 0 : ORIG_UP;  // above the VR model, seen from the side
  orig.position.set(-origPalm[0], -origPalm[1] + up, -origPalm[2]);
}

const handsGlb = await load(MODELS["g_hands"]);
hands = handsGlb.scene; scene.add(hands);
hands.traverse(o => { if (o.isBone) { bones[o.name] = o; restQ[o.name] = o.quaternion.clone(); } if (o.isMesh) o.frustumCulled = false; });

async function show(name) {
  if (gun) scene.remove(gun);
  const g = await load(MODELS[name]);
  gun = g.scene; scene.add(gun);
  gunParts = {}; gun.traverse(o => { if (o.name) gunParts[o.name] = o; });
  for (const k of Object.keys(gunParts)) gunParts[k].userData.rest = gunParts[k].position.clone();
  gunInfo = (gun.children[0] && gun.children[0].userData) || {};
  if (!Object.keys(gunInfo).length) gun.traverse(o => { if (o.userData && o.userData.slide_travel !== undefined) gunInfo = o.userData; });
  state.slide = 0; state.mag = 0; state.shellT = -1;
  document.getElementById("slide").value = 0; document.getElementById("mag").value = 0;
  document.getElementById("two").checked = !!gunInfo.two_handed;
  const parts = Object.keys(gunParts).filter(k => ["body", "magazine", "slide", "shell"].includes(k));
  document.getElementById("info").innerHTML =
    `<b>${NAMES[name] || name}</b><br>Peças: ${parts.join(", ")}<br>` +
    `Curso do ferrolho: ${((gunInfo.slide_travel || 0) * 100).toFixed(1)} cm<br>` +
    `${gunInfo.pump ? "Bomba (pump action)" : gunInfo.two_handed ? "Duas mãos" : "Uma mão"}`;
  document.getElementById("eject").disabled = !gunParts.magazine;
  document.getElementById("insert").disabled = !gunParts.magazine;
  document.getElementById("mag").disabled = !gunParts.magazine;
  document.getElementById("pump").disabled = !gunInfo.pump;
  document.getElementById("shell").disabled = !gunParts.shell;
  for (const b of document.querySelectorAll(".list button")) b.classList.toggle("on", b.dataset.name === name);
  await showOrig(name);
  frameView("side");
}

// the gun's space: barrel -z, top +y, origin at the right palm. Back along the barrel is +z
function poseGun(now) {
  if (!gun) return;
  const travel = gunInfo.slide_travel || 0;
  if (gunParts.slide) gunParts.slide.position.copy(gunParts.slide.userData.rest).add(new THREE.Vector3(0, 0, travel * state.slide));
  if (gunParts.magazine) {
    const m = gunParts.magazine, k = state.mag;
    m.position.copy(m.userData.rest).add(new THREE.Vector3(0, -0.28 * k, 0.04 * k));
    m.rotation.x = -0.4 * k; m.visible = k < 0.999;
  }
  if (gunParts.shell) {
    const s = gunParts.shell; s.visible = state.shellT >= 0;
    if (state.shellT >= 0) s.position.copy(s.userData.rest).add(new THREE.Vector3(-0.05, -0.12, 0.05).multiplyScalar(1 - state.shellT));
  }
}

const AX = new THREE.Vector3(1, 0, 0);
function curl(prefix, angle) {
  for (const n in bones) if (n.startsWith(prefix)) {
    const seg = Math.min(+n.slice(-1) || 0, 2), k = [0.8, 1, 0.8][seg];
    bones[n].quaternion.copy(restQ[n]).multiply(new THREE.Quaternion().setFromAxisAngle(AX, angle * k));
  }
}
function poseHands() {
  hands.visible = document.getElementById("hands").checked;
  const trig = +document.getElementById("trigger").value, grip = +document.getElementById("grip").value;
  const li = +document.getElementById("lindex").value, th = document.getElementById("thumb").checked;
  curl("R_index", trig * 0.35);
  curl("L_index", -0.2 + 1.25 * li);
  curl("L_finger", -0.35 + 1.5 * grip);
  curl("L_thumb", th ? 0.35 : 0);
  // the right hand where the game holds this gun (its "hand" matrix, tools/game_anim.py)
  const R = bones["R_root"];
  if (R) {
    if (gunInfo.hand) new THREE.Matrix4().fromArray(gunInfo.hand).decompose(R.position, R.quaternion, R.scale);
    else { R.position.set(0, 0, 0); R.quaternion.identity(); }
  }
  // the left hand: on the fore end with both hands (where the pump is), turned as the game's; else beside
  const L = bones["L_root"];
  if (L) {
    if (document.getElementById("two").checked && gunInfo.fore) {
      const f = gunInfo.fore;
      L.position.set(f[0], f[1], f[2] + (gunInfo.pump ? (gunInfo.slide_travel || 0) * state.slide : 0));
      if (gunInfo.fore_rot) L.quaternion.setFromRotationMatrix(new THREE.Matrix4().fromArray(gunInfo.fore_rot));
    } else { L.position.set(-0.28, -0.08, -0.05); L.quaternion.identity(); }
  }
}

function muzzle() {
  if (!gunParts.body) return new THREE.Vector3(0, 0.05, -0.3);
  const box = new THREE.Box3().setFromObject(gunParts.body); return new THREE.Vector3(0, (box.min.y + box.max.y) / 2 + 0.03, box.min.z);
}
// animations: a list of (duration, function of 0..1)
function play(steps) { state.anim = { steps, i: 0, t0: performance.now() }; }
function tickAnim(now) {
  const a = state.anim; if (!a) return;
  let s = a.steps[a.i], t = (now - a.t0) / s[0];
  if (t >= 1) { s[1](1); a.i++; a.t0 = now; if (a.i >= a.steps.length) state.anim = null; return; }
  s[1](t);
}
const set = (k, v) => { state[k] = v; const el = document.getElementById(k); if (el) el.value = v; };
document.getElementById("fire").onclick = () => {
  if (state.mag > 0.5 && gunParts.magazine) return;
  flash.position.copy(muzzle()); flash.visible = true;
  document.getElementById("trigger").value = 1;
  if (gunInfo.pump) play([[60, t => {}], [60, t => { flash.visible = false; document.getElementById("trigger").value = 0; }]]);
  else play([[30, t => set("slide", t)], [80, t => { set("slide", 1 - t); flash.visible = false; }],
             [60, t => { document.getElementById("trigger").value = 1 - t; }]]);
};
document.getElementById("eject").onclick = () => play([[500, t => set("mag", t * t)]]);
document.getElementById("insert").onclick = () => play([[600, t => set("mag", 1 - t)]]);
document.getElementById("pump").onclick = () => play([[220, t => set("slide", t)], [220, t => set("slide", 1 - t)]]);
document.getElementById("shell").onclick = () => play([[600, t => { state.shellT = t; }], [150, t => { state.shellT = t < 1 ? 1 : -1; }]]);
document.getElementById("slide").oninput = e => state.slide = +e.target.value;
document.getElementById("mag").oninput = e => state.mag = +e.target.value;

// the camera fitted to the gun (and the hands round it): from its side, from above, or from behind as the player
let lastView = "side";
function frameView(which) {
  lastView = which;
  const box = new THREE.Box3();
  if (gunParts.body) box.expandByObject(gunParts.body);
  if (orig && orig.visible && !document.getElementById("over").checked) box.expandByPoint(new THREE.Vector3(0, ORIG_UP + 0.2, box.min.z)), box.expandByPoint(new THREE.Vector3(0, ORIG_UP + 0.2, box.max.z));
  if (!gunParts.body) box.set(new THREE.Vector3(-0.1, -0.1, -0.4), new THREE.Vector3(0.1, 0.1, 0.1));
  const c = box.getCenter(new THREE.Vector3()), size = box.getSize(new THREE.Vector3());
  const span = Math.max(size.z, size.y * 1.6, 0.25) * 1.25;
  const d = span / (2 * Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2)) / Math.min(1, camera.aspect) + 0.05;
  controls.target.copy(c);
  if (which === "side") camera.position.set(c.x + d, c.y + d * 0.12, c.z);
  else if (which === "top") camera.position.set(c.x + 0.001, c.y + d, c.z);
  else { camera.position.set(0.04, 0.17, 0.32); controls.target.set(0, 0.04, box.min.z); }
  controls.update();
}
addEventListener("resize", () => frameView(lastView));
document.getElementById("side_v").onclick = () => frameView("side");
document.getElementById("top_v").onclick = () => frameView("top");
document.getElementById("eye_v").onclick = () => frameView("eye");

const list = document.getElementById("list");
for (const name of Object.keys(MODELS)) {
  if (name === "g_hands" || name.startsWith("orig_")) continue;
  const b = document.createElement("button"); b.textContent = NAMES[name] || name; b.dataset.name = name;
  b.onclick = () => show(name); list.appendChild(b);
}
await show("g_ak1" in MODELS ? "g_ak1" : Object.keys(MODELS).find(n => n !== "g_hands"));
document.getElementById("orig").onchange = () => frameView(lastView);
document.getElementById("over").onchange = () => frameView(lastView);
renderer.setAnimationLoop(now => {
  const dt = clock.getDelta();
  if (mixer) mixer.update(dt * +document.getElementById("speed").value);
  tickAnim(now); poseGun(now); poseHands(); placeOrig(); controls.update(); renderer.render(scene, camera);
});
</script>
</body>
</html>
"""

if __name__ == "__main__":
    d = sys.argv[1]
    order = ["g_pst", "g_gk1", "g_mgn", "g_smg", "g_sm3", "g_mp1", "g_p90", "g_asr", "g_m16", "g_ak1", "g_snr",
             "g_shg", "g_rm1", "g_hands"]
    order += sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(d, "orig_*.glb")))
    models = {}
    for n in order:
        p = os.path.join(d, n + ".glb")
        if os.path.exists(p):
            models[n] = base64.b64encode(open(p, "rb").read()).decode()
    html = PAGE.replace("__MODELS__", json.dumps(models)).replace("__NAMES__", json.dumps(NAMES, ensure_ascii=False))
    out = os.path.join(d, "vitrine.html")
    open(out, "w", encoding="utf-8").write(html)
    print(out, os.path.getsize(out) // 1024, "KB", list(models))
