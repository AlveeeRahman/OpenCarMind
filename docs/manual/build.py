"""Build the user manuals from one source each, so the web page and the Markdown cannot drift.

    python3 docs/manual/build.py            write the pages and the Markdown
    python3 docs/manual/build.py --check    exit 1 if a written file differs from a fresh build

Source: src/<name>.src.html (plus src/shared.css and src/shared.js). Output, next to this script:
    <name>.html   the brand page for the GitHub Pages site (docs/ is served at alveeerahman.github.io/OpenCarMind/)
    <name>.md     the same text as Markdown that GitHub renders

Images stay files, referenced by relative paths (img/... for the Mosaico captures, ../images/... for the S3 renders).
The source uses a small set of elements and classes. Anything else stops the build, so nothing is dropped quietly.
"""
import html
import pathlib
import re
import sys
from html.parser import HTMLParser

HERE = pathlib.Path(__file__).resolve().parent
SRC = HERE / "src"
MANUALS = {"edgeos-2.0.0": 240, "esp32-s3": 200}   # name: image width in the Markdown, in px

FORBIDDEN = ("\u2014", "/Users/", "/private/tmp", "/home/", "/_blob/", "{{", "x-dc")   # em dash, local paths, template leftovers
VOID = {"img", "meta", "link", "br", "hr", "input"}
SKIP = {"title", "meta", "link", "style", "script"}


class Node:
    def __init__(self, tag, attrs=None, parent=None):
        self.tag, self.attrs, self.parent, self.kids = tag, dict(attrs or {}), parent, []

    def cls(self):
        return self.attrs.get("class", "").split()

    def has(self, c):
        return c in self.cls()

    def find(self, pred):
        for k in self.kids:
            if isinstance(k, Node):
                if pred(k):
                    return k
                hit = k.find(pred)
                if hit:
                    return hit
        return None


class Comment(str):
    pass


