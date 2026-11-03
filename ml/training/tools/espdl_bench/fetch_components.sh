#!/bin/sh
# Download ESP-DL and its two dependencies from the Espressif component registry into components/, under the plain
# names ESP-DL's CMakeLists expects. Build with IDF_COMPONENT_MANAGER=0 so the manager does not try to fetch them again.
set -e
cd "$(dirname "$0")"; mkdir -p components
for spec in esp-dl:3.3.14 dl_fft:0.8.0 esp_new_jpeg:1.0.2; do
  c=${spec%%:*}; v=${spec##*:}
  curl -sSf -o "/tmp/$c.zip" "https://components-file.espressif.com/components/espressif/$c/$v/espressif__$c-v$v.zip"
  rm -rf "components/$c"; mkdir -p "components/$c"; unzip -q "/tmp/$c.zip" -d "components/$c"; rm "/tmp/$c.zip"
  echo "$c $v"
done
