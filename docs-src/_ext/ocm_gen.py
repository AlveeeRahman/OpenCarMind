"""Generate the documentation pages that come from the repository's own Markdown.

The README, the user manuals in docs/manual, CHANGELOG.md and the other docs stay the single source. This module
reads them, splits the manuals into one page per chapter and the changelog into one page per version, cuts named
sections out of the README and the guides, and rewrites links and images so they work inside the Sphinx site:

    a link to a part of the repository that has a page here   -> a link to that page
    a link to anything else in the repository                 -> its file on GitHub
    an image                                                  -> an image directive with a path to the real file

It runs as a Sphinx extension (on builder-inited, before Sphinx reads the sources) and as a script:

    python3 docs-src/_ext/ocm_gen.py          write the generated pages and print what could not be resolved

Generated files go to docs-src/manuals/, docs-src/release-notes/ and docs-src/_gen/. All three are in .gitignore.
"""
import html
import os
import posixpath
import re
import shutil
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent          # docs-src/
REPO = SRC.parent
GITHUB = "https://github.com/AlveeeRahman/OpenCarMind"
BRANCH = "main"

MANUALS = {
    # key: (source, target prefix, label for the index line)
    "esp32-s3": ("docs/manual/esp32-s3.md", "s3-", "ESP32-S3, OpenCarMind 2.5.1"),
    "edgeos-2.1.0": ("docs/manual/edgeos-2.1.0.md", "e21-", "ESP-Mosaico, OpenCarMind EdgeOS 2.1.0"),
    "edgeos-2.0.0": ("docs/manual/edgeos-2.0.0.md", "e20-", "ESP-Mosaico, OpenCarMind EdgeOS 2.0.0 (older)"),
}
OLDER_MANUALS = {"edgeos-2.0.0": "edgeos-2.1.0"}

# Whole files that become one page each, and the page they land on.
WHOLE = {
    "docs/CONFIGURATION.md": "reference/configuration",
    "docs/ADAPTERS.md": "hardware/adapters",
    "docs/MOSAICO.md": "guides/mosaico-guide",
    "docs/DASHBOARD.md": "guides/dashboard",
    "desktop/README.md": "guides/desktop-app",
    "docs/MODEL.md": "reference/model-files",
    "MODEL_CARD.md": "research/model-card",
    "docs/TEST_DRIVES.md": "research/test-drives",
    "docs/FIELD_CHECKS.md": "research/field-checks",
    "CONTRIBUTING.md": "contributing/index",
    "SECURITY.md": "contributing/security",
    "THIRD_PARTY_NOTICES.md": "contributing/licence",
}

