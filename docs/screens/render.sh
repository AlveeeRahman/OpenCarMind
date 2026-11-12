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
head = src[:src.index('<body>') + 6].replace('.row{display:flex;gap:24px;padding:24px}', '.row{display:block;padding:0}').replace('body{margin:0;background:#0A0A0A', 'body{margin:0;background:#000000')
# dark -> light palette, mirrors THEME_DARK / THEME_LIGHT in main/display.c. One pass, so a light value that equals
# another dark value (dark muted becomes #4A4A4A, which is also the dark border) is never remapped twice.
MAP = {'#000000':'#FFFFFF', '#050505':'#FCFCFC', '#F2ECE1':'#15202D', '#B8B8B8':'#4A5563', '#8CC8FF':'#3B82F6',
       '#56F487':'#15803D', '#FCCA21':'#B45309', '#E64343':'#C62828', '#4A4A4A':'#8C8C8C', '#7A7A7A':'#6B7280',
       '#AD8DFD':'#7C3AED', '#72ECE4':'#0E7490', '#6CB35D':'#4D7C0F', '#FE86C2':'#A21CAF', '#38B8F6':'#1D4ED8',
       '#FE860F':'#EA6A12', '#FD122E':'#BE123C', '#F2A13A':'#DD6E0E', '#F08E54':'#C2410C', '#F2A33A':'#E5484D', '#FFF':'#15202D', '#FFFFFF':'#15202D'}
def lighten(html):
    return re.sub(r'#(?:[0-9A-Fa-f]{6}|fff)\b', lambda m: MAP.get(m.group(0).upper(), m.group(0)), html)
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
