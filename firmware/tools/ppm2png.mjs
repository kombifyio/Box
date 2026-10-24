#!/usr/bin/env node
// Converts the UI preview frames (tests/ui_preview, P6 PPM) into PNGs and a
// contact sheet per locale: node firmware/tools/ppm2png.mjs <dir>
import { readFileSync, writeFileSync, readdirSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { join } from "node:path";

const dir = process.argv[2] || ".";

function readPpm(path) {
  const b = readFileSync(path);
  const head = b.toString("latin1", 0, 64).split(/\s+/);
  const w = +head[1], h = +head[2];
  let off = 0, fields = 0;
  while (fields < 4) { while (/\s/.test(String.fromCharCode(b[off]))) off++; while (!/\s/.test(String.fromCharCode(b[off]))) off++; fields++; }
  return { w, h, px: b.subarray(off + 1) };
}

const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = (buf) => { let c = 0xffffffff; for (const v of buf) c = crcTable[(c ^ v) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
  return Buffer.concat([len, td, c]);
}
function png(w, h, rgb) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}

const files = readdirSync(dir).filter((f) => f.endsWith(".ppm")).sort();
const groups = {};
for (const f of files) {
  const img = readPpm(join(dir, f));
  writeFileSync(join(dir, f.replace(/\.ppm$/, ".png")), png(img.w, img.h, img.px));
  (groups[f.split("-R-")[0].split("-X-")[0]] ||= []).push(img);
}
for (const [loc, imgs] of Object.entries(groups)) {
  const cols = 4, pad = 12, cw = 360, rows = Math.ceil(imgs.length / cols);
  const W = cols * (cw + pad) + pad, H = rows * (cw + pad) + pad;
  const sheet = Buffer.alloc(W * H * 3, 0x18);
  imgs.forEach((img, i) => {
    const ox = pad + (i % cols) * (cw + pad), oy = pad + Math.floor(i / cols) * (cw + pad);
    for (let y = 0; y < img.h; y++) img.px.copy(sheet, ((oy + y) * W + ox) * 3, y * img.w * 3, (y + 1) * img.w * 3);
  });
  writeFileSync(join(dir, `sheet-${loc}.png`), png(W, H, sheet));
}
console.log(`converted ${files.length} frames, sheets: ${Object.keys(groups).join(", ")}`);
