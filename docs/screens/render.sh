#!/bin/sh
# Renders every screen in screens.html to docs/images/<id>.png (dark) and <id>-light.png at the
# panel's 2x size (344x640). The light variant applies the firmware's light palette by remapping
# every dark-theme colour, the same thing main/display.c does at 06:00.
# Needs Google Chrome; run from the repository root:  sh docs/screens/render.sh
set -e
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
[ -x "$CHROME" ] || CHROME="$(command -v google-chrome || command -v chromium || true)"
[ -n "$CHROME" ] || { echo "Chrome not found"; exit 1; }
TMP="$(mktemp -d)"
python3 - "$TMP" <<'PY'
import re, sys, os
tmp = sys.argv[1]
src = open('docs/screens/screens.html').read()
head = src[:src.index('<body>') + 6].replace('.row{display:flex;gap:24px;padding:24px}', '.row{display:block;padding:0}').replace('body{margin:0;background:#1a1f27', 'body{margin:0;background:#0B0F14')
# dark -> light palette, mirrors THEME_DARK / THEME_LIGHT in main/display.c
MAP = {'#0B0F14':'#F5F3EE', '#151B23':'#FFFFFF', '#E5E7EB':'#15181E', '#7B8794':'#5B6470', '#22D3EE':'#0E7490',
       '#22C55E':'#15803D', '#F59E0B':'#B45309', '#EF4444':'#DC2626', '#3A4552':'#D9D3C9', '#3B4552':'#A8B0BA', '#fff':'#15181E', '#FFFFFF':'#15181E'}
def lighten(html):
    out = html
    for d, l in MAP.items():
        out = re.sub(re.escape(d), l + '\u0000', out, flags=re.I)      # mark replaced values so later rules skip them
    out = out.replace('\u0000', '')
    # tiles need an outline on a white page
    out = out.replace('.tile{background:#FFFFFF;', '.tile{background:#FFFFFF;border:1px solid #D9D3C9;')
    out = out.replace('.t3{background:#FFFFFF;', '.t3{background:#FFFFFF;border:1px solid #D9D3C9;')
    out = out.replace('.bar{height:44px;background:#FFFFFF;', '.bar{height:44px;background:#FFFFFF;border-bottom:1px solid #D9D3C9;')
    out = out.replace('.card{margin:0 12px;background:#FFFFFF;', '.card{margin:0 12px;background:#FFFFFF;')
    return out
for m in re.finditer(r'(<div class="scr[^"]*" id="([^"]+)"[^>]*>.*?)(?=\n<div class="scr|\n</div></body>)', src, re.S):
    page = head + '<div class="row">' + m.group(1) + '</div></body></html>'
    open(os.path.join(tmp, m.group(2) + '.html'), 'w').write(page)
    open(os.path.join(tmp, m.group(2) + '-light.html'), 'w').write(lighten(page))
PY
mkdir -p docs/images
for f in "$TMP"/*.html; do
  n="$(basename "$f" .html)"
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --window-size=344,640 --screenshot="docs/images/$n.png" "file://$f" >/dev/null 2>&1
  echo "rendered docs/images/$n.png"
done
rm -rf "$TMP"
