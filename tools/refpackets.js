// Generates reference packets by executing DX Light's OWN webpack modules
// (60895 = RB protocol, 72299 = sequence id, 79539.lowLight, 42127 colour helpers)
// extracted verbatim from app.asar. Usage: node refpackets.js <index.js> <RRGGBB> <lampCount> [brightnessPercent]
const fs = require("fs");
const [bundle, hex, lamps] = process.argv.slice(2);
const src = fs.readFileSync(bundle, "utf8");
function moduleFn(id) {
  const m = new RegExp("[,{]" + id + ":([(]e(,t)?(,r)?[)]|e)=>").exec(src);
  if (!m) throw new Error("module " + id + " not found");
  // walk braces to find end of the arrow function body
  let i = src.indexOf("{", m.index + m[0].length - 1), depth = 0, q = null;
  for (let j = i; j < src.length; j++) {
    const c = src[j];
    if (q) { if (c === "\\") { j++; continue; } if (c === q) q = null; continue; }
    if (c === '"' || c === "'" || c === "`") { q = c; continue; }
    if (c === "{") depth++; else if (c === "}" && --depth === 0) return eval("(" + src.slice(m.index + 1 + id.length + 1, j + 1) + ")");
  }
}
const cache = {};
const fakeStore = { store: { get: () => undefined } };   // defaults => isLightCompression on, syncSaturation off
function req(id) {
  id = String(id);
  if (id === "81687") return fakeStore;
  if (cache[id]) return cache[id].exports;
  const mod = cache[id] = { exports: {} };
  const r = (x) => req(x);
  r.r = (e) => {}; r.d = (e, defs) => { for (const k in defs) Object.defineProperty(e, k, { get: defs[k], enumerable: true }); };
  moduleFn(id)(mod, mod.exports, r);
  return mod.exports;
}
const Proto = req("60895"), { lowLight } = req("79539"), { hexToRgb, ajustColorOrder } = req("42127");
const p = new Proto();
// Same pipeline as the app's resume handler (module 96203 getUsbProtocol), device id "000603", wireMap RGB
const rgb = ajustColorOrder(hexToRgb("#" + hex), "RGB");
const o = lowLight("000603", [rgb.r, rgb.g, rgb.b]);
const l = Number(lamps);
const frame = l > 0 && l < 254 ? [1, o[0], o[1], o[2], l, l + 1, 0, 0, 0, 254] : [1, o[0], o[1], o[2], 254];
const hx = (b) => Buffer.from(b).toString("hex").match(/../g).join(" ");
console.log("lowLight(" + hex + ") ->", o.join(","));
console.log("setSectionLED :", hx(p.setSectionLED(frame)));
console.log("turnOffLight  :", hx(p.turnOffLight()));
console.log("getDeviceInfo :", hx(p.getDeviceInfo()));
// Brightness exactly as the renderer computes it: Math.floor(255 * a), minimum 5.
const a = process.argv[5] !== undefined ? Number(process.argv[5]) / 100 : 1;
const bv = Math.floor(255 * a), bval = bv >= 5 ? bv : 5;
console.log("setBrightness :", hx(p.setBrightness(bval)));
