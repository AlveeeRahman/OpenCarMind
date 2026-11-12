// Shared hand-drawn helpers for the EdgeOS AI-service figures (edgeos-ai-*.html).
// The same strokes, fonts and colours as edgeos-runlevels.html. Each figure sets SEED before it draws,
// so every render is the same. Render with docs/images/render.sh.
const C = {
  page: '#FBF4E4', card: '#FFFCF4', ink: '#2B2620', soft: '#6B6158',
  orange: '#D9780F', blue: '#2F5FD0', purple: '#7A48B5', green: '#2E8A4E', black: '#2B2620',
  red: '#C8372D', amber: '#F59A2E', brown: '#B8650C', rule: '#CDBFA5'
};
const HAND = "'Patrick Hand', 'Comic Sans MS', 'Marker Felt', cursive";
const TITLE = "Kalam, 'Comic Sans MS', 'Marker Felt', cursive";
const NS = 'http://www.w3.org/2000/svg';
const svg = document.getElementById('fig');

let seed = 20261112;
function rnd() {                       // mulberry32: the same strokes on every render
  seed |= 0; seed = (seed + 0x6D2B79F5) | 0;
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const jit = a => (rnd() * 2 - 1) * a;

function el(name, attrs, parent = svg) {
  const e = document.createElementNS(NS, name);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  parent.appendChild(e);
  return e;
}

// A smooth path through points (Catmull-Rom turned into cubic Beziers).
function smooth(p, closed) {
  const n = p.length, g = i => p[closed ? (i + n) % n : Math.max(0, Math.min(n - 1, i))];
  let d = `M${p[0][0].toFixed(1)},${p[0][1].toFixed(1)}`;
  const last = closed ? n : n - 1;
  for (let i = 0; i < last; i++) {
    const a = g(i - 1), b = g(i), c = g(i + 1), e = g(i + 2);
    d += ` C${(b[0] + (c[0] - a[0]) / 6).toFixed(1)},${(b[1] + (c[1] - a[1]) / 6).toFixed(1)}` +
         ` ${(c[0] - (e[0] - b[0]) / 6).toFixed(1)},${(c[1] - (e[1] - b[1]) / 6).toFixed(1)} ${c[0].toFixed(1)},${c[1].toFixed(1)}`;
  }
  return d;
}

// Points around a rounded rectangle, nudged by a slow wobble so the edge looks drawn by hand.
function roundRectPts(x, y, w, h, r, amp) {
  const pts = [], step = 16;
  const ph1 = rnd() * 6.28, ph2 = rnd() * 6.28;
  const side = (x0, y0, x1, y1) => {
    const L = Math.hypot(x1 - x0, y1 - y0), n = Math.max(2, Math.round(L / step));
    const nx = -(y1 - y0) / L, ny = (x1 - x0) / L;
    for (let i = 0; i < n; i++) {
      const t = i / n, s = pts.length;
      const o = amp * (0.7 * Math.sin(s * 0.35 + ph1) + 0.3 * Math.sin(s * 1.1 + ph2));
      pts.push([x0 + (x1 - x0) * t + nx * o, y0 + (y1 - y0) * t + ny * o]);
    }
  };
  const arc = (cx, cy, a0) => {
    for (let i = 0; i < 3; i++) {
      const a = a0 + (i / 3) * Math.PI / 2;
      pts.push([cx + r * Math.cos(a) + jit(amp * 0.4), cy + r * Math.sin(a) + jit(amp * 0.4)]);
    }
  };
  side(x + r, y, x + w - r, y);          arc(x + w - r, y + r, -Math.PI / 2);
  side(x + w, y + r, x + w, y + h - r);  arc(x + w - r, y + h - r, 0);
  side(x + w - r, y + h, x + r, y + h);  arc(x + r, y + h - r, Math.PI / 2);
  side(x, y + h - r, x, y + r);          arc(x + r, y + r, Math.PI);
  return pts;
}

function roughRect(x, y, w, h, r, color, o = {}) {
  const sw = o.sw || 2.4, passes = o.dash ? 1 : 2;
  if (o.fill) el('rect', { x, y, width: w, height: h, rx: r, fill: o.fill });
  for (let k = 0; k < passes; k++) {
    const pts = roundRectPts(x + jit(1), y + jit(1), w + jit(1.5), h + jit(1.5), r, o.amp ?? 1.3);
    const extra = Math.round(pts.length * 0.04);   // overshoot the start a little, like a pen closing a loop
    const open = pts.concat(pts.slice(0, extra + 1)).map(([a, b], i) => i > pts.length ? [a + jit(1.2), b + jit(1.2)] : [a, b]);
    el('path', { d: smooth(open, false), fill: 'none', stroke: color, 'stroke-width': k ? sw * 0.7 : sw,
      'stroke-linecap': 'round', 'stroke-linejoin': 'round', opacity: k ? 0.55 : 1,
      ...(o.dash ? { 'stroke-dasharray': o.dash } : {}) });
  }
}

// A hand-drawn stroke through waypoints, with an optional open arrowhead at the end.
function roughLine(way, color, o = {}) {
  const pts = [];
  for (let s = 0; s < way.length - 1; s++) {
    const [x0, y0] = way[s], [x1, y1] = way[s + 1];
    const L = Math.hypot(x1 - x0, y1 - y0), n = Math.max(3, Math.round(L / 18));
    const bow = (o.bow ?? 0.012) * L * (rnd() < 0.5 ? -1 : 1), nx = -(y1 - y0) / L, ny = (x1 - x0) / L;
    for (let i = s ? 1 : 0; i <= n; i++) {
      const t = i / n, b = bow * Math.sin(Math.PI * t), e = (i === 0 || i === n) ? 0 : 0.6;
      pts.push([x0 + (x1 - x0) * t + nx * b + jit(e), y0 + (y1 - y0) * t + ny * b + jit(e)]);
    }
  }
  const sw = o.sw || 2.4;
  el('path', { d: smooth(pts, false), fill: 'none', stroke: color, 'stroke-width': sw, 'stroke-linecap': 'round',
    ...(o.dash ? { 'stroke-dasharray': o.dash } : {}) });
  if (o.arrow !== false) {
    const [tx, ty] = pts[pts.length - 1], [px, py] = pts[pts.length - 3];
    const a = Math.atan2(ty - py, tx - px), L = 14;
    for (const s of [-1, 1]) {
      const b = a + Math.PI + s * (0.48 + jit(0.06));
      el('path', { d: `M${tx},${ty} Q${tx + Math.cos(b) * L * 0.5 + jit(1)},${ty + Math.sin(b) * L * 0.5 + jit(1)} ${tx + Math.cos(b) * L},${ty + Math.sin(b) * L}`,
        fill: 'none', stroke: color, 'stroke-width': sw, 'stroke-linecap': 'round' });
    }
  }
}

function text(x, y, s, o = {}) {
  const t = el('text', { x, y, 'font-family': o.font || HAND, 'font-size': o.size || 20, fill: o.color || C.ink,
    'font-weight': o.weight || 400, 'text-anchor': o.anchor || 'start' }, o.parent || svg);
  t.textContent = s;
  return t;
}

// One line made of runs with their own style: runs = [[string, {color, weight, font, size}], ...].
function runs(x, y, parts, o = {}) {
  const t = el('text', { x, y, 'font-family': o.font || HAND, 'font-size': o.size || 20, fill: o.color || C.ink,
    'text-anchor': o.anchor || 'start' });
  for (const [s, p = {}] of parts) {
    const ts = el('tspan', {}, t);
    if (p.color) ts.setAttribute('fill', p.color);
    if (p.weight) ts.setAttribute('font-weight', p.weight);
    if (p.font) ts.setAttribute('font-family', p.font);
    if (p.size) ts.setAttribute('font-size', p.size);
    if (p.dy) ts.setAttribute('dy', p.dy);
    ts.textContent = s;
  }
  return t;
}

// An equation line. '^{..}' is a superscript and '_{..}' a subscript, drawn as shifted runs.
function eq(x, y, s, o = {}) {
  const size = o.size || 21, parts = [], re = /([\^_])\{([^}]*)\}/g;
  let last = 0, m, shift = 0;
  while ((m = re.exec(s))) {
    if (m.index > last) { parts.push([s.slice(last, m.index), { dy: -shift }]); shift = 0; }
    const d = m[1] === '^' ? -size * 0.38 : size * 0.22;
    parts.push([m[2], { size: size * 0.68, dy: d - shift }]); shift = d;
    last = re.lastIndex;
  }
  if (last < s.length) parts.push([s.slice(last), { dy: -shift }]);
  return runs(x, y, parts.map(([a, p]) => [a, p.dy ? p : { size: p.size }]), { size, color: o.color, anchor: o.anchor });
}

