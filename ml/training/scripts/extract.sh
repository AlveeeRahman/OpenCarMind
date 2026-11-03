#!/bin/sh
# Unpacks the downloaded zips next to themselves (data/<name>/extracted). Data is treated as untrusted
# input: nothing in it is executed, only parsed.
set -e
cd "$(dirname "$0")/../data"
for d in driving_dataset car_hacking can_signal_extraction; do
  [ -f "$d/$d.zip" ] || continue
  mkdir -p "$d/extracted"
  unzip -q -o "$d/$d.zip" -d "$d/extracted" || true
  echo "$d: $(find "$d/extracted" -type f | wc -l | tr -d ' ') files"
done
