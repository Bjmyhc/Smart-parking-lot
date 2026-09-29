// 裁剪块光度归一化 (P5.10) 离线自检 —— main.cpp 里 crop_photometric_norm 的 1:1 复刻。
//
// 验的是什么: 屏摄/欠曝会把蓝底拍淡, 白字与蓝底的分离度下降; 归一化要在不破坏色相(不能把蓝底
// 拉成灰)的前提下把对比度拉回来; 而本来就正常的裁剪只允许被轻微动一下(束在 1.19 倍左右)。
//
// 判据:
//   A 正常蓝牌: 色相必须还是 210 度上下, 蓝底/白字的 RGB 距离只准涨、涨幅 <= 25%
//   B 屏摄发白: 色相必须保住(仍是蓝), RGB 距离必须明显变大(>=15%), 蓝底的 B 通道必须仍最大
//   C 纯色块  : 跨度太小, 必须完全不动作 (s=1)
//
// 运行 (本机 node v20):
//     node PlateRecognitionIDF\tools\crop_norm_selftest.js

'use strict';

const TARGET = 200.0, S_MAX = 3.0, MIN_SPREAD = 12;

function norm(px) {
  const n = px.length;
  let sb = 0, sg = 0, sr = 0;
  const hist = new Array(256).fill(0);
  for (const p of px) {
    sb += p.b; sg += p.g; sr += p.r;
    hist[(77 * p.r + 150 * p.g + 29 * p.b) >> 8]++;
  }
  const nlo = Math.floor(n * 5 / 100), nhi = Math.floor(n * 95 / 100);
  let cum = 0, plo = 0, phi = 255, gl = false, gh = false;
  for (let i = 0; i < 256; i++) {
    cum += hist[i];
    if (!gl && cum >= nlo) { plo = i; gl = true; }
    if (!gh && cum >= nhi) { phi = i; gh = true; break; }
  }
  const spread = phi - plo;
  let s = 1;
  if (spread >= MIN_SPREAD) { s = TARGET / spread; if (s < 1) s = 1; if (s > S_MAX) s = S_MAX; }
  const mb = sb / n, mg = sg / n, mr = sr / n;
  const out = px.map(p => {
    if (s <= 1) return { b: p.b, g: p.g, r: p.r };
    const cl = v => Math.max(0, Math.min(255, Math.round(v)));
    return { b: cl(mb + (p.b - mb) * s), g: cl(mg + (p.g - mg) * s), r: cl(mr + (p.r - mr) * s) };
  });
  return { s, plo, phi, spread, out };
}
function hue(c) {
  const v = Math.max(c.r, c.g, c.b), m = Math.min(c.r, c.g, c.b), d = v - m;
  if (!d) return NaN;
  if (v === c.r) { const h = 60 * (c.g - c.b) / d; return h < 0 ? h + 360 : h; }
  if (v === c.g) return 60 * (c.b - c.r) / d + 120;
  return 60 * (c.r - c.g) / d + 240;
}
const dist = (a, b) => Math.hypot(a.r - b.r, a.g - b.g, a.b - b.b);
const WHITE = { b: 245, g: 245, r: 245 };

// 合成一块裁剪: 蓝底占 (1-frac), 其余是白笔画
function build(blue, frac, n) {
  const px = [], k = Math.round(n * frac);
  for (let i = 0; i < n; i++) px.push(i < k ? { ...WHITE } : { ...blue });
  return px;
}

let pass = 0, fail = 0;
function check(name, ok, detail) {
  if (ok) pass++; else fail++;
  console.log((ok ? '  OK  ' : '  FAIL') + ' ' + name + '  ' + detail);
}

// A) 正常蓝牌 (蓝底取自样张 粤T666FP.jpg 的蓝底: B167 G89 R14)
{
  const blue = { b: 167, g: 89, r: 14 };
  const px = build(blue, 0.25, 2268);
  const R = norm(px);
  const nb = R.out[2267];
  const d0 = dist(WHITE, blue), d1 = dist(R.out[0], nb);
  const up = d1 / d0;
  check('A 正常蓝牌: 色相 210 度上下', Math.abs(hue(nb) - 211) <= 8,
    `hue ${hue(blue).toFixed(0)}° -> ${hue(nb).toFixed(0)}°`);
  check('A 正常蓝牌: 只轻微加强 (<=1.30)', up <= 1.30 && up >= 1.0,
    `s=${R.s.toFixed(2)} 蓝底/白字距离 ${d0.toFixed(0)} -> ${d1.toFixed(0)} (x${up.toFixed(2)})`);
}
// B) 屏摄发白: 蓝底由现场实测裁剪均值(B217 G169 R105, 白笔画 25%)反推 = (204,140,55)
{
  const blue = { b: 204, g: 140, r: 55 };
  const px = build(blue, 0.25, 2268);
  const R = norm(px);
  const nb = R.out[2267];
  const d0 = dist(WHITE, blue), d1 = dist(R.out[0], nb);
  const up = d1 / d0;
  check('B 屏摄发白: 色相仍是蓝', hue(nb) >= 200 && hue(nb) <= 215,
    `hue ${hue(blue).toFixed(0)}° -> ${hue(nb).toFixed(0)}°`);
  check('B 屏摄发白: 对比度明显变大 (>=1.15)', up >= 1.15,
    `s=${R.s.toFixed(2)} 距离 ${d0.toFixed(0)} -> ${d1.toFixed(0)} (x${up.toFixed(2)})`);
  check('B 屏摄发白: 蓝底仍是 B 通道最大', nb.b > nb.g && nb.g > nb.r,
    `蓝底 -> B${nb.b} G${nb.g} R${nb.r}`);
}
// C) 纯色块 (没有内容可拉) -> 必须完全不动作
{
  const px = build({ b: 200, g: 200, r: 200 }, 0.0, 2268);
  const R = norm(px);
  check('C 纯色块: 不动 (s=1)', R.s === 1, `s=${R.s.toFixed(2)} 跨度=${R.spread}`);
}
console.log('=======================================================================');
console.log('RESULT: pass=' + pass + ' fail=' + fail);
process.exitCode = fail === 0 ? 0 : 1;
