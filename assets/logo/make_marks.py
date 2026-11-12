"""Writes the CarMind model marks (assets/logo/carmind-*.svg) from one geometry, so every size stays in step.

The letters are drawn on a 100-unit cap height: an R with a stem, a D-shaped bowl and a straight leg; an E made of
three bars with the amber chip on the short middle one, its top bar running into the V; a V with a flat-cut foot.
CarMind E.V stands upright (fixed). CarMind R.E.V leans forward 10 degrees (it learns).

Two sizes of each mark:
  carmind-<model>.svg        the full mark, for 64 px and up (README cards, the site, the brand sheet)
  carmind-<model>-small.svg  letters filling the tile and a heavier rim, for 16 to 63 px (figures, tags, GIFs, tables),
                             where the full mark's letters would be only a few pixels tall

usage: python3 assets/logo/make_marks.py   (from the repository root)
"""
from pathlib import Path

R = "M0 0H22V100H0Z M0 0H40A30.5 30.5 0 0 1 40 61H0Z M22 22V39H40A8.5 8.5 0 0 0 40 22Z M28 55H58L76.9 100H46.9Z"
EV = "M0 0H100V22H0Z M0 39H44.4V61H0Z M0 78H92.8L102 100H0Z M70 0H100L127 64.3L154 0H184L142 100H112Z"
CHIP = "#F59A2E"
TILE = '<rect x="16" y="16" width="480" height="480" rx="108" fill="{bg}"/>'

# name: (label, tile colour, letter colour, rim, leaning R.E.V, transform for the full mark, transform for the small mark)
# Small transforms put the letters at about 80 % (R.E.V) and 70 % (E.V) of the tile width, centred on their ink.
MARKS = {
    "carmind-rev": ("CarMind R.E.V", "#15202D", "#F2ECE1", "#34465C", True,
                    "translate(106.5 197.7) scale(1.166) skewX(-10)", "translate(75.7 185.7) scale(1.406) skewX(-10)"),
    "carmind-rev-old": ("CarMind R.E.V (old)", "#0E8C6C", "#FFFFFF", None, True,
                        "translate(106.5 197.7) scale(1.166) skewX(-10)", "translate(75.7 185.7) scale(1.406) skewX(-10)"),
    "carmind-ev": ("CarMind E.V", "#2F5FD0", "#F2ECE1", None, False,
                   "translate(118 181) scale(1.5)", "translate(76.6 158.5) scale(1.95)"),
}


def rim(colour, width):
    if colour:
        return f'<rect x="17.5" y="17.5" width="477" height="477" rx="106.5" fill="none" stroke="{colour}" stroke-width="{width}"/>'
    return f'<rect x="17.5" y="17.5" width="477" height="477" rx="106.5" fill="none" stroke="#FFFFFF" stroke-opacity="0.16" stroke-width="{width}"/>'


def mark(label, bg, ink, rim_colour, rev, transform, rim_width):
    letters = (f'    <path d="{R}"/>\n    <path transform="translate(90 0)" d="{EV}"/>\n'
               f'    <rect x="144.4" y="39" width="22" height="22" rx="3" fill="{CHIP}"/>') if rev else (
              f'    <path d="{EV}"/>\n    <rect x="54.4" y="39" width="22" height="22" rx="3" fill="{CHIP}"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" role="img" aria-label="{label}">\n'
            f'  <title>{label}</title>\n  {TILE.format(bg=bg)}{rim(rim_colour, rim_width)}\n'
            f'  <g transform="{transform}" fill="{ink}">\n{letters}\n  </g>\n</svg>\n')


if __name__ == "__main__":
    here = Path(__file__).resolve().parent
    for name, (label, bg, ink, rim_colour, rev, full, small) in MARKS.items():
        (here / f"{name}.svg").write_text(mark(label, bg, ink, rim_colour, rev, full, 3))
        (here / f"{name}-small.svg").write_text(mark(label, bg, ink, rim_colour, rev, small, 10))
        print("wrote", name, "and", name + "-small")
