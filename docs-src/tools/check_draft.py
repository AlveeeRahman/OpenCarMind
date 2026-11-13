# Vendored from the humanizer skill (MIT licence, Copyright (c) 2026 Peter Yang), unchanged below this line.
# See THIRD_PARTY_NOTICES.md. docs-src/check_prose.py runs it on the documentation pages.
#!/usr/bin/env python3
"""Scan a humanized markdown draft for style violations and equation problems.

Usage:
    python check_draft.py path/to/draft.md
    python check_draft.py path/to/draft.md --style-only
    python check_draft.py path/to/draft.md --math-only

Exit code 1 means at least one ERROR was found. WARN items need human judgment.
"""

import argparse
import json
import re
import statistics
import sys
from pathlib import Path

# ---------------------------------------------------------------- word lists

NEVER = [
    r"delv(?:e|es|ed|ing)", r"embark\w*", r"enlightening", r"esteemed",
    r"shed(?:s|ding)? light", r"craft(?:s|ed|ing)?", r"imagine",
    r"realms?", r"game[- ]changers?", r"unlock\w*", r"discover\w*",
    r"skyrocket\w*", r"abyss", r"not alone", r"in a world where",
    r"revolutioni[sz]\w*", r"disruptive", r"utili[sz]\w*", r"dive deep",
    r"tapestry", r"illuminat\w*", r"unveil\w*", r"pivotal", r"intricate",
    r"elucidat\w*", r"hence", r"furthermore", r"moreover", r"however",
    r"harness\w*", r"exciting", r"groundbreaking", r"cutting[- ]edge",
    r"remarkabl\w*", r"remains to be seen", r"glimpse into", r"navigat\w*",
    r"landscapes?", r"stark", r"testament", r"in summary", r"in conclusion",
    r"boost\w*", r"opened up", r"powerful", r"inquir(?:y|ies)", r"ever[- ]evolving",
    r"foster(?:s|ed|ing)?", r"leverag\w*", r"facilitat\w*", r"empower\w*",
    r"streamlin\w*", r"robust", r"paradigm shift", r"beacon", r"multifaceted",
    r"meticulous\w*", r"paramount", r"transformative", r"elevat\w*", r"supercharg\w*",
    r"this is huge", r"this changes everything",
]

FILLER = [
    r"very", r"really", r"actually", r"literally", r"certainly",
    r"basically", r"probably", r"maybe", r"just", r"honestly", r"simply",
    r"truly", r"fundamentally", r"importantly", r"crucially", r"inherently",
    r"inevitably",
]

CONSTRUCTIONS = [
    (r"not just\b[^.!?]{0,60}?\bbut also\b", "\"not just X, but also Y\""),
    (r"\bit'?s not\b[^.!?]{0,80}[.!?]\s+it'?s\b", "\"It's not X. It's Y.\""),
    (r"here'?s the thing", "fake conversational hook"),
    (r"here'?s where it gets \w+", "fake conversational hook"),
    (r"think about it\b", "fake conversational hook"),
    (r"let that sink in", "fake conversational hook"),
    (r"the key takeaway", "generic framing"),
    (r"in closing", "generic framing"),
    (r"at the end of the day", "cliché"),
    (r"when it comes to", "filler phrase"),
    (r"it'?s worth noting", "hedge filler"),
    (r"plays? a (?:crucial|key|vital|significant) role", "importance puffery"),
    (r"stands? as a testament", "importance puffery"),
    (r"marks? a pivotal moment", "importance puffery"),
    (r"solidif(?:y|ies) its position", "importance puffery"),
    (r"underscores? its significance", "importance puffery"),
    (r"what most people get wrong", "faux-insight setup"),
    (r"here'?s what nobody tells you", "faux-insight setup"),
    (r"the part (?:everyone|most people) miss(?:es)?", "faux-insight setup"),
    (r"this is the part most people skip", "faux-insight setup"),
    (r"let me be clear", "throat-clearing opener"),
    (r"i'?ll be honest", "throat-clearing opener"),
    (r"the uncomfortable truth is", "throat-clearing opener"),
    (r"what if i told you", "rhetorical setup"),
    (r"plot twist:", "rhetorical setup"),
    (r"experts agree", "weasel attribution; name the source"),
    (r"studies show", "weasel attribution; name the source"),
    (r"industry reports suggest", "weasel attribution; name the source"),
    (r"(?:many|some) (?:argue|believe|say)\b", "weasel attribution; name the source"),
    (r"widely regarded as", "weasel attribution; name the source"),
    (r"the key point is", "interpretive metadiscourse"),
    (r"this distinction matters", "interpretive metadiscourse"),
    (r"as you can see", "interpretive metadiscourse"),
    (r"matters more than it sounds", "interpretive metadiscourse"),
    (r"in other words", "interpretive metadiscourse"),
    (r"at its core", "empty phrase"),
    (r"in today'?s world", "empty phrase"),
    (r"in the (?:age|world) of", "empty phrase"),
    (r"the (?:reality|truth) is", "empty phrase"),
    (r"going forward", "empty phrase"),
    (r"in this article", "empty phrase"),
    (r"let'?s dive in", "empty phrase"),
    (r"it'?s important to note", "empty phrase"),
    (r"\bultimately,", "summary-recap opener"),
    (r"\boverall,\s", "summary-recap opener"),
    (r",\s(?:highlighting|underscoring|reflecting|showcasing|demonstrating|illustrating)\s",
     "superficial -ing analysis clause"),
    (r"\bnot a\b[^.!?]{0,40}\.\s*not a\b", "negative listing"),
    (r"that'?s it\.\s*that'?s the whole", "dramatic fragmentation"),
    (r"has the ability to", "fake-strong verb; use \"can\""),
    (r"serves as a", "fake-strong verb; use \"is\" or a concrete verb"),
    (r"made a decision", "fake-strong verb; use \"decided\""),
]

