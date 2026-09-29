// ROI 短边收边 (P5.9) 离线自检 —— main.cpp 里 roi_trim_short_axis 的 1:1 复刻。
//
// 验的是什么: 拟合出来的框在'短边方向'常常比车牌高出一截(现场剖面: 最上段 1/9、最下段 3/9 是
// 车牌色, 中间 5~8/9; 框的长短边比中位 2.61 vs 真车牌 ~3.14), 于是字符被竖直压扁。
// 收边的任务就是: 多吃的时候能收回来, 本来就准的时候一点都不许收。
//
// 做法: 合成一帧(蓝底车牌 + 白笔画 + 车牌自己的白边), 把框上下各多留 N px, 看收边后的长短边比
// 是否回到真值; 另外用一个'框正好贴着蓝底'的 case 确认它不会被误收。
// 不依赖摄像头/板子/第三方库 (本机 node v20 即可):
//     node PlateRecognitionIDF\tools\roi_trim_selftest.js

'use strict';

const W = 640, H = 480;
const frame = Buffer.alloc(W * H * 2);
const BLUE = [17, 90, 168];    // 近似样张 粤T666FP.jpg 的蓝底 (B=167 G=89 R=14)
const DARK = [60, 60, 60];     // 车身/背景

function put(x, y, r, g, b) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  const i = (y * W + x) * 2;
  const v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  frame[i] = (v >> 8) & 0xFF; frame[i + 1] = v & 0xFF;
}
function px(x, y) {   // 与 main.cpp sample_rgb565_bilinear 的解码宏逐位一致(大端)
  const i = (y * W + x) * 2, hi = frame[i], lo = frame[i + 1];
  return { r: hi & 0xF8, g: (((hi & 0x07) << 5) | ((lo & 0xE0) >> 3)) & 0xFF, b: (lo & 0x1F) << 3 };
}
function bilinear(fx, fy) {
  if (fx < 0) fx = 0; if (fx > W - 1) fx = W - 1;
  if (fy < 0) fy = 0; if (fy > H - 1) fy = H - 1;
  const x0 = Math.floor(fx), y0 = Math.floor(fy);
  const x1 = (x0 + 1 < W) ? x0 + 1 : x0, y1 = (y0 + 1 < H) ? y0 + 1 : y0;
  const ax = fx - x0, ay = fy - y0;
  const xs = [x0, x1, x0, x1], ys = [y0, y0, y1, y1];
  const wt = [(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay];
  let ab = 0, ag = 0, ar = 0;
  for (let k = 0; k < 4; k++) { const c = px(xs[k], ys[k]); ab += wt[k] * c.b; ag += wt[k] * c.g; ar += wt[k] * c.r; }
  const cl = v => Math.max(0, Math.min(255, Math.floor(v + 0.5)));
  return { b: cl(ab), g: cl(ag), r: cl(ar) };
}
// 与 main.cpp roi_is_plate_color 一致 (H 200~248 度, S>=43, V>=46)
function isPlateColor(r, g, b) {
  const v = Math.max(r, g, b), m = Math.min(r, g, b);
  if (v < 46) return false;
  const d = v - m;
  if (d === 0) return false;
  if (Math.floor(255 * d / v) < 43) return false;
  let deg;
  if (v === r) { deg = Math.floor(60 * (g - b) / d); if (deg < 0) deg += 360; }
  else if (v === g) { deg = Math.floor(60 * (b - r) / d) + 120; }
  else { deg = Math.floor(60 * (r - g) / d) + 240; }
  return (deg >= 200 && deg <= 248) || (deg >= 70 && deg <= 170);
}
function quadEval(qx, qy, u, v) {
  const w0 = (1 - u) * (1 - v), w1 = u * (1 - v), w2 = u * v, w3 = (1 - u) * v;
  return { x: w0 * qx[0] + w1 * qx[1] + w2 * qx[2] + w3 * qx[3],
           y: w0 * qy[0] + w1 * qy[1] + w2 * qy[2] + w3 * qy[3] };
}
const ROI_VTRIM_N = 24, ROI_VTRIM_UN = 9, ROI_VTRIM_MAX = 0.25, ROI_VTRIM_MIN_KEEP = 0.60;

// ===== main.cpp roi_trim_short_axis 的复刻 =====
function trimShortAxis(qx, qy) {
  const N = ROI_VTRIM_N, UN = ROI_VTRIM_UN, cov = [];
  for (let j = 0; j < N; j++) {
    const v = (j + 0.5) / N;
    let hit = 0;
    for (let i = 0; i < UN; i++) {
      const u = (i + 1) / (UN + 1);
      const p = quadEval(qx, qy, u, v);
      const x = Math.round(p.x), y = Math.round(p.y);
      if (x < 0 || y < 0 || x >= W || y >= H) continue;
      const c = bilinear(p.x, p.y);
      if (isPlateColor(c.r, c.g, c.b)) hit++;
    }
    cov.push(hit);
  }
  let v_lo = 0, v_hi = 1;
  const need = 4;
  let jt = -1, jb = -1;
  for (let j = 0; j + 2 < N; j++) if (cov[j] >= need && cov[j + 1] >= need && cov[j + 2] >= need) { jt = j; break; }
  for (let j = N - 1; j - 2 >= 0; j--) if (cov[j] >= need && cov[j - 1] >= need && cov[j - 2] >= need) { jb = j; break; }
  if (jt < 0 || jb < 0) return { v_lo, v_hi, cov, why: 'no-boundary' };
  let lo = jt / N, hi = (jb + 1) / N;
  if (lo < 0) lo = 0; if (hi > 1) hi = 1;
  if (lo > ROI_VTRIM_MAX) lo = 0;
  if (1 - hi > ROI_VTRIM_MAX) hi = 1;
  if (hi - lo < ROI_VTRIM_MIN_KEEP) return { v_lo: 0, v_hi: 1, cov, why: 'too-little-left' };
  return { v_lo: lo, v_hi: hi, cov, why: '' };
}

// ===== 合成帧 =====
function buildScene(framePx) {
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) put(x, y, DARK[0], DARK[1], DARK[2]);
  const px1 = 140, py1 = 190, px2 = 510, py2 = 335;
  for (let y = py1; y < py2; y++) for (let x = px1; x < px2; x++) put(x, y, 240, 240, 240);   // 车牌白边
  const bx1 = px1 + framePx, by1 = py1 + framePx, bx2 = px2 - framePx, by2 = py2 - framePx;
  for (let y = by1; y < by2; y++) for (let x = bx1; x < bx2; x++) put(x, y, BLUE[0], BLUE[1], BLUE[2]);
  const cw = (bx2 - bx1) / 7;
  for (let c = 0; c < 7; c++) {
    const cx = bx1 + c * cw + cw / 2;
    for (const off of [-0.22, 0, 0.22]) {
      const sx = Math.round(cx + off * cw);
      for (let y = by1 + 16; y < by2 - 16; y++) for (let d = 0; d < 4; d++) put(sx + d, y, 245, 245, 245);
    }
  }
  return { bx1, by1, bx2, by2 };
}