# Sections cut out of a file. name: (source, selector). A selector is the heading text ("## Title" or "### Title"),
# or "details:" plus the start of a <details> summary. "+nodetails" drops the <details> blocks of a section.
FRAGMENTS = {
    "readme-intro": ("README.md", "intro"),
    "readme-choose-model": ("README.md", "## Choose your model"),
    "readme-what-you-get": ("README.md", "## What you get"),
    "readme-local-first": ("README.md", "## Local-first"),
    "readme-how-it-works": ("README.md", "## How it works"),
    "readme-screens": ("README.md", "## The screens"),
    "readme-hardware": ("README.md", "## Hardware"),
    "readme-build-flash": ("README.md", "## Build and flash"),
    "readme-models": ("README.md", "## The models+intro"),
    "readme-test-drives": ("README.md", "### The test drives"),
    "readme-rev-results": ("README.md", "### CarMind R.E.V results so far"),
    "readme-how-made": ("README.md", "### How the models are made"),
    "readme-rev-learns": ("README.md", "### What R.E.V learns"),
    "readme-rev-board": ("README.md", "### How R.E.V learns on the board"),
    "readme-xai": ("README.md", "### Looking inside R.E.V with explainable AI"),
    "readme-steps": ("README.md", "### How R.E.V got better, step by step"),
    "readme-edge-learning": ("README.md", "### Edge learning: R.E.V picks what to learn"),
    "readme-status": ("README.md", "## Status"),
    "readme-edgeos-intro": ("README.md", "## OpenCarMind EdgeOS 2.1.0 (ESP-Mosaico)+intro"),
    "readme-new-210": ("README.md", "### New in OpenCarMind EdgeOS 2.1.0"),
    "readme-what-it-is": ("README.md", "### What it is"),
    "readme-parts": ("README.md", "### The parts and why each exists"),
    "readme-runlevels": ("README.md", "### Services and runlevels"),
    "readme-admission": ("README.md", "### Model admission by response-time analysis"),
    "readme-budgets": ("README.md", "### Scheduler budgets per core"),
    "readme-ai-services": ("README.md", "### AI services in OpenCarMind EdgeOS+nodetails"),
    "readme-soc": ("README.md", "details:For experts: how the battery charge estimator works"),
    "readme-soh": ("README.md", "details:For experts: how the battery health estimate works"),
    "readme-anomaly": ("README.md", "details:For experts: how the battery anomaly detector works"),
    "readme-ids": ("README.md", "details:For experts: how the network intrusion detector works"),
    "readme-deadlines": ("README.md", "details:For experts: why the AI services meet their deadlines"),
    "readme-not-shipped": ("README.md", "details:For experts: the models we tested and did not ship"),
    "readme-costs": ("README.md", "### What it costs"),
    "readme-on-board": ("README.md", "### What you see on the board"),
    "readme-after-check": ("README.md", "### Changes after the board check"),
    "readme-pairing": ("README.md", "### Pairing safety"),
    "readme-flash-back": ("README.md", "### Flash it, or go back to mossREV 1.2.0"),
    "readme-prepared-off": ("README.md", "### Prepared but off: A/B rollback and the battery as a UPS"),
    "readme-verified": ("README.md", "### What is verified on the board"),
    "readme-limits": ("README.md", "## Limitations and future work"),
    "readme-planned": ("README.md", "## Planned"),
    "readme-references": ("README.md", "## References"),
    "readme-layout": ("README.md", "## Repository layout"),
    "readme-credits": ("README.md", "## Credits and licence"),
    "mosaico-hardware": ("docs/MOSAICO.md", "## The hardware"),
    "mosaico-flashing": ("docs/MOSAICO.md", "## Flashing"),
    "mosaico-storage": ("docs/MOSAICO.md", "## Storage that survives resets"),
    "mosaico-nand": ("docs/MOSAICO.md", "## The 128 MB NAND"),
    "mosaico-clock": ("docs/MOSAICO.md", "## Clock, brightness and theme"),
    "config-sd-files": ("docs/CONFIGURATION.md", "## SD card files"),
    "config-wifi": ("docs/CONFIGURATION.md", "## Wi-Fi networks"),
    "config-clock": ("docs/CONFIGURATION.md", "## The clock"),
    "config-usb-setup": ("docs/CONFIGURATION.md", "## Setup over USB with the desktop app"),
    "config-json": ("docs/CONFIGURATION.md", "## config.json"),
    "config-menuconfig": ("docs/CONFIGURATION.md", "## menuconfig options"),
}

# Where a README heading is shown, when a fragment carries it. Filled in by the page specs below.
FRAGMENT_PAGES = {
    "readme-intro": "index",
    "readme-choose-model": "concepts/models",
    "readme-what-you-get": "index",
    "readme-local-first": "concepts/how-it-works",
    "readme-how-it-works": "concepts/how-it-works",
    "readme-screens": "guides/s3-screens",
    "readme-hardware": "hardware/boards",
    "readme-build-flash": "get-started/esp32-s3",
    "readme-models": "concepts/models",
    "readme-test-drives": "research/model-results",
    "readme-rev-results": "research/model-results",
    "readme-how-made": "concepts/models",
    "readme-rev-learns": "concepts/models",
    "readme-rev-board": "concepts/models",
    "readme-xai": "concepts/models",
    "readme-steps": "concepts/models",
    "readme-edge-learning": "concepts/models",
    "readme-status": "research/index",
    "readme-edgeos-intro": "concepts/edgeos",
    "readme-new-210": "concepts/health-app",
    "readme-what-it-is": "concepts/edgeos",
    "readme-parts": "concepts/edgeos",
    "readme-runlevels": "concepts/edgeos",
    "readme-admission": "concepts/edgeos",
    "readme-budgets": "concepts/edgeos",
    "readme-ai-services": "concepts/ai-services",
    "readme-soc": "concepts/battery",
    "readme-soh": "concepts/battery",
    "readme-anomaly": "concepts/battery",
    "readme-ids": "concepts/network-guard",
    "readme-deadlines": "concepts/ai-services",
    "readme-not-shipped": "concepts/ai-services",
    "readme-costs": "concepts/edgeos",
    "readme-on-board": "concepts/edgeos",
    "readme-after-check": "concepts/pairing-safety",
    "readme-pairing": "concepts/pairing-safety",
    "readme-flash-back": "get-started/esp-mosaico",
    "readme-prepared-off": "concepts/edgeos",
    "readme-verified": "research/verified-on-board",
    "readme-limits": "research/index",
    "readme-planned": "research/index",
    "readme-references": "research/references",
    "readme-layout": "contributing/index",
    "readme-credits": "contributing/licence",
    "mosaico-hardware": "hardware/boards",
    "mosaico-flashing": "get-started/esp-mosaico",
    "mosaico-storage": "guides/storage",
    "mosaico-nand": "guides/storage",
    "mosaico-clock": "guides/wifi-clock",
    "config-sd-files": "guides/storage",
    "config-wifi": "guides/wifi-clock",
    "config-clock": "guides/wifi-clock",
    "config-usb-setup": "guides/desktop-app",
    "config-json": "reference/settings",
    "config-menuconfig": "reference/settings",
}