GENERIC_HEADINGS = [
    "key takeaways", "why it matters", "benefits", "final thoughts",
    "conclusion", "introduction", "overview", "summary",
]

LATEX_TELLS = [
    (r"(?<!\\)\$\$", "$$ display math; type the equation in Unicode instead"),
    (r"(?<!\\)\$[^$\n]+\$", "$...$ inline math; type the equation in Unicode instead"),
    (r"\\\(|\\\)|\\\[|\\\]", "LaTeX delimiters; type the equation in Unicode instead"),
    (r"```math", "math fence; type the equation in Unicode instead"),
    (
        r"\\(?:frac|dfrac|tfrac|sqrt|sum|prod|int|oint|lim|partial|nabla|infty"
        r"|alpha|beta|gamma|delta|epsilon|varepsilon|zeta|eta|theta|vartheta|iota"
        r"|kappa|lambda|mu|nu|xi|rho|sigma|tau|upsilon|phi|varphi|chi|psi|omega"
        r"|Gamma|Delta|Theta|Lambda|Xi|Pi|Sigma|Phi|Psi|Omega"
        r"|cdot|times|div|pm|mp|leq|geq|neq|approx|equiv|propto|ll|gg"
        r"|in|notin|subset|subseteq|supset|cup|cap|emptyset|forall|exists|neg|land|lor"
        r"|to|rightarrow|leftarrow|Rightarrow|Leftrightarrow|mapsto|therefore"
        r"|mathbb|mathcal|mathrm|mathbf|mathit|operatorname|text|begin|end"
        r"|left|right|quad|qquad|label|ref|eqref|tag|newcommand|usepackage)\b",
        "LaTeX command; type the symbol in Unicode instead",
    ),
]

ASCII_SUBSTITUTES = [
    (r"<=", "\u2264"), (r">=", "\u2265"), (r"!=", "\u2260"), (r"~=", "\u2248"),
    (r"<->", "\u2194"), (r"->", "\u2192"), (r"=>", "\u21d2"), (r"<-", "\u2190"),
    (r"\+/-", "\u00b1"), (r"\bsqrt\s*\(", "\u221a"), (r"\binfinity\b", "\u221e"),
    (r"\bdelta\b", "\u03b4"), (r"\balpha\b", "\u03b1"), (r"\bbeta\b", "\u03b2"),
    (r"\bgamma\b", "\u03b3"), (r"\btheta\b", "\u03b8"), (r"\blambda\b", "\u03bb"),
    (r"\bsigma\b", "\u03c3"), (r"\bepsilon\b", "\u03b5"), (r"\bomega\b", "\u03c9"),
]

SUPERS = "\u2070\u00b9\u00b2\u00b3\u2074\u2075\u2076\u2077\u2078\u2079"
SUBS = "\u2080\u2081\u2082\u2083\u2084\u2085\u2086\u2087\u2088\u2089"