let pass = 0, fail = 0;
const b = buildScene(6);
const bw = b.bx2 - b.bx1, bh = b.by2 - b.by1;
const trueRatio = bw / bh;
const qx = [b.bx1, b.bx2, b.bx2, b.bx1];

function check(name, extraPx, expectTrim, ratioTol) {
  const qy = [b.by1 - extraPx, b.by1 - extraPx, b.by2 + extraPx, b.by2 + extraPx];
  const boxH = bh + 2 * extraPx;
  const r = trimShortAxis(qx, qy);
  const kept = r.v_hi - r.v_lo;
  const before = bw / boxH, after = bw / (boxH * kept);
  const trimmed = (r.v_lo > 0 || r.v_hi < 1);
  let ok, note;
  if (!expectTrim) {
    ok = !trimmed;
    note = ok ? '未收(正确)' : '不该收却收了';
  } else {
    ok = trimmed && Math.abs(after - trueRatio) <= ratioTol;
    note = '收边 [' + (r.v_lo * 100).toFixed(1) + '%, ' + (r.v_hi * 100).toFixed(1) + '%]';
  }
  if (ok) pass++; else fail++;
  console.log((ok ? '  OK  ' : '  FAIL') + ' ' + name +
    ' 框高 ' + boxH + ' 比例 ' + before.toFixed(2) + ' -> ' + after.toFixed(2) +
    ' (真值 ' + trueRatio.toFixed(2) + ') ' + note + (r.why ? ' [' + r.why + ']' : ''));
}

console.log('蓝底 ' + bw + 'x' + bh + ' 长短边比 ' + trueRatio.toFixed(2));
check('A 框=蓝底本身 (本来就准)      ', 0, false, 0);
check('B 上下各多吃 8% (对齐现场中位)', Math.round(bh * 0.08), true, 0.15);
check('C 上下各多吃 15%              ', Math.round(bh * 0.15), true, 0.15);
check('D 上下各多吃 25%              ', Math.round(bh * 0.25), true, 0.15);
check('E 上下各多吃 40% (过头, 宁可不收)', Math.round(bh * 0.40), false, 0);
console.log('=======================================================================');
console.log('RESULT: pass=' + pass + ' fail=' + fail);
process.exitCode = fail === 0 ? 0 : 1;
