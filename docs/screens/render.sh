#!/bin/sh
# Renders every screen in screens.html to docs/images/<id>.png (dark) and <id>-light.png at the
# panel's 2x size (344x640). The light variant applies the firmware's light palette by remapping
# every dark-theme colour, the same thing main/ui/display.c does at 06:00.
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
# dark -> light palette, mirrors THEME_DARK / THEME_LIGHT in main/ui/display.c. One pass, so a light value that equals
# another dark value (dark muted becomes #4A4A4A, which is also the dark border) is never remapped twice.
MAP = {'#000000':'#F5F5F5', '#050505':'#FFFFFF', '#F2ECE1':'#101418', '#B8B8B8':'#4A4A4A', '#8CC8FF':'#1F4FC2',
       '#56F487':'#1B6B2A', '#FCCA21':'#6B4A00', '#E64343':'#C62828', '#4A4A4A':'#8C8C8C', '#7A7A7A':'#6E6E6E',
       '#AD8DFD':'#5517A5', '#72ECE4':'#034239', '#6CB35D':'#40731A', '#FE86C2':'#971C65', '#38B8F6':'#196EA9',
       '#FE860F':'#A65F00', '#FD122E':'#D4002A', '#F2A13A':'#8F5200', '#FFF':'#101418', '#FFFFFF':'#101418'}
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
