// Shared drawing helpers for the EdgeOS technical figures (edgeos-ai-*.html and edgeos-runlevels.html).
// A plain technical style in the manner of vendor documentation: white page, flat panels with thin borders,
// straight connectors with filled arrowheads, IBM Plex Sans for text and STIX Two for equations, and a small
// palette with one colour per role. Dashed outlines mark parts that were tested and not shipped.
// The function names match the earlier hand-drawn helpers, so each figure keeps its layout code.
// Render with docs/images/render.sh.
const C = {
  page: '#FFFFFF', card: '#FFFFFF', panel: '#F6F8FB', ink: '#15202D', soft: '#57616F', line: '#C3CCD8',
  blue: '#2F5FD0', purple: '#24467F', orange: '#A8530A', green: '#1E7A4C', black: '#15202D',
  red: '#B42318', amber: '#A8530A', brown: '#8A4B08', rule: '#DCE2EA'
};
const HAND = "'IBM Plex Sans', system-ui, sans-serif";
const TITLE = "'IBM Plex Sans', system-ui, sans-serif";
const MATH = "'STIX Two Text', 'Times New Roman', serif";
const SCALE = 0.86;                     // Plex is wider than the old hand font, so body text is set a little smaller
const NS = 'http://www.w3.org/2000/svg';
const svg = document.getElementById('fig');

let seed = 1;                          // kept so figures that set it still run; nothing is random any more
const rnd = () => 0.5;
const jit = () => 0;

function el(name, attrs, parent = svg) {
  const e = document.createElementNS(NS, name);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  parent.appendChild(e);
  return e;
}

// A panel: flat fill, one thin border. Colour is used for the border only when the panel is a group of its own.
function roughRect(x, y, w, h, r, color, o = {}) {
  el('rect', {
    x, y, width: w, height: h, rx: Math.min(r, 10),
    fill: o.fill || 'none', stroke: color, 'stroke-width': Math.min(o.sw || 1.5, 1.5),
    ...(o.dash ? { 'stroke-dasharray': '6 4' } : {})
  });
}

// A straight connector through waypoints, with a filled arrowhead at the end unless arrow is false.
function roughLine(way, color, o = {}) {
  const sw = Math.min(o.sw || 1.6, 2);
  const d = 'M' + way.map(([a, b]) => `${a.toFixed(1)},${b.toFixed(1)}`).join(' L');
  el('path', { d, fill: 'none', stroke: color, 'stroke-width': sw, 'stroke-linejoin': 'miter', 'stroke-linecap': 'butt',
    ...(o.dash ? { 'stroke-dasharray': o.dash.includes(' 8') ? '2 4' : '6 4' } : {}) });
  if (o.arrow !== false) {
    const [tx, ty] = way[way.length - 1], [px, py] = way[way.length - 2];
    const a = Math.atan2(ty - py, tx - px), L = 10, Wd = 4.2;
    const bx = tx - Math.cos(a) * L, by = ty - Math.sin(a) * L;
    el('path', { d: `M${tx},${ty} L${bx - Math.sin(a) * Wd},${by + Math.cos(a) * Wd} L${bx + Math.sin(a) * Wd},${by - Math.cos(a) * Wd} Z`,
      fill: color });
  }
}

function text(x, y, s, o = {}) {
  const t = el('text', { x, y, 'font-family': o.font || HAND, 'font-size': (o.size || 20) * SCALE, fill: o.color || C.ink,
    'font-weight': o.weight ? Math.min(o.weight, 600) : 400, 'text-anchor': o.anchor || 'start' }, o.parent || svg);
  t.textContent = s;
  return t;
}

// One line made of runs with their own style: runs = [[string, {color, weight, font, size}], ...].
function runs(x, y, parts, o = {}) {
  const t = el('text', { x, y, 'font-family': o.font || HAND, 'font-size': (o.size || 20) * SCALE, fill: o.color || C.ink,
    'text-anchor': o.anchor || 'start' });
  for (const [s, p = {}] of parts) {
    const ts = el('tspan', {}, t);
    if (p.color) ts.setAttribute('fill', p.color);
    if (p.weight) ts.setAttribute('font-weight', Math.min(p.weight, 600));
    if (p.font) ts.setAttribute('font-family', p.font);
    if (p.size) ts.setAttribute('font-size', p.size * SCALE);
    if (p.dy) ts.setAttribute('dy', p.dy * SCALE);
    ts.textContent = s;
  }
  return t;
}