class Tree(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = self.cur = Node("root")

    def handle_starttag(self, tag, attrs):
        n = Node(tag, attrs, self.cur)
        self.cur.kids.append(n)
        if tag not in VOID:
            self.cur = n

    def handle_startendtag(self, tag, attrs):
        self.cur.kids.append(Node(tag, attrs, self.cur))

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        n = self.cur
        while n is not self.root and n.tag != tag:
            n = n.parent
        if n is self.root:
            sys.exit(f"stray </{tag}>")
        self.cur = n.parent

    def handle_data(self, data):
        self.cur.kids.append(data)

    def handle_comment(self, data):
        self.cur.kids.append(Comment(data))


def parse(text):
    t = Tree()
    t.feed(text)
    t.close()
    return t.root


def lint(name, node, in_code=False):
    """No semicolons in prose (the house style), and nothing from the forbidden list anywhere."""
    for k in node.kids:
        if isinstance(k, Node):
            lint(name, k, in_code or k.tag in ("code", "pre", "style", "script", "title"))
        elif not isinstance(k, Comment) and not in_code and ";" in k:
            sys.exit(f"{name}: semicolon in prose: {k.strip()[:80]!r}")


# ---------- Markdown ----------
def esc(s):
    s = re.sub(r"\s+", " ", s)
    return s.replace("\\", "\\\\").replace("*", "\\*").replace("_", "\\_").replace("<", "&lt;").replace("|", "\\|")


def text_of(n):
    return "".join(k if isinstance(k, str) and not isinstance(k, Comment) else text_of(k) if isinstance(k, Node) else ""
                   for k in n.kids)


def inline(n):
    out = []
    for k in n.kids:
        if isinstance(k, Comment):
            out.append(f"<!--{k}-->")
        elif isinstance(k, str):
            out.append(esc(k))
        elif k.tag in ("strong", "b"):
            out.append(f"**{inline(k).strip()}**")
        elif k.tag == "em":
            out.append(f"*{inline(k).strip()}*")
        elif k.tag == "code":
            t = text_of(k)
            out.append(f"``{t}``" if "`" in t else f"`{t}`")
        elif k.tag == "a":
            out.append(f"[{inline(k).strip()}]({k.attrs['href']})")
        elif k.tag == "br":
            out.append("<br>")
        elif k.tag == "span" and k.has("coming"):
            out.append(f"*{inline(k).strip()}.* ")
        elif k.tag == "span" and k.has("n"):
            continue
        elif k.tag == "span":
            out.append(inline(k))
        else:
            sys.exit(f"inline: unexpected <{k.tag} class={k.attrs.get('class')}>")
    return re.sub(r" {2,}", " ", "".join(out))


INLINE_TAGS = {"strong", "b", "em", "code", "a", "br", "span"}


def flow(n, ctx):
    """Children of a list item or a txt block: loose inline content becomes paragraphs."""
    blocks, buf = [], []

    def flush():
        t = "".join(buf).strip()
        if t:
            blocks.append(t)
        buf.clear()

    for k in n.kids:
        if isinstance(k, Comment) or isinstance(k, str) or k.tag in INLINE_TAGS:
            tmp = Node("span")
            tmp.kids = [k]
            buf.append(inline(tmp))
        else:
            flush()
            blocks.extend(block(k, ctx))
    flush()
    return blocks


def indent(blocks, first, rest):
    lines = "\n\n".join(blocks).split("\n")
    return "\n".join([first + lines[0]] + [(rest + ln) if ln else "" for ln in lines[1:]])


def figure_md(fig, ctx):
    img = fig.find(lambda x: x.tag == "img")
    cap = fig.find(lambda x: x.tag == "figcaption")
    capt = inline(cap).strip() if cap else ""
    if img is None:
        return None, f"[Capture coming] {capt}"
    alt = html.escape(img.attrs.get("alt", ""), quote=True)
    return f'<img src="{img.attrs["src"]}" width="{ctx["w"]}" alt="{alt}">', capt


def figures(figs, ctx):
    pairs = [figure_md(f, ctx) for f in figs]
    imgs = " ".join(p[0] for p in pairs if p[0])
    if len(pairs) == 1:
        caps = f"*{pairs[0][1]}*" if pairs[0][1] else ""
    else:
        names = ["Left", "Right"] if len(pairs) == 2 else [str(i + 1) for i in range(len(pairs))]
        caps = " ".join(f"*{nm}:* {c}" for nm, (_, c) in zip(names, pairs))
    return [b for b in (imgs and f"<p>{imgs}</p>", caps) if b]


def alert(kind, n, ctx):
    inner = []
    for k in n.kids:
        if isinstance(k, Node) and k.tag == "h3":
            inner.append(f"**{inline(k).strip()}**")
        elif isinstance(k, Node) and k.has("cgrid"):
            for x in (x for d in k.kids if isinstance(d, Node) for x in d.kids if isinstance(x, Node)):
                inner.extend([f"**{inline(x).strip()}**"] if x.tag == "h3" else block(x, ctx))
        elif isinstance(k, Node):
            inner.extend(block(k, ctx))
        elif isinstance(k, Comment):
            inner.append(f"<!--{k}-->")
    body = "\n>\n".join("\n".join("> " + ln if ln else ">" for ln in b.split("\n")) for b in inner)
    return [f"> [!{kind}]\n{body}"]


def block(n, ctx):
    if isinstance(n, Comment):
        return [f"<!--{n}-->"]
    if isinstance(n, str):
        t = esc(n).strip()
        return [t] if t else []
    tag = n.tag
    if tag in SKIP or n.has("zoom"):
        return []
    if tag == "header":
        h1 = n.find(lambda x: x.tag == "h1")
        top = [k for k in n.find(lambda x: x.has("top")).kids if isinstance(k, Node)][1]
        kick = n.find(lambda x: x.has("kick"))
        lead = n.find(lambda x: x.has("lead"))
        facts = n.find(lambda x: x.has("facts"))
        out = [f"# {inline(h1).strip()}", f"*{inline(top).strip()}. {inline(kick).strip()}.*", inline(lead).strip()]
        rows = []
        for f in facts.kids:
            if isinstance(f, Node):
                s, v = [k for k in f.kids if isinstance(k, Node)]
                rows.append(f"- **{inline(s).strip()}:** {inline(v).strip()}")
        return out + ["\n".join(rows)]
    if tag == "nav":
        items = [li for li in n.find(lambda x: x.tag == "ol").kids if isinstance(li, Node)]
        lines = []
        for i, li in enumerate(items, 1):
            a = li.find(lambda x: x.tag == "a")
            lines.append(f"{i}. [{inline(a).strip()}]({a.attrs['href']})")
        return ["## Contents", "\n".join(lines)]
    if tag == "section":
        out = [f'<a id="{n.attrs["id"]}"></a>']
        for k in n.kids:
            out.extend(block(k, ctx))
        return out
    if tag == "main":
        return [b for k in n.kids for b in block(k, ctx)]
    if tag == "footer":
        spans = [inline(s).strip() for s in n.find(lambda x: x.has("wrap")).kids if isinstance(s, Node)]
        return ["---", " · ".join(spans)]
    if tag == "p" and n.has("eye"):
        return []
    if tag == "h2":
        return [f"## {inline(n).strip()}"]
    if tag == "h3":
        return [f"### {inline(n).strip()}"]
    if tag == "p":
        t = inline(n).strip()
        return [t] if t else []
    if tag == "div" and n.has("callout"):
        return alert("NOTE", n, ctx)
    if tag == "div" and n.has("warn"):
        return alert("WARNING", n, ctx)
    if tag == "div" and n.has("app"):
        figs = []
        rest = []
        for k in n.kids:
            if isinstance(k, Node) and k.tag == "figure":
                figs.append(k)
            elif isinstance(k, Node) and k.has("figs"):
                figs.extend(f for f in k.kids if isinstance(f, Node) and f.tag == "figure")
            else:
                rest.append(k)
        # the app's own h3 first, then its captures, then the rest of its text
        txt = next((k for k in rest if isinstance(k, Node) and k.has("txt")), None)
        head = txt.find(lambda x: x.tag == "h3") if txt else None
        if head is not None and head.parent is not txt:
            head = None
        out = block(head, ctx) if head is not None else []
        out.extend(figures(figs, ctx))
        for k in rest:
            kids = [kk for kk in k.kids if kk is not head] if k is txt else [k]
            for kk in kids:
                out.extend(block(kk, ctx))
        return out
    if tag == "div" and n.has("figs"):
        return figures([f for f in n.kids if isinstance(f, Node) and f.tag == "figure"], ctx)
    if tag == "figure":
        return figures([n], ctx)
    if tag == "div" and n.has("key"):
        strong = n.find(lambda x: x.tag == "strong")
        body = " ".join(inline(p).strip() for p in n.kids if isinstance(p, Node) and p.tag == "p")
        return [f"**{inline(strong).strip()}.** {body}"]
    if tag == "div" and (set(n.cls()) & {"grid2", "grid3", "card", "txt", "keys", "cgrid"} or not n.cls()):
        kids = [k for k in n.kids if not (isinstance(k, str) and not isinstance(k, Comment) and not k.strip())]
        out = []
        for i, k in enumerate(kids):
            if isinstance(k, Node) and k.tag == "span" and k.has("coming"):
                continue                                   # a status tag goes after the heading it sits above
            out.extend(block(k, ctx))
            prev = kids[i - 1] if i else None
            if isinstance(prev, Node) and prev.tag == "span" and prev.has("coming"):
                out.append(f"*{inline(prev).strip()}.*")
        return out
    if tag == "dl":
        rows, dt = [], None
        for k in n.kids:
            if isinstance(k, Node) and k.tag == "dt":
                dt = inline(k).strip()
            elif isinstance(k, Node) and k.tag == "dd":
                rows.append(f"- **{dt}:** {inline(k).strip()}")
        return ["\n".join(rows)]
    if tag in ("ol", "ul"):
        items = [li for li in n.kids if isinstance(li, Node) and li.tag == "li"]
        out = []
        for i, li in enumerate(items, 1):
            body = li.kids[0] if len([k for k in li.kids if isinstance(k, Node)]) == 1 and isinstance(li.kids[0], Node) \
                and li.kids[0].tag == "div" else li
            mark = f"{i}. " if tag == "ol" else "- "
            out.append(indent(flow(body, ctx), mark, " " * len(mark)))
        return ["\n".join(out)]
    if tag == "pre":
        return ["```sh\n" + text_of(n).strip("\n") + "\n```"]
    if tag == "div" and n.has("tbl"):
        table = n.find(lambda x: x.tag == "table")
        head = [inline(th).strip() for th in table.find(lambda x: x.tag == "thead").find(lambda x: x.tag == "tr").kids
                if isinstance(th, Node)]
        rows = [[inline(td).strip() for td in tr.kids if isinstance(td, Node)]
                for tr in table.find(lambda x: x.tag == "tbody").kids if isinstance(tr, Node)]
        lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] + ["| " + " | ".join(r) + " |" for r in rows]
        return ["\n".join(lines)]
    if tag == "div" and n.has("ph"):
        return []
    sys.exit(f"block: unexpected <{tag} class={n.attrs.get('class')}>")


