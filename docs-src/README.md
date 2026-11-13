# docs-src: the documentation site

The source of <https://alveeerahman.github.io/OpenCarMind/docs/>, built with Sphinx, MyST Markdown and
pydata-sphinx-theme. The landing page stays in `docs/`, and the Pages workflow serves this folder's build under
`/docs/` next to it.

```sh
pip install --require-hashes -r docs-src/requirements.txt         # Python 3.13
sphinx-build -W --keep-going -b html docs-src docs-src/_build/html
python3 -m http.server -d docs-src/_build/html 8000              # then open http://localhost:8000
```

## Where the text comes from

Most pages are built from Markdown that already lives in the repository, so there is one source for each text.
`_ext/ocm_gen.py` runs when the build starts and writes:

| Generated | From |
|---|---|
| `manuals/<name>/*.md`, one page per chapter | `docs/manual/esp32-s3.md`, `edgeos-2.1.0.md`, `edgeos-2.0.0.md` |
| `release-notes/*.md`, one page per release | `CHANGELOG.md` |
| `reference/c-api/*.md` | the public headers in `main/` |
| `_gen/*.md`, sections included by the hand-written pages | `README.md`, `docs/MOSAICO.md`, `docs/CONFIGURATION.md` and other docs |
| `_gen/messages.md`, the message table | the quoted messages in the current manuals |

These folders are in `.gitignore`. To change a manual, edit its Markdown in `docs/manual/`. To change which README
section a page shows, edit the tables at the top of `_ext/ocm_gen.py`. The pages in `index.md`, `get-started/`,
`guides/`, `concepts/`, `hardware/`, `reference/`, `research/` and `contributing/` are written here.

Links are rewritten as the pages are generated. A link to a part of the repository that has a page here goes to
that page, and any other link to the repository goes to the file on GitHub.

## Checks

| Check | Command |
|---|---|
| warnings fail the build | `sphinx-build -W --keep-going -b html docs-src docs-src/_build/html` |
| links inside the site must resolve, outside links are reported | `sphinx-build -b linkcheck docs-src docs-src/_build/linkcheck`, then `python3 docs-src/check_links.py` |
| house style on new and changed pages | `python3 docs-src/check_prose.py --changed origin/main` |
| the packed site works from a sub-path | `python3 docs-src/package.py docs-src/_build/html out`, then `python3 docs-src/package.py --check out/opencarmind-docs.tar.gz /docs/opencarmind/` |

## For other websites

The Pages build also publishes the site for other websites to show:

- `https://alveeerahman.github.io/OpenCarMind/docs/opencarmind-docs.tar.gz`: the whole built site with relative
  links only, so it works when unpacked under any path.
- `https://alveeerahman.github.io/OpenCarMind/docs/docs-version.json`: the commit it was built from, the build
  time in UTC and the firmware versions it documents.

Every link inside the site is relative. The only absolute address is `<link rel="canonical">`, which comes from
`OCM_DOCS_BASE_URL` (default `https://alveeerahman.github.io/OpenCarMind/docs/`).
