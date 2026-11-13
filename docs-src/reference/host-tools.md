# Host tools

Tools that run on a computer. None of them needs a board. The descriptions below are the tools' own docstrings.

## ocm_report.py

Turns the session files from the SD card into one HTML report per drive and a summary for Dashy. See [Dashboard](../guides/dashboard.md).

```{literalinclude} ../../tools/ocm_report.py
:language: text
:start-after: '"""'
:end-before: '"""'
```

## gen_ml_params.py

Writes the model headers `main/ml_params.h` and `main/ml_params_rev.h` from the shipped model folders. CI checks that both headers match.

```{literalinclude} ../../tools/gen_ml_params.py
:language: text
:start-after: '"""'
:end-before: '"""'
```

## check_docs.py

Fails if a Markdown file links to a local file or image that does not exist, or if a shipped model's export record does not match its files.

## Host tests

The firmware's pure C parts build and run on a computer. `make -C tests/host` runs the main set. Named targets run one part each, for example:

| Target | What it checks |
|---|---|
| `make -C tests/host rev` | CarMind R.E.V's learners: fuel head, airflow constant, fill-up calibration, state file |
| `make -C tests/host os` | the OpenCarMind EdgeOS rules in `main/os_core.c` |
| `make -C tests/host ai` | the AI services' release table under overload and the ladder order |
| `make -C tests/host bat` | the battery charge estimator against the Mac reference |
| `make -C tests/host pair` | the pairing rule with the switch off and on |
| `make -C tests/host guard` | the adapter fingerprint, both decision tables and the question |
| `make -C tests/host clock` | the clock jump guard |

The fuzz tests in `tests/fuzz` run libFuzzer over the input parsers. See [Contributing](../contributing/index.md).
