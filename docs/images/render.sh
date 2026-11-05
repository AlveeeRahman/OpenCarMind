#!/bin/sh
# Renders the sources in docs/images/src to PNG with headless Chrome (2x). Needs Google Chrome and internet for the fonts.
# Run from the repository root:  sh docs/images/render.sh
set -e
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
[ -x "$CHROME" ] || CHROME="$(command -v google-chrome || command -v chromium || true)"
[ -n "$CHROME" ] || { echo "Chrome not found"; exit 1; }
shot() {  # name width height output [background]
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=2 --window-size="$2,$3" \
    ${5:+--default-background-color=$5} --virtual-time-budget=8000 --screenshot="$4" "file://$PWD/docs/images/src/$1.html" >/dev/null 2>&1
  echo "rendered $4"
}
shot tool 1200 608 docs/images/workflow/tool.png
shot inference 1200 632 docs/images/workflow/inference.png
shot local 1200 624 docs/images/workflow/local-first.png
shot clock 1200 816 docs/images/workflow/clock.png
shot scheduler 1200 800 docs/images/workflow/scheduler.png
shot rev-learning 1200 1616 docs/images/workflow/rev-learning.png
shot training 1200 728 docs/images/workflow/training.png
# needs docs/images/system.png and system-rev.png from docs/screens/render.sh
shot rev-changes 900 520 docs/images/rev-changes.png
shot banner 1280 420 docs/images/banner.png
shot logo 512 512 assets/logo/opencarmind-logo.png 00000000
shot mosaico-rev 1600 740 docs/images/mosaico-rev.png
shot lcd-amoled 1200 260 docs/images/lcd-amoled.png