// Word-wrap s into lines no wider than maxW. Returns the baseline after the last line.
function wrap(x, y, s, maxW, lh, o = {}) {
  const words = s.replace(/ (%|‰|ms|µs|s|h|B|KB|mA|mAh|dBm|°C)\b/g, '\u00a0$1').replace(/ (%)/g, '\u00a0$1').split(' ');
  let line = '', t = text(x, y, '', o);
  for (const w of words) {
    const tryLine = line ? line + ' ' + w : w;
    t.textContent = tryLine;
    if (t.getComputedTextLength() > maxW && line) {
      t.textContent = line; y += lh; line = w; t = text(x, y, w, o);
    } else line = tryLine;
  }
  t.textContent = line;
  return y;
}

function numCircle(cx, cy, n, color) {
  el('circle', { cx, cy, r: 20, fill: C.page });
  const pts = [];
  for (let i = 0; i < 22; i++) {
    const a = i / 20 * Math.PI * 2 - 1.2, r = 20 + jit(0.9);
    pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]);
  }
  el('path', { d: smooth(pts, false), fill: 'none', stroke: color, 'stroke-width': 2.6, 'stroke-linecap': 'round' });
  text(cx, cy + 8, String(n), { font: TITLE, weight: 700, size: 22, color, anchor: 'middle' });
}

