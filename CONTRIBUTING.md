# Contributing

Thanks for helping. This page covers how to build, test and send a change.

## Build and test

You need ESP-IDF v6.1 for the firmware and Python 3.12 for the tools.

```sh
make -C tests/host                                   # fault engine, custom PIDs, pattern layer, adapter names, input checks
python3 -m unittest discover -s tests -v             # report tool
python3 tools/check_docs.py                          # Markdown links and images
idf.py set-target esp32s3 && idf.py build            # firmware
cargo test --manifest-path desktop/app/src-tauri/Cargo.toml   # desktop app, needs Rust
```

If you change a file in `ml/onboard/`, regenerate `main/ml_params.h` with `python3 tools/gen_ml_params.py ml/onboard main/ml_params.h`; for `ml/onboard/rev/`, run `python3 tools/gen_ml_params.py ml/onboard/rev main/ml_params_rev.h MLR`. CI fails if a header and its files disagree.

## Sending a change

1. Open an issue first for anything bigger than a fix, so we can agree on the approach.
2. Keep a pull request to one change. Update the docs in the same pull request.
3. Wait for CI. It builds the firmware with Wi-Fi clock sync on and off, runs the host tests and the Python tests, and checks the docs. The `desktop` workflow builds the app and runs its tests on Windows, macOS and Linux when `desktop/` changes.
4. Say what you tested on a real board, if anything. "Compiles" and "ran on a car" are different claims, and the README keeps them apart.

## Style

Firmware is C11 and C++17 in the existing style: short functions, comments only where the reason is not obvious. Python follows what is in `tools/`: standard library first, small functions, no comment that repeats the code. Prose in the docs is plain: short sentences, a number or a name instead of an adjective, no hype.

## Reporting a security problem

Use the private route in [SECURITY.md](SECURITY.md), not a public issue.