// An equation line, set in STIX Two. '^{..}' is a superscript and '_{..}' a subscript.
function eq(x, y, s, o = {}) {
  const size = (o.size || 21) * 1.08, parts = [], re = /([\^_])\{([^}]*)\}/g;
  let last = 0, m, shift = 0;
  while ((m = re.exec(s))) {
    if (m.index > last) { parts.push([s.slice(last, m.index), { dy: -shift }]); shift = 0; }
    const d = m[1] === '^' ? -size * 0.38 : size * 0.22;
    parts.push([m[2], { size: size * 0.68, dy: d - shift }]); shift = d;
    last = re.lastIndex;
  }
  if (last < s.length) parts.push([s.slice(last), { dy: -shift }]);
  return runs(x, y, parts.map(([a, p]) => [a, p.dy ? p : { size: p.size }]),
    { size, color: o.color, anchor: o.anchor, font: MATH });
}

// Word-wrap s into lines no wider than maxW. Returns the baseline after the last line.
function wrap(x, y, s, maxW, lh, o = {}) {
  const words = s.replace(/ (%|‰|ms|µs|s|h|B|KB|mA|mAh|dBm|°C)\b/g, ' $1').replace(/ (%)/g, ' $1').split(' ');
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

// A step number: a filled disc with the number in white.
function numCircle(cx, cy, n, color) {
  el('circle', { cx, cy, r: 15, fill: color });
  const t = text(cx, cy + 6, String(n), { weight: 600, size: 18, color: '#FFFFFF', anchor: 'middle' });
  return t;
}

// A numbered panel with a title and a lane line. Returns the baseline of the lane line.
function card(n, x, y, w, h, color, title, lane, o = {}) {
  roughRect(x, y, w, h, 10, o.dash ? C.soft : C.line, { fill: C.panel, dash: o.dash });
  if (n) numCircle(x + 6, y + 6, n, o.dash ? C.soft : color);
  const ty = wrap(x + 36, y + 40, title, w - 56, 28, { font: TITLE, weight: 600, size: o.tsize ? o.tsize * 0.92 : 23, color: C.ink });
  if (!lane) return ty;
  text(x + 36, ty + 24, lane, { size: 18, color: C.soft });
  return ty + 24;
}

function pill(x, y, label, color, o = {}) {
  const t = text(0, 0, label, { size: o.size || 19, color });
  const w = t.getComputedTextLength() + 26;
  t.setAttribute('x', x + 13); t.setAttribute('y', y + 21);
  roughRect(x, y, w, 30, 6, color, { fill: o.fill || '#FFFFFF', dash: o.dash });
  t.parentNode.appendChild(t);           // keep the label above the fill
  return x + w;
}

// Status labels. The words match the status table of the architecture page.
const STATUS = {
  board:    { label: 'on the board',          color: C.green, fill: '#E7F3EC' },
  mac:      { label: 'validated on the Mac',  color: C.blue,  fill: '#EAF0FC' },
  training: { label: 'in training',           color: C.brown, fill: '#F8EEDF' },
  notship:  { label: 'tested, not shipped',   color: C.soft,  fill: '#F1F3F6', dash: true },
  pending:  { label: 'PENDING',               color: C.red,   fill: '#FBECEA', dash: true },
};
// Draw a status label at (x, y); anchor 'end' puts its right edge at x. Returns [left, right].
function chip(x, y, kind, o = {}) {
  const s = STATUS[kind], label = o.label || s.label, size = o.size || 18;
  const probe = text(0, 0, label, { size, weight: 600 });
  const w = probe.getComputedTextLength() + 38;
  probe.remove();
  const x0 = o.anchor === 'end' ? x - w : x;
  roughRect(x0, y, w, 28, 6, s.color, { fill: s.fill, dash: s.dash });
  el('rect', { x: x0 + 12, y: y + 10, width: 8, height: 8, rx: 1.5, fill: s.color });
  text(x0 + 28, y + 19.5, label, { size, color: s.color, weight: 600 });
  return [x0, x0 + w];
}

// Kept for figures that still call it: draws nothing.
function star() {}

// The figure title and its one-paragraph summary. Returns the summary's last baseline.
function heading(W, title, intro) {
  el('rect', { width: W, height: svg.getAttribute('height'), fill: C.page });
  text(60, 72, title, { weight: 600, size: 38, color: C.ink });
  el('rect', { x: 60, y: 90, width: W - 120, height: 1, fill: C.rule });
  return wrap(60, 134, intro, W - 120, 27, { size: 21, color: C.soft });
}

function bullet(x, y, s, maxW, color, o = {}) {
  el('rect', { x: x + 2, y: y - 7 * SCALE, width: 5, height: 5, fill: color });
  return wrap(x + 18, y, s, maxW - 18, o.lh || 24, { size: o.size || 19, color: o.color });
}

function dottedRule(y, W) {
  el('rect', { x: 40, y, width: W - 80, height: 1, fill: C.rule });
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
  const want = ["400 18px 'IBM Plex Sans'", "600 22px 'IBM Plex Sans'", "400 22px 'STIX Two Text'"];
  Promise.all(want.map(f => document.fonts.load(f))).then(() => {
    draw();
    finish(W, H);
    document.body.dataset.fonts = want.map(f => document.fonts.check(f) ? 'ok' : 'MISSING').join(',');
  });
}