GENERATED_DIRS = ("manuals", "release-notes", "_gen")

# ------------------------------------------------------------------ helpers

HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
ANCHOR_LINE = re.compile(r'^<a id="([^"]+)"></a>\s*(.*)$')
LINK = re.compile(r"(!?)\[((?:[^\[\]]|\[[^\[\]]*\])*)\]\(([^)\s]+)\)")
IMG = re.compile(r"<img\b[^>]*>")
ATTR = re.compile(r'(\w[\w-]*)="([^"]*)"')


def slug(text):
    """The heading anchor GitHub and MyST both make: lower case, punctuation dropped, spaces to hyphens."""
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"[*`]", "", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    return re.sub(r"[^\w\- ]", "", text.strip().lower()).replace(" ", "-")


def read(rel):
    return (REPO / rel).read_text(encoding="utf-8")


def code_mask(lines):
    """True for every line inside a fenced code block, fences included."""
    inside, fence, out = False, "", []
    for ln in lines:
        m = re.match(r"^\s*(```+|~~~+)", ln)
        if m and not inside:
            inside, fence = True, m.group(1)
            out.append(True)
            continue
        if inside:
            out.append(True)
            if ln.strip().startswith(fence):
                inside = False
            continue
        out.append(False)
    return out


def headings(lines):
    """(index, level, text) for every heading outside code."""
    mask = code_mask(lines)
    out = []
    for i, ln in enumerate(lines):
        if mask[i]:
            continue
        m = HEADING.match(ln)
        if m:
            out.append((i, len(m.group(1)), m.group(2)))
    return out


def section(lines, title, level):
    """The lines of the section with this heading, heading included, up to the next heading at this level or above."""
    hs = headings(lines)
    for n, (i, lv, text) in enumerate(hs):
        if lv == level and text == title:
            end = next((j for j, lv2, _ in hs[n + 1:] if lv2 <= level), len(lines))
            while end > i and (not lines[end - 1].strip() or ANCHOR_LINE.match(lines[end - 1])):
                end -= 1
            return lines[i:end]
    raise KeyError(f"heading not found: {'#' * level} {title}")


def details_block(lines, start):
    for i, ln in enumerate(lines):
        if ln.startswith("<summary>") and start in re.sub(r"<[^>]+>", "", ln):
            j = i + 1
            while not lines[j].startswith("</details>"):
                j += 1
            return lines[i + 1:j]
    raise KeyError(f"details not found: {start}")


def drop_details(lines):
    out, skip = [], False
    for ln in lines:
        if ln.startswith("<details"):
            skip = True
        if not skip:
            out.append(ln)
        if ln.startswith("</details>"):
            skip = False
    return out


# ------------------------------------------------------------------ anchors