class Report:
    def __init__(self):
        self.items = []

    def add(self, level, line, message):
        self.items.append((level, line, message))

    def errors(self):
        return [i for i in self.items if i[0] == "ERROR"]

    def as_dict(self):
        return {
            "errors": len(self.errors()),
            "warnings": len(self.items) - len(self.errors()),
            "findings": [
                {"level": lvl, "line": line, "message": msg}
                for lvl, line, msg in sorted(self.items, key=lambda i: (i[0] != "ERROR", i[1]))
            ],
        }

    def render(self):
        if not self.items:
            print("Clean. No style violations or math hazards found.")
            return
        order = {"ERROR": 0, "WARN": 1}
        for level, line, message in sorted(self.items, key=lambda i: (order[i[0]], i[1])):
            loc = f"line {line}" if line else "file"
            print(f"{level:5}  {loc:<10}  {message}")
        print()
        print(f"{len(self.errors())} errors, {len(self.items) - len(self.errors())} warnings")


# ------------------------------------------------------------------ masking

def mask_regions(lines):
    """Blank out code fences, inline code, math spans, links, and frontmatter.

    Returns a list of same-length strings so line numbers and offsets survive.
    Style checks run against these; math checks run against the raw lines.
    """
    masked = []
    in_fence = False
    in_frontmatter = False
    in_math_block = False
    for idx, raw in enumerate(lines):
        if idx == 0 and raw.strip() == "---":
            in_frontmatter = True
            masked.append(" " * len(raw))
            continue
        if in_frontmatter:
            if raw.strip() == "---":
                in_frontmatter = False
            masked.append(" " * len(raw))
            continue
        if raw.lstrip().startswith("```"):
            in_fence = not in_fence
            masked.append(" " * len(raw))
            continue
        if in_fence:
            masked.append(" " * len(raw))
            continue
        if raw.strip() == "$$":
            in_math_block = not in_math_block
            masked.append(" " * len(raw))
            continue
        if in_math_block:
            masked.append(" " * len(raw))
            continue

        text = raw
        # display math on one line, inline math, inline code, link targets
        for pattern in (r"\$\$.*?\$\$", r"(?<!\$)\$[^$\n]+\$(?!\$)", r"`[^`\n]+`", r"\]\([^)\n]+\)"):
            text = re.sub(pattern, lambda m: " " * len(m.group(0)), text)
        masked.append(text)
    return masked


# ------------------------------------------------------------- style checks

def check_style(masked, report):
    body = "\n".join(masked)

    for i, text in enumerate(masked, start=1):
        for dash in ("\u2014", "\u2015"):
            if dash in text:
                report.add("ERROR", i, "em dash")
        if ";" in text:
            report.add("ERROR", i, "semicolon")
        if re.search(r"(?<!\S)#[A-Za-z]\w*", text):
            report.add("ERROR", i, "hashtag")
        if re.search(r"\*\*[^*\n]+\*\*", text):
            report.add("ERROR", i, "bold asterisks in body text")
        elif re.search(r"(?<!\*)\*[^*\s][^*\n]*\*(?!\*)", text):
            report.add("WARN", i, "italic asterisks in body text")

        heading = re.match(r"\s{0,3}#{1,6}\s+(.*)", text)
        if heading and heading.group(1).strip().lower().rstrip(":") in GENERIC_HEADINGS:
            report.add("ERROR", i, f"generic heading: {heading.group(1).strip()}")

        low = text.lower()
        for pattern in NEVER:
            for m in re.finditer(rf"\b{pattern}\b", low):
                report.add("ERROR", i, f"banned word: {m.group(0)}")
        for pattern in FILLER:
            for m in re.finditer(rf"\b{pattern}\b", low):
                report.add("WARN", i, f"filler word: {m.group(0)} (cut unless it carries meaning)")

    low_body = body.lower()
    for pattern, label in CONSTRUCTIONS:
        for m in re.finditer(pattern, low_body, re.DOTALL):
            line = low_body[: m.start()].count("\n") + 1
            report.add("ERROR", line, f"banned construction: {label}")

    # Colon reveal: a capitalized noun phrase, a colon, then a lowercase reveal.
    # WARN rather than ERROR because legitimate label lines share the shape.
    for i, text in enumerate(masked, start=1):
        if re.match(r"\s*[-*#>|]", text) or "](" in text:
            continue
        if re.match(r"[A-Z][^.:!?\n]{8,70}:\s+[a-z]", text.strip()):
            report.add("WARN", i, "possible colon reveal; rewrite as a plain sentence")

    check_rhythm(masked, report)
    check_paragraph_openers(masked, report)