// A numbered card with a title and a lane line. Returns the baseline of the lane line.
function card(n, x, y, w, h, color, title, lane, o = {}) {
  roughRect(x, y, w, h, 18, color, { fill: o.fill || C.card, sw: 2.6, dash: o.dash });
  if (n) numCircle(x + 6, y + 6, n, color);
  const ty = wrap(x + 36, y + 40, title, w - 56, 30, { font: TITLE, weight: 700, size: o.tsize || 25, color });
  if (!lane) return ty;
  text(x + 36, ty + 24, lane, { size: 18, color, weight: 400 }).setAttribute('opacity', '0.8');
  return ty + 24;
}

function pill(x, y, label, color, o = {}) {
  const t = text(0, 0, label, { size: o.size || 19, color });
  const w = t.getComputedTextLength() + 30;
  t.setAttribute('x', x + 15); t.setAttribute('y', y + 22);
  roughRect(x, y, w, 32, 16, color, { sw: 2, amp: 0.8, fill: o.fill, dash: o.dash });
  t.parentNode.appendChild(t);           // keep the label above the fill
  return x + w;
}

// Status chips. The words match the status table of the architecture page.
const STATUS = {
  board:    { label: 'on the board',          color: C.green, fill: '#E5F2E8' },
  mac:      { label: 'validated on the Mac',  color: C.blue,  fill: '#E8EEFD' },
  training: { label: 'in training',           color: C.brown, fill: '#FBEBD3' },
  notship:  { label: 'tested, not shipped',   color: C.soft,  fill: '#EFE8DA', dash: '6 5' },
  pending:  { label: 'PENDING',               color: C.red,   fill: '#FBE7E3', dash: '6 5' },
};
// Draw a status chip at (x, y); anchor 'end' puts its right edge at x. Returns [left, right].
function chip(x, y, kind, o = {}) {
  const s = STATUS[kind], label = o.label || s.label, size = o.size || 18;
  const probe = text(0, 0, label, { size, weight: 700 });
  const w = probe.getComputedTextLength() + 44;
  probe.remove();
  const x0 = o.anchor === 'end' ? x - w : x;
  roughRect(x0, y, w, 30, 15, s.color, { sw: 2, amp: 0.7, fill: s.fill, dash: s.dash });
  el('circle', { cx: x0 + 17, cy: y + 15, r: 5.5, fill: s.color });
  text(x0 + 30, y + 21, label, { size, color: s.color, weight: 700 });
  return [x0, x0 + w];
}

function star(cx, cy, R, color) {
  const pts = [];
  for (let i = 0; i < 10; i++) {
    const a = -Math.PI / 2 + i * Math.PI / 5, r = (i % 2 ? R * 0.45 : R) + jit(0.8);
    pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]);
  }
  const d = 'M' + pts.map(p => p.map(v => v.toFixed(1)).join(',')).join(' L') + ' Z';
  el('path', { d, fill: color, stroke: C.brown, 'stroke-width': 2, 'stroke-linejoin': 'round' });
}

// The title with its star and a double hand-drawn underline, then the intro. Returns the intro's last baseline.
function heading(W, title, intro) {
  el('rect', { width: W, height: svg.getAttribute('height'), fill: C.page });
  star(64, 62, 19, C.amber);
  const tt = text(96, 76, title, { font: TITLE, weight: 700, size: 44 });
  const tw = tt.getComputedTextLength();
  roughLine([[94, 92], [96 + tw * 0.5, 96], [100 + tw, 91]], C.amber, { arrow: false, sw: 3.2, bow: 0.004 });
  roughLine([[110, 99], [96 + tw * 0.6, 102], [90 + tw, 98]], C.amber, { arrow: false, sw: 2, bow: 0.004 });
  return wrap(60, 140, intro, W - 120, 28, { size: 22, color: C.soft });
}

function bullet(x, y, s, maxW, color, o = {}) {
  text(x, y, '-', { size: o.size || 19, color });
  return wrap(x + 18, y, s, maxW - 18, o.lh || 24, { size: o.size || 19, color: o.color });
}

function dottedRule(y, W) {
  roughLine([[40, y], [W - 40, y]], C.rule, { arrow: false, sw: 1.6, bow: 0.002, dash: '3 8' });
}

// Report text that runs past the page, and whether the fonts loaded, for the render check.
function finish(W, H) {
  const over = [];
  for (const t of svg.querySelectorAll('text')) {
    if (t.getAttribute('transform')) continue;   // rotated labels: the box is not in page space
    const b = t.getBBox();
    if (b.x < 4 || b.x + b.width > W - 8 || b.y + b.height > H) over.push(t.textContent);
  }
  document.body.dataset.overflow = over.join(' | ');
}

function start(draw, W, H) {
  const want = ["700 25px Kalam", "400 20px 'Patrick Hand'"];
  Promise.all(want.map(f => document.fonts.load(f))).then(() => {
    draw();
    finish(W, H);
    document.body.dataset.fonts = want.map(f => document.fonts.check(f) ? 'ok' : 'MISSING').join(',');
  });
}
