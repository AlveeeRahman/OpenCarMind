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
shot tool 1472 480 docs/images/workflow/tool.png
shot inference 1472 480 docs/images/workflow/inference.png
shot local 1472 480 docs/images/workflow/local-first.png
shot sync 1472 480 docs/images/workflow/wifi-link.png
shot scheduler 1472 480 docs/images/workflow/scheduler.png
shot banner 1280 420 docs/images/banner.png
shot logo 512 512 assets/logo/opencarmind-logo.png 00000000