def sentences_with_lines(masked):
    """Split into sentences, joining hard-wrapped lines first.

    Without the join, a sentence wrapped across two lines reads as two short
    sentences and the rhythm check fires on nothing.
    """
    paragraphs = []
    buf, start = [], None
    for i, text in enumerate(masked, start=1):
        stripped = text.strip()
        skip = (
            not stripped
            or stripped.startswith(("#", ">", "|", "-", "*", "$"))
            or re.match(r"\d+\.", stripped)
        )
        if skip:
            if buf:
                paragraphs.append((start, " ".join(buf)))
                buf, start = [], None
            continue
        if start is None:
            start = i
        buf.append(stripped)
    if buf:
        paragraphs.append((start, " ".join(buf)))

    out = []
    for line_no, para in paragraphs:
        for part in re.split(r"(?<=[.!?])\s+", para):
            words = re.findall(r"[A-Za-z0-9'\-]+", part)
            if len(words) >= 3:
                out.append((line_no, len(words), part))
    return out


def check_rhythm(masked, report):
    sents = sentences_with_lines(masked)
    if len(sents) < 3:
        return
    for j in range(len(sents) - 2):
        window = sents[j : j + 3]
        lengths = [w[1] for w in window]
        if max(lengths) - min(lengths) <= 2:
            report.add(
                "WARN",
                window[0][0],
                f"three consecutive sentences of similar length ({lengths}); vary one",
            )
        first_words = [re.findall(r"[A-Za-z']+", w[2])[:1] for w in window]
        heads = [f[0].lower() for f in first_words if f]
        if len(heads) == 3 and len(set(heads)) == 1:
            report.add("ERROR", window[0][0], f"three sentences in a row open with \"{heads[0]}\"")
    all_lengths = [s[1] for s in sents]
    if len(all_lengths) >= 6 and statistics.pstdev(all_lengths) < 4:
        report.add(
            "WARN", 0,
            f"sentence length variance is low across the file (stdev {statistics.pstdev(all_lengths):.1f})",
        )


def check_paragraph_openers(masked, report):
    openers = []
    prev_blank = True
    for i, text in enumerate(masked, start=1):
        stripped = text.strip()
        if not stripped:
            prev_blank = True
            continue
        if prev_blank and not stripped.startswith(("#", ">", "|", "$")):
            first = re.findall(r"[A-Za-z']+", stripped)
            if first:
                openers.append((i, first[0].lower()))
        prev_blank = False
    for j in range(len(openers) - 1):
        if openers[j][1] == openers[j + 1][1]:
            report.add("WARN", openers[j + 1][0], f"paragraph opens with the same word: \"{openers[j][1]}\"")


# -------------------------------------------------------------- math checks

BAD_LATEX = [
    (r"\\label\b", "\\label has no effect; number equations in prose"),
    (r"\\eqref\b", "\\eqref does not render"),
    (r"\\ref\b", "\\ref does not render"),
    (r"\\begin\{equation\}", "use $$ ... $$ instead of the equation environment"),
    (r"\\begin\{align\}", "use \\begin{aligned} inside $$ ... $$"),
    (r"\\newcommand\b", "custom macros are stripped by GitHub"),
    (r"\\def\b", "custom macros are stripped by GitHub"),
    (r"\\usepackage\b", "not valid in markdown math"),
    (r"\\tag\b", "equation tags render inconsistently"),
    (r"\\require\{", "MathJax-only, breaks in VS Code"),
    (r"\\textcolor\b", "color support is unreliable"),
    (r"\\substack\b", "patchy KaTeX support"),
]

ASCII_SUBSTITUTES = [
    (r"<=", "\u2264"), (r">=", "\u2265"), (r"!=", "\u2260"),
    (r"<->", "\u2194"), (r"->", "\u2192"), (r"=>", "\u21d2"),
    (r"\+/-", "\u00b1"), (r"\binfinity\b", "\u221e"),
]

SUPERS = "\u2070\u00b9\u00b2\u00b3\u2074\u2075\u2076\u2077\u2078\u2079"
SUBS = "\u2080\u2081\u2082\u2083\u2084\u2085\u2086\u2087\u2088\u2089"