class Site:
    """Which page shows which source file and which of its anchors."""

    def __init__(self):
        self.home = {}        # source -> docname of its main page
        self.anchors = {}     # (source, anchor) -> (docname, target) ; target "" for a heading slug
        self.unresolved = []
        self.whole = {}       # source -> docname of the page that shows the whole file

    def register(self, src, docname, lines, prefix="", home=False):
        if home or src not in self.home:
            self.home[src] = docname
        mask = code_mask(lines)
        for i, ln in enumerate(lines):
            if mask[i]:
                continue
            m = ANCHOR_LINE.match(ln)
            if m:
                self.anchors.setdefault((src, m.group(1)), (docname, prefix + m.group(1)))
            h = HEADING.match(ln)
            if h:
                self.anchors.setdefault((src, slug(h.group(2))), (docname, ""))

    def link(self, src, target, here):
        """Rewrite one link target found in file src, for the page `here`."""
        if re.match(r"^[a-z][a-z0-9+.-]*:", target):
            return target
        path, _, anchor = target.partition("#")
        rel = posixpath.normpath(posixpath.join(posixpath.dirname(src), path)) if path else src
        if anchor and rel == src and self.whole.get(src) == here:
            hit = self.anchors.get((rel, anchor))
            return f"#{hit[1]}" if hit and hit[1] else f"#{anchor}"
        if anchor:
            hit = self.anchors.get((rel, anchor))
            if hit:
                docname, tgt = hit
                if tgt:
                    return f"#{tgt}"
                return f"#{anchor}" if docname == here else f"/{docname}.md#{anchor}"
        elif rel in self.home:
            return f"/{self.home[rel]}.md"
        if not (REPO / rel).exists():
            self.unresolved.append(f"{src}: {target}")
        kind = "tree" if (REPO / rel).is_dir() else "blob"
        return f"{GITHUB}/{kind}/{BRANCH}/{rel}" + (f"#{anchor}" if anchor else "")


# ------------------------------------------------------------------ conversion

def image_directive(src, attrs, out_dir, many):
    rel = posixpath.normpath(posixpath.join(posixpath.dirname(src), attrs["src"]))
    if not (REPO / rel).is_file():
        raise FileNotFoundError(f"{src}: image {attrs['src']} not found")
    path = os.path.relpath(REPO / rel, out_dir).replace(os.sep, "/")
    alt = html.unescape(attrs.get("alt", "")).replace("\n", " ").strip()
    width = int(attrs["width"]) if attrs.get("width", "").isdigit() else 0
    cls = "ocm-shot" if (many or 0 < width <= 320) else "ocm-figure"
    out = [f"```{{image}} {path}", f":alt: {alt}" if alt else ":alt:", f":class: {cls}"]
    if 0 < width <= 320:
        out.append(f":width: {width}px")
    out.append("```")
    return out


def convert(src, lines, here, site, prefix="", shift=0, drop_first_heading=False):
    """Turn source Markdown into MyST for the page `here` (a docname)."""
    out_dir = (SRC / here).parent
    lines = [ln for ln in lines if not re.fullmatch(r"\s*<!--.*-->\s*", ln)]
    mask = code_mask(lines)
    out, i, first = [], 0, True
    while i < len(lines):
        ln = lines[i]
        if mask[i]:
            out.append(ln)
            i += 1
            continue
        h = HEADING.match(ln)
        if h:
            if drop_first_heading and first:
                first = False
                i += 1
                continue
            first = False
            level = max(1, len(h.group(1)) - shift)
            out.append("#" * level + " " + rewrite_inline(src, h.group(2), here, site))
            i += 1
            continue
        a = ANCHOR_LINE.match(ln)
        if a:
            out.extend([f"({prefix}{a.group(1)})=", ""] if not a.group(2) else [f"({prefix}{a.group(1)})=", a.group(2)])
            i += 1
            continue
        m = re.match(r"^> \[!(NOTE|WARNING|TIP|IMPORTANT|CAUTION)\]\s*$", ln)
        if m:
            kind = {"NOTE": "note", "WARNING": "warning", "TIP": "tip", "IMPORTANT": "important",
                    "CAUTION": "caution"}[m.group(1)]
            body = []
            i += 1
            while i < len(lines) and lines[i].startswith(">"):
                body.append(lines[i][2:] if lines[i].startswith("> ") else lines[i][1:])
                i += 1
            out.append(f":::{{{kind}}}")
            out.extend(convert(src, body, here, site, prefix))
            out.append(":::")
            continue
        if ln.startswith("<details"):
            title = "More"
            j = i
            while j < len(lines) and "<summary>" not in lines[j]:
                j += 1
            if j < len(lines):
                title = re.sub(r"<[^>]+>", "", re.search(r"<summary>(.*?)</summary>", lines[j]).group(1)).strip()
            body, k = [], j + 1
            while not lines[k].startswith("</details>"):
                body.append(lines[k])
                k += 1
            out.append(f"::::{{dropdown}} {title}")
            out.extend(convert(src, body, here, site, prefix))
            out.append("::::")
            i = k + 1
            continue
        # an HTML block: lines up to the next blank line
        if ln.lstrip().startswith("<p") or ln.lstrip().startswith("<img"):
            j = i
            block = []
            while j < len(lines) and lines[j].strip():
                block.append(lines[j])
                j += 1
            text = "\n".join(block)
            chunks = re.findall(r"<p\b[^>]*>.*?</p>|<img\b[^>]*>", text, re.S)
            if chunks and not re.sub(r"<p\b[^>]*>.*?</p>|<img\b[^>]*>", "", text, flags=re.S).strip():
                done = []
                for chunk in chunks:
                    imgs = IMG.findall(chunk)
                    rest = re.sub(r"</?p\b[^>]*>|<img\b[^>]*>", "", chunk).strip()
                    sub = re.fullmatch(r"<p[^>]*>\s*<sub>(.*)</sub>\s*</p>", chunk, re.S)
                    if imgs and not rest:
                        many = len(imgs) > 1
                        if many:
                            done.append(":::{container} ocm-row")
                        for tag in imgs:
                            done.extend(image_directive(src, dict(ATTR.findall(tag)), out_dir, many))
                        if many:
                            done.append(":::")
                    elif sub:
                        done.extend(["{.ocm-caption}", rewrite_inline(src, html_links_to_md(sub.group(1)), here, site)])
                    else:
                        done = None
                        break
                    done.append("")
                if done is not None:
                    out.extend(done)
                    i = j
                    continue
        out.append(rewrite_inline(src, ln, here, site))
        i += 1
    return out


