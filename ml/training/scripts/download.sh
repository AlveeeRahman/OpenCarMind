#!/bin/sh
# Downloads the HCRL datasets (academic use; cite the papers listed in README.md) into data/<name>/.
# Dropbox shared folders are fetched as one zip by switching dl=0 to dl=1.
set -e
cd "$(dirname "$0")/../data"
dl() { # name url
  mkdir -p "$1"; cd "$1"
  echo "downloading $1 ..."
  curl -L --retry 3 -o "$1.zip" "$2"
  ls -la "$1.zip"
  cd ..
}
dl driving_dataset "https://www.dropbox.com/scl/fo/1qstaqilcuykczehipx6g/AEa8sTcRU33Rkn9pt1558eI?rlkey=4nai0q47ubctl7zrt6omwng88&st=mgdqxf79&dl=1"
dl car_hacking "https://www.dropbox.com/scl/fo/9rwsf9pclhvv9xxloojom/AF7JeRW893grZkigkulkAHk?rlkey=3h6zamu3kc262lrnipu5qden8&st=3g52q9hd&dl=1"
dl can_signal_extraction "https://www.dropbox.com/scl/fo/hwpvgkrngf8fqm8vrk3ch/AF5bkuZpv-WoKFl-Nqf--xk?rlkey=8i8xdzsk8597ls9clmpfj5bqi&st=to4i3p86&dl=1"
echo "all downloads finished"
