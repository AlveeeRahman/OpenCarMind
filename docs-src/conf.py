# Sphinx configuration for the OpenCarMind documentation site.
#
#   pip install -r docs-src/requirements.txt
#   sphinx-build -W --keep-going -b html docs-src docs-src/_build/html
#
# Pages that come from the repository's Markdown (the manuals, the changelog, sections of the README) are written
# by _ext/ocm_gen.py when the build starts. See docs-src/README.md.
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE / "_ext"))
import ocm_gen  # noqa: E402

project = "OpenCarMind"
author = "Alve Rahman Akash"
copyright = f"{datetime.now(timezone.utc).year} Alve Rahman Akash, MIT licence"
release = "S3 2.5.1, OpenCarMind EdgeOS 2.1.0"
version = release

extensions = [
    "myst_parser",
    "sphinx_design",
    "sphinx_copybutton",
    "sphinx.ext.mathjax",
    "ocm_gen",
]

myst_enable_extensions = ["colon_fence", "dollarmath", "attrs_block", "attrs_inline"]
myst_heading_anchors = 4
suppress_warnings = []

exclude_patterns = ["_build", "_gen", "README.md", "Thumbs.db", ".DS_Store"]
templates_path = ["_templates"]

# The canonical address of the site: BuildForPeople serves this build there, and GitHub Pages serves a copy. Set OCM_DOCS_BASE_URL to build the
# same pages for another address: every link inside the site is relative, only <link rel="canonical"> uses this.
html_baseurl = os.environ.get("OCM_DOCS_BASE_URL", "https://buildforpeople.dev/docs/opencarmind/")

html_theme = "pydata_sphinx_theme"
html_title = "OpenCarMind documentation"
html_short_title = "OpenCarMind"
html_logo = str(REPO / "docs" / "assets" / "icon.png")
html_favicon = str(REPO / "docs" / "assets" / "icon.png")
html_static_path = ["_static"]
html_css_files = ["ocm.css"]
html_copy_source = False
html_show_sourcelink = False
html_last_updated_fmt = None

html_theme_options = {
    "logo": {"text": "OpenCarMind", "alt_text": "OpenCarMind documentation, home"},
    "navbar_align": "left",
    "navbar_center": ["navbar-nav"],
    "navbar_end": ["ocm-versions", "theme-switcher", "navbar-icon-links"],
    "navbar_persistent": ["search-button-field"],
    "header_links_before_dropdown": 6,
    "icon_links": [
        {"name": "GitHub", "url": "https://github.com/AlveeeRahman/OpenCarMind", "icon": "fa-brands fa-github"},
        {"name": "Releases", "url": "https://github.com/AlveeeRahman/OpenCarMind/releases",
         "icon": "fa-solid fa-tag"},
    ],
    "use_edit_page_button": True,
    "show_toc_level": 2,
    "show_prev_next": True,
    "navigation_with_keys": False,
    "secondary_sidebar_items": ["page-toc", "edit-this-page"],
    "footer_start": ["copyright"],
    "footer_end": ["sphinx-version", "theme-version"],
    "pygments_light_style": "default",
    "pygments_dark_style": "github-dark",
}

# "Edit on GitHub" opens the file a page really comes from: a manual, the changelog, a guide, or the page itself.
EDIT_PREFIXES = {f"manuals/{k}/": v[0] for k, v in ocm_gen.MANUALS.items()}
EDIT_PREFIXES["release-notes/"] = "CHANGELOG.md"
EDIT_PREFIXES["reference/c-api/"] = "docs-src/_ext/ocm_gen.py"
EDIT_WHOLE = {page + ".md": src for src, page in ocm_gen.WHOLE.items()}
EDIT_TEMPLATE = (
    "{% set ns = namespace(t='docs-src/' ~ file_name) %}"
    "{% for p, s in ocm_edit_prefixes.items() %}{% if file_name.startswith(p) %}{% set ns.t = s %}{% endif %}"
    "{% endfor %}{% if file_name in ocm_edit_whole %}{% set ns.t = ocm_edit_whole[file_name] %}{% endif %}"
    "https://github.com/AlveeeRahman/OpenCarMind/edit/main/{{ ns.t }}"
)

html_context = {
    "github_user": "AlveeeRahman",
    "github_repo": "OpenCarMind",
    "github_version": "main",
    "doc_path": "docs-src",
    "ocm_edit_prefixes": EDIT_PREFIXES,
    "ocm_edit_whole": EDIT_WHOLE,
    "edit_page_url_template": EDIT_TEMPLATE,
    "edit_page_provider_name": "GitHub",
    "default_mode": "auto",
}

# The firmware lines the switcher in the top bar jumps between. Every target is a page of this site.
ocm_versions = [
    ("ESP32-S3 2.5.1", "get-started/esp32-s3"),
    ("ESP32-S3 beta 2.5.1b", "release-notes/251b"),
    ("OpenCarMind EdgeOS 2.1.0", "get-started/esp-mosaico"),
    ("OpenCarMind EdgeOS 2.0.0 (older)", "manuals/edgeos-2.0.0/index"),
]
html_context["ocm_versions"] = ocm_versions

# Link check: pages inside the site must resolve, outside links are reported but do not fail the build
# (docs-src/check_links.py reads the result). These hosts refuse automated requests.
linkcheck_ignore = [
    r"https://www\.amazon\.com/.*",
    r"https://store\.mosaico\.ai/.*",
    r"https://www\.nasa\.gov/.*",
    r"https://scorecard\.dev/.*",
]
linkcheck_anchors_ignore_for_url = [r"https://github\.com/.*"]
linkcheck_timeout = 20
linkcheck_retries = 2
linkcheck_workers = 8