def html_links_to_md(text):
    return re.sub(r'<a href="([^"]+)">(.*?)</a>', r"[\2](\1)", text)


HTML_TAGS = r"(?:a|b|i|em|strong|sub|sup|br|img|p|code|details|summary|span|kbd|ol|ul|li|div|table|tr|td|th)"
PLACEHOLDER = re.compile(r"<(?!/?" + HTML_TAGS + r"\b)(?!!--)([A-Za-z][^<>]{0,40})>")


def escape_placeholders(text):
    """<name> or <port> in prose is a placeholder, not an HTML tag: show it. Code spans are left alone."""
    parts = re.split(r"(`+[^`]*`+)", text)
    return "".join(p if p.startswith("`") else PLACEHOLDER.sub(r"&lt;\1&gt;", p) for p in parts)


def rewrite_inline(src, text, here, site):
    text = escape_placeholders(text)
    def sub(m):
        bang, label, target = m.groups()
        if bang:
            rel = posixpath.normpath(posixpath.join(posixpath.dirname(src), target))
            if (REPO / rel).is_file():
                path = os.path.relpath(REPO / rel, (SRC / here).parent).replace(os.sep, "/")
                return f"![{label}]({path})"
            return m.group(0)
        return f"[{label}]({site.link(src, target, here)})"
    return LINK.sub(sub, text)


def write(docname_or_path, lines):
    p = SRC / (docname_or_path if docname_or_path.endswith(".md") else docname_or_path + ".md")
    p.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines).rstrip() + "\n"
    text = re.sub(r"\n{3,}", "\n\n", text)
    p.write_text(text, encoding="utf-8")


# ------------------------------------------------------------------ manuals

def split_manual(text):
    """(front lines, [(anchor id, chapter lines)]) from a manual's Markdown."""
    lines = [ln for ln in text.split("\n") if not re.fullmatch(r"\s*<!--.*-->\s*", ln)]
    starts = [i for i, ln in enumerate(lines) if ANCHOR_LINE.match(ln) and i + 2 < len(lines)
              and lines[i + 2].startswith("## ")]
    front = lines[:starts[0]]
    cut = next(i for i, ln in enumerate(front) if ln.startswith("## Contents"))
    front = front[:cut]
    chapters = []
    for n, s in enumerate(starts):
        e = starts[n + 1] if n + 1 < len(starts) else len(lines)
        body = lines[s:e]
        while body and (not body[-1].strip() or body[-1].strip() == "---"):
            body.pop()
        anchor = ANCHOR_LINE.match(lines[s]).group(1)
        chapters.append((anchor, body))
    footer = []
    if chapters:
        last = chapters[-1][1]
        if "---" in [x.strip() for x in last]:
            k = [x.strip() for x in last].index("---")
            footer = last[k + 1:]
            chapters[-1] = (chapters[-1][0], last[:k])
    return front, chapters, footer


