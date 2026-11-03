Files bundled with the desktop app. CI puts these here before building:
  opencarmind-firmware.bin   the merged firmware image the Board page flashes
  firmware-version.txt       its version
  esptool-LICENSE.txt        the licence of the bundled esptool (GPL-2.0, source: https://github.com/espressif/esptool)
Locally: run scripts/fetch-esptool.sh <target triple> and copy a firmware image from a release.