def check_math(lines, masked, report):
    """Check LaTeX correctness, and prefer Unicode for lone inline symbols."""
    in_fence = in_block = False
    block_start = 0
    fence_start = 0
    fence_math = False

    for i, raw in enumerate(lines, start=1):
        if raw.lstrip().startswith("```"):
            if in_fence and fence_math:
                report.add(
                    "ERROR", fence_start,
                    "LaTeX inside a code fence renders as grey code, not an equation; "
                    "remove the fence",
                )
            in_fence = not in_fence
            fence_start, fence_math = i, False
            continue
        if in_fence:
            if re.search(r"\$\$|(?<!\\)\$[^$\n]+\$|\\begin\{aligned\}|\\frac|\\sum_", raw):
                fence_math = True
            continue

        if re.search(r"\\\(|\\\)", raw):
            report.add("ERROR", i, r"\( \) delimiters do not render; use $...$")
        if re.search(r"\\\[|\\\]", raw):
            report.add("ERROR", i, r"\[ \] delimiters do not render; use $$...$$")
        for pattern, why in BAD_LATEX:
            if re.search(pattern, raw):
                report.add("ERROR", i, why)

        stripped = raw.strip()
        if stripped == "$$":
            if not in_block:
                block_start = i
                if i > 1 and lines[i - 2].strip():
                    report.add("ERROR", i, "no blank line before $$ block")
            else:
                if i < len(lines) and lines[i].strip():
                    report.add("ERROR", i, "no blank line after $$ block")
            in_block = not in_block
            continue
        if in_block:
            continue

        if re.match(r"^\s*\$\$.+\$\$\s*$", raw):
            report.add("WARN", i, "put $$ on its own line above and below the equation")
            continue

        # Inline code spans hold literal $ (shell vars, prices) that are not math.
        outside_code = re.sub(r"`[^`\n]*`", "", raw)
        dollars = re.findall(r"(?<!\\)\$", outside_code)
        if len(dollars) % 2 == 1:
            report.add("ERROR", i, "odd number of unescaped $; escape currency as \\$")

        for m in re.finditer(r"(?<!\$)\$([^$\n]+)\$(?!\$)", outside_code):
            inner = m.group(1)
            prose_like = (
                inner.count(" ") >= 3
                and not re.search(r"[\\^_=<>]", inner)
                and re.match(r"^\d", inner.strip())
            )
            if prose_like:
                report.add("ERROR", i, "two currency amounts read as a math span; escape as \\$")
            elif inner != inner.strip():
                report.add("ERROR", i, "space just inside $...$ breaks GitHub math")
            if inner.count("{") != inner.count("}"):
                report.add("ERROR", i, "unbalanced braces inside inline math")
            if "\\\\" in inner:
                report.add("WARN", i, "line break inside inline math; move to a $$ block")

    if in_block:
        report.add("ERROR", block_start, "unclosed $$ display block")

    # Unicode preferences, checked outside code, links, and math spans
    for i, text in enumerate(masked, start=1):
        for pattern, symbol in ASCII_SUBSTITUTES:
            if re.search(pattern, text):
                report.add("WARN", i, f"ASCII stand-in; use {symbol} inline")
        for m in re.finditer(r"[A-Za-z\u03b1-\u03c9]\^-?[0-9a-pr-z](?![A-Za-z0-9])", text):
            report.add("WARN", i, f"caret superscript in {m.group(0)!r}; use {SUPERS} or $...$")
        for m in re.finditer(r"[A-Za-z\u03b1-\u03c9]_[0-9aehijklmnoprstuvx](?![A-Za-z0-9])", text):
            report.add("WARN", i, f"underscore subscript in {m.group(0)!r}; use {SUBS} or $...$")


# -------------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--style-only", action="store_true")
    parser.add_argument("--math-only", action="store_true")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()

    path = args.path.expanduser().resolve()
    if not path.exists():
        print(f"No such file: {path}", file=sys.stderr)
        return 2
    if not path.is_file():
        print(f"Not a regular file: {path}", file=sys.stderr)
        return 2

    try:
        lines = path.read_text(encoding="utf-8").split("\n")
    except UnicodeDecodeError:
        print(f"Not UTF-8 text: {path}", file=sys.stderr)
        return 2
    except PermissionError:
        print(f"Permission denied: {path}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"Could not read {path}: {exc}", file=sys.stderr)
        return 2
    report = Report()

    masked = mask_regions(lines)
    if not args.math_only:
        check_style(masked, report)
    if not args.style_only:
        check_math(lines, masked, report)

    if args.json:
        print(json.dumps(report.as_dict(), indent=2))
    else:
        report.render()
    return 1 if report.errors() else 0


if __name__ == "__main__":
    sys.exit(main())