def plan_manuals(site):
    plans = []
    for key, (src, prefix, label) in MANUALS.items():
        front, chapters, footer = split_manual(read(src))
        site.register(src, f"manuals/{key}/index", front, prefix, home=True)
        for anchor, body in chapters:
            site.register(src, f"manuals/{key}/{anchor}", body, prefix)
        plans.append((key, src, prefix, label, front, chapters, footer))
    return plans


def write_manuals(plans, site):
    for key, src, prefix, label, front, chapters, footer in plans:
        here = f"manuals/{key}/index"
        out = [f"({prefix}index)=", ""]
        out += convert(src, front, here, site, prefix)
        if key in OLDER_MANUALS:
            new = OLDER_MANUALS[key]
            out += ["", ":::{note}", f"This manual is for an older version. The current one is the "
                    f"[{MANUALS[new][2]} manual](/manuals/{new}/index.md).", ":::"]
        out += ["", "```{toctree}", ":maxdepth: 1", ":caption: Chapters", ""]
        out += [anchor for anchor, _ in chapters]
        out += ["```"]
        if footer:
            out += ["", "{.ocm-caption}", " ".join(x.strip() for x in footer if x.strip())]
        write(here, out)
        for anchor, body in chapters:
            here = f"manuals/{key}/{anchor}"
            write(here, convert(src, body, here, site, prefix, shift=1))


# ------------------------------------------------------------------ changelog

def plan_changelog(site):
    lines = read("CHANGELOG.md").split("\n")
    hs = [(i, t) for i, lv, t in headings(lines) if lv == 2]
    versions = []
    for n, (i, title) in enumerate(hs):
        e = hs[n + 1][0] if n + 1 < len(hs) else len(lines)
        name = slug(title.replace("OpenCarMind EdgeOS", "edgeos").replace("(ESP-Mosaico)", ""))
        name = re.sub(r"-+", "-", name).strip("-")
        doc = f"release-notes/{name}"
        site.register("CHANGELOG.md", doc, lines[i:e])
        versions.append((doc, lines[i:e]))
    site.home["CHANGELOG.md"] = "release-notes"
    return versions


def write_changelog(versions, site):
    for doc, body in versions:
        write(doc, convert("CHANGELOG.md", body, doc, site, shift=1))
    write("_gen/release-notes-toc", ["```{toctree}", ":maxdepth: 1", ""] +
          ["/" + d for d, _ in versions] + ["```"])


# ------------------------------------------------------------------ messages

QUOTED_ITEM = re.compile(r'^- \*\*"(.+?)"(?::\*\*|\*\*:?)\s*(.*)$')
QUOTED = re.compile(r'"([^"]{3,120})"')


def messages(plans, site):
    """One table per manual of every message it quotes, with what it means and the chapter it is in."""
    out = []
    for key, src, prefix, label, front, chapters, footer in plans:
        if key in OLDER_MANUALS:
            continue
        rows, seen = [], set()
        for anchor, body in chapters:
            title = next((HEADING.match(x).group(2) for x in body if HEADING.match(x)), anchor)
            mask = code_mask(body)
            for k, ln in enumerate(body):
                if mask[k] or ln.startswith("|") or "<img" in ln:
                    continue
                m = QUOTED_ITEM.match(ln)
                if m:
                    found = [(m.group(1), m.group(2))]
                else:
                    found = [(q, ln.lstrip("- ")) for q in QUOTED.findall(ln) if len(q.split()) >= 2]
                for msg, meaning in found:
                    if msg in seen:
                        continue
                    seen.add(msg)
                    meaning = re.sub(r"\*\*", "", meaning).strip()
                    meaning = re.sub(r"^(?:>\s*|\d+\.\s+)+", "", meaning)
                    rows.append((msg, meaning, anchor, title))
        here = "reference/messages"
        out += [f"## {label}", "", "| Message on the board | What it means | Chapter |", "|---|---|---|"]
        for msg, meaning, anchor, title in rows:
            cell = rewrite_inline(src, meaning, here, site).replace("|", "\\|")
            msg = msg.replace("|", "\\|").replace("&lt;", "<").replace("<", "&lt;").replace("*", "\\*")
            out.append(f"| \u201c{msg}\u201d | {cell} | [{title}](/manuals/{key}/{anchor}.md) |")
        out.append("")
    write("_gen/messages", out)


