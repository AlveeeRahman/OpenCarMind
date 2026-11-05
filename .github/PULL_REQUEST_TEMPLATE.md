## What changes

One sentence on what this pull request does and why.

## How it was tested

Tick what you ran. "Compiles" and "ran on a car" are different claims, and the README keeps them apart.

- [ ] `make -C tests/host` and `make -C tests/host rev` (host tests)
- [ ] `idf.py build` for the ESP32-S3
- [ ] `python3 -m unittest discover -s tests` and `python3 tools/check_docs.py`
- [ ] `cargo test --manifest-path desktop/app/src-tauri/Cargo.toml` (if `desktop/` changed)
- [ ] Flashed to a board: which board, which adapter, which car

## Checklist

- [ ] One change per pull request
- [ ] Docs updated in the same pull request
- [ ] `CHANGELOG.md` has a line under Unreleased if a user would notice the change
- [ ] No Wi-Fi names, passwords, keys or personal drive data in the diff