def to_markdown(root, width, name):
    blocks = [b for k in root.kids for b in block(k, {"w": width})]
    head = (f"<!-- Generated by docs/manual/build.py from docs/manual/src/{name}.src.html. "
            "Edit the source and run the script, not this file. -->")
    md = "\n\n".join([head] + [b.rstrip() for b in blocks if b and b.strip()]) + "\n"
    return re.sub(r"\n{3,}", "\n\n", md)


# ---------- HTML ----------
PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<!-- Generated by docs/manual/build.py from docs/manual/src/@@NAME@@.src.html. Edit the source and run the script. -->
@@HEAD@@
</head>
<body>
@@BODY@@
</body>
</html>
"""


def to_html(src, name):
    css = (SRC / "shared.css").read_text()
    js = (SRC / "shared.js").read_text()
    cut = src.index("<header")
    head, body = src[:cut].strip(), src[cut:].strip()
    head = head.replace("/*@shared*/", css.strip())
    body = body.replace("<!--@script-->", js.strip())
    return PAGE.replace("@@NAME@@", name).replace("@@HEAD@@", head).replace("@@BODY@@", body)


def check_images(name, root):
    missing = []

    def walk(n):
        for k in n.kids:
            if isinstance(k, Node):
                if k.tag == "img" and "src" in k.attrs and not k.attrs["src"].startswith("data:"):
                    if not (HERE / k.attrs["src"]).is_file():
                        missing.append(k.attrs["src"])
                walk(k)
    walk(root)
    if missing:
        sys.exit(f"{name}: missing images {missing}")


def used_images(root):
    found = set()

    def walk(n):
        for k in n.kids:
            if isinstance(k, Node):
                if k.tag == "img" and k.attrs.get("src", "").startswith("img/"):
                    found.add(k.attrs["src"][4:])
                walk(k)
    walk(root)
    return found


def build():
    out, used = {}, set()
    for name, width in MANUALS.items():
        src = (SRC / f"{name}.src.html").read_text()
        for bad in FORBIDDEN:
            if bad in src:
                sys.exit(f"{name}: found {bad!r}")
        root = parse(src)
        lint(name, root)
        check_images(name, root)
        used |= used_images(root)
        out[HERE / f"{name}.html"] = to_html(src, name)
        out[HERE / f"{name}.md"] = to_markdown(root, width, name)
    unused = sorted(p.name for p in (HERE / "img").glob("*.png") if p.name not in used)
    if unused:
        sys.exit(f"img/ has images no manual uses: {unused}")
    return out


def main(argv):
    out = build()
    if "--check" in argv:
        stale = [p.name for p, t in out.items() if not p.is_file() or p.read_text() != t]
        if stale:
            sys.exit(f"stale, run docs/manual/build.py: {stale}")
        print("manuals up to date")
        return
    for p, t in out.items():
        p.write_text(t)
        print(f"wrote {p.relative_to(HERE.parent.parent)} ({len(t.encode())} bytes)")


if __name__ == "__main__":
    main(sys.argv[1:])