# ------------------------------------------------------------------ fragments and whole files

def plan_fragments(site):
    plans = []
    for name, (src, sel) in FRAGMENTS.items():
        lines = read(src).split("\n")
        if sel == "intro":
            start = next(i for i, ln in enumerate(lines) if ln.startswith("OpenCarMind is"))
            end = next(i for i, ln in enumerate(lines) if ln.startswith("## Contents"))
            body = lines[start:end]
        elif sel.startswith("details:"):
            body = details_block(lines, sel[len("details:"):])
        else:
            flags = sel.split("+")
            hm = HEADING.match(flags[0])
            body = section(lines, hm.group(2), len(hm.group(1)))
            if "nodetails" in flags:
                body = drop_details(body)
            if "intro" in flags:
                k = next((n for n, ln in enumerate(body[1:], 1) if HEADING.match(ln)), len(body))
                body = body[:k]
        page = FRAGMENT_PAGES[name]
        hm = HEADING.match(body[0]) if body else None
        if hm:
            site.anchors.setdefault((src, slug(hm.group(2))), (page, name))
        site.register(src, page, body)
        plans.append((name, src, body, page))
    for src, page in WHOLE.items():
        site.whole[src] = page
        site.register(src, page, read(src).split("\n"), home=True)
    return plans


def write_fragments(plans, site):
    for name, src, body, page in plans:
        hm = HEADING.match(body[0]) if body else None
        shift = (len(hm.group(1)) - 2) if hm else 0
        lines = convert(src, body, page, site, shift=shift, drop_first_heading=True)
        write(f"_gen/{name}", ([f"({name})=", ""] if hm else []) + lines)
    for src, page in WHOLE.items():
        write(f"_gen/whole-{page.replace('/', '-')}", convert(src, read(src).split("\n"), page, site))


# ------------------------------------------------------------------ headers

HEADERS = {
    "Operating-system layer": ["os_core.h", "os.h", "os_hooks.h", "sched_mon.h", "sys_mode.h", "reset_class.h"],
    "AI services": ["ai_svc_core.h", "ai_svc.h", "ai_snap.h"],
    "Battery": ["bat_est_core.h", "bat_est.h", "bat_soh.h", "bat_anom_core.h", "bat_anom.h"],
    "Network guard and pairing": ["ids_core.h", "ids_svc.h", "adapter_guard.h", "adapter_match.h", "obd_pair.h",
                                  "clock_set.h"],
    "Models and learning": ["ml_infer.h", "rev_learn.h", "rev_cal.h", "rev_tank.h", "rev_torque.h", "rev_state.h"],
    "Car link and logging": ["obd_ble.h", "elm327.h", "obd_fields.h", "ecu_select.h", "fault_engine.h", "econ.h",
                             "custom_pids.h", "sdlog.h", "events.h", "nand_store.h", "fat_mirror.h"],
    "USB, Wi-Fi and version": ["usb_door.h", "wifi_sync.h", "wifi_creds.h", "version.h"],
}

FUNC = re.compile(r"^(?!typedef\b)(?!return\b)[A-Za-z_][\w\s\*,]*?\b([A-Za-z_]\w*)\s*\((.*)\)\s*(?:__attribute__\S*)?$", re.S)


def _comment_text(raw):
    raw = re.sub(r"^/\*+|\*+/$", "", raw.strip())
    return " ".join(re.sub(r"^\s*\*\s?", "", x).strip() for x in raw.split("\n")).strip()


def header_parts(text):
    """The header's opening comment, and (declaration, the comment above or beside it) for each function."""
    m = re.match(r"\s*/\*(.*?)\*/", text, re.S)
    lead = ""
    if m:
        lead = "\n".join(re.sub(r"^ ?\* ?", "", x) for x in m.group(1).split("\n")).strip()
        text = text[m.end():]
    decls, notes, stmt, depth, i = [], [], "", 0, 0
    lines = text.split("\n")
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        if depth == 0 and not stmt:
            if not line:
                notes = []
                continue
            if line.startswith("/*"):
                block = line
                while "*/" not in block and i < len(lines):
                    block += "\n" + lines[i]
                    i += 1
                notes.append(_comment_text(block.split("*/")[0] + "*/"))
                rest = block.split("*/", 1)[1].strip()
                if not rest:
                    continue
                line = rest
            elif line.startswith("//"):
                notes.append(line[2:].strip())
                continue
            if line.startswith("#"):
                while line.endswith("\\") and i < len(lines):
                    line = lines[i].strip()
                    i += 1
                notes = []
                continue
        if depth > 0:
            depth += line.count("{") - line.count("}")
            if depth == 0:
                stmt, notes = "", []
            continue
        trailing = " ".join(_comment_text("/*" + c + "*/") for c in re.findall(r"/\*(.*?)\*/", line))
        code = re.sub(r"/\*.*?\*/", "", line)
        code = re.sub(r"//.*$", "", code).strip()
        if "/*" in code:                                  # a comment that goes on below the code
            block = code.split("/*", 1)[1]
            code = code.split("/*", 1)[0].strip()
            while "*/" not in block and i < len(lines):
                block += "\n" + lines[i]
                i += 1
            trailing = (trailing + " " + _comment_text(block.split("*/")[0])).strip()
        if code.startswith(("extern \"C\"", "}")):
            continue
        stmt = (stmt + " " + code).strip()
        if trailing:
            notes.append(trailing)
        if "{" in code:
            depth = code.count("{") - code.count("}")
            head = stmt.split("{")[0].strip()
            fm = FUNC.match(head)
            if fm and "static inline" in head:
                decls.append((re.sub(r"\s+", " ", head), " ".join(notes)))
            if depth == 0:
                stmt, notes = "", []
            continue
        if stmt.endswith(";"):
            fm = FUNC.match(stmt[:-1].strip())
            if fm and "(" in stmt and "=" not in stmt.split("(")[0]:
                decls.append((re.sub(r"\s+", " ", stmt), " ".join(notes)))
            stmt, notes = "", []
    return lead, decls


def write_c_api():
    index = ["```{toctree}", ":maxdepth: 1", ""]
    for group, files in HEADERS.items():
        doc = "c-" + slug(group).replace("-and-", "-")
        index.append(f"/reference/c-api/{doc}")
        out = [f"# {group}", "", "Every function each header declares, with the comment the header gives it. The "
               "header's own opening comment is shown as written in the source.", ""]
        for name in files:
            p = REPO / "main" / name
            if not p.is_file():
                continue
            lead, decls = header_parts(p.read_text(encoding="utf-8"))
            out += [f"## {name}", "", f"Source: [main/{name}]({GITHUB}/blob/{BRANCH}/main/{name})", ""]
            if lead:
                out += ["```text", lead, "```", ""]
            seen, uniq = set(), []
            for d, c in decls:
                fn = FUNC.match(d.rstrip(";").split("__attribute__")[0].strip()).group(1)
                if fn not in seen:
                    seen.add(fn)
                    uniq.append((d, re.sub(r"-{3,}[^-]*-{3,}", "", c).strip()))
            if uniq:
                out += ["| Function | Comment in the header |", "|---|---|"]
                for d, c in uniq:
                    c = c.replace("|", "\\|").replace("`", "'")
                    out.append(f"| `{d.replace('|', '¦')}` | {c} |")
                out.append("")
        write(f"reference/c-api/{doc}", out)
    index.append("```")
    write("_gen/c-api-toc", index)


# ------------------------------------------------------------------ main

def generate():
    for d in GENERATED_DIRS:
        shutil.rmtree(SRC / d, ignore_errors=True)
    shutil.rmtree(SRC / "reference" / "c-api", ignore_errors=True)
    site = Site()
    manuals = plan_manuals(site)
    versions = plan_changelog(site)
    fragments = plan_fragments(site)
    write_manuals(manuals, site)
    write_changelog(versions, site)
    write_fragments(fragments, site)
    messages(manuals, site)
    write_c_api()
    return site


def on_builder_inited(app):
    site = generate()
    for u in sorted(set(site.unresolved)):
        print(f"ocm_gen: link to a missing file: {u}")


def setup(app):
    app.connect("builder-inited", on_builder_inited)
    return {"version": "1", "parallel_read_safe": True, "parallel_write_safe": True}


if __name__ == "__main__":
    s = generate()
    for u in sorted(set(s.unresolved)):
        print("unresolved:", u)
    sys.exit(0)
