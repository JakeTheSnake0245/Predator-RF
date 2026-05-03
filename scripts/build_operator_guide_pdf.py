#!/usr/bin/env python3
"""Render docs/OPERATOR_GUIDE.md to a PDF with the Predator RF icon at the top.

Pure-Python: fpdf2 + mistune. No system libs required.
Output: docs/Predator_RF_Operator_Guide.pdf
"""
import pathlib, sys
import mistune
from fpdf import FPDF

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC  = ROOT / "docs" / "OPERATOR_GUIDE.md"
ICON = ROOT / "root" / "res" / "icons" / "sdrpp.png"
OUT  = ROOT / "docs" / "Predator_RF_Operator_Guide.pdf"

RED = (178, 34, 34)
DARK = (26, 26, 26)
GREY = (110, 110, 110)
TH_BG = (241, 214, 214)
ROW_ALT = (250, 250, 250)
CODE_BG = (240, 240, 240)
PRE_BG = (30, 30, 30)
PRE_FG = (232, 232, 232)


FONT_DIR = "/usr/share/fonts/truetype/dejavu"
SANS = "dejavu"
MONO = "dejavumono"


class Guide(FPDF):
    def __init__(self):
        super().__init__(orientation="P", unit="mm", format="Letter")
        self.set_margins(left=16, top=18, right=16)
        self.set_auto_page_break(auto=True, margin=20)
        self.alias_nb_pages()
        # Register Unicode fonts so em-dashes, bullets, arrows etc. render.
        self.add_font(SANS, "",   f"{FONT_DIR}/DejaVuSans.ttf")
        self.add_font(SANS, "B",  f"{FONT_DIR}/DejaVuSans-Bold.ttf")
        # No DejaVuSans-Oblique on box: fall back italic → regular (still readable).
        self.add_font(SANS, "I",  f"{FONT_DIR}/DejaVuSans.ttf")
        self.add_font(SANS, "BI", f"{FONT_DIR}/DejaVuSans-Bold.ttf")
        self.add_font(MONO, "",   f"{FONT_DIR}/DejaVuSansMono.ttf")
        self.add_font(MONO, "B",  f"{FONT_DIR}/DejaVuSansMono-Bold.ttf")
        self.add_font(MONO, "I",  f"{FONT_DIR}/DejaVuSansMono-Oblique.ttf")
        self.add_font(MONO, "BI", f"{FONT_DIR}/DejaVuSansMono-BoldOblique.ttf")

    def footer(self):
        self.set_y(-14)
        self.set_font(SANS, "", 8)
        self.set_text_color(*GREY)
        self.cell(0, 5, f"Predator RF — Operator Guide   |   Page {self.page_no()} of {{nb}}",
                  align="C")


pdf = Guide()
pdf.add_page()

# ---------- Cover banner ----------
icon_w = 24
top_y = pdf.get_y()
pdf.image(str(ICON), x=pdf.l_margin, y=top_y, w=icon_w, h=icon_w)
text_x = pdf.l_margin + icon_w + 6
pdf.set_xy(text_x, top_y + 1)
pdf.set_text_color(*RED)
pdf.set_font(SANS, "B", 24)
pdf.cell(0, 10, "Predator RF", new_x="LMARGIN", new_y="NEXT")
pdf.set_x(text_x)
pdf.set_text_color(*DARK)
pdf.set_font(SANS, "", 14)
pdf.cell(0, 7, "Operator Guide", new_x="LMARGIN", new_y="NEXT")
pdf.set_x(text_x)
pdf.set_text_color(*GREY)
pdf.set_font(SANS, "I", 9)
pdf.cell(0, 5, "Pick this up cold. Zero to a working RF picture in under 30 minutes.",
         new_x="LMARGIN", new_y="NEXT")

pdf.ln(4)
pdf.set_draw_color(*RED)
pdf.set_line_width(0.6)
pdf.line(pdf.l_margin, pdf.get_y(), pdf.w - pdf.r_margin, pdf.get_y())
pdf.ln(4)
pdf.set_draw_color(180, 180, 180)
pdf.set_line_width(0.2)
pdf.set_text_color(*DARK)


# ---------- Markdown -> tokens ----------
md_text = SRC.read_text(encoding="utf-8")
# Strip the H1 + tagline (already in cover banner)
lines = md_text.splitlines()
if lines and lines[0].startswith("# "):
    lines = lines[1:]
    while lines and not lines[0].strip():
        lines.pop(0)
    if lines and lines[0].startswith("**Pick this up cold"):
        lines = lines[1:]
md_body = "\n".join(lines)

parse = mistune.create_markdown(renderer=None, plugins=["table", "strikethrough"])
tokens = parse(md_body)


# ---------- Inline rendering ----------
def inline_segments(children):
    """Flatten inline tokens into list of (text, style, mono, link)."""
    out = []
    def walk(nodes, style="", mono=False, link=None):
        for n in nodes:
            t = n["type"]
            if t == "text":
                out.append((n["raw"], style, mono, link))
            elif t == "strong":
                walk(n["children"], style + "B", mono, link)
            elif t == "emphasis":
                walk(n["children"], style + "I", mono, link)
            elif t == "codespan":
                out.append((n["raw"], style, True, link))
            elif t == "link":
                walk(n["children"], style, mono, n.get("attrs", {}).get("url"))
            elif t == "linebreak" or t == "softbreak":
                out.append((" ", style, mono, link))
            elif t == "image":
                pass
            elif "children" in n:
                walk(n["children"], style, mono, link)
            elif "raw" in n:
                out.append((n["raw"], style, mono, link))
    walk(children)
    return out


def write_inline(segments, size=10.5, line_h=5.2, base_style=""):
    pdf.set_text_color(*DARK)
    for text, style, mono, link in segments:
        full_style = (base_style + style)
        family = MONO if mono else SANS
        clean_style = "".join(sorted(set(c for c in full_style if c in "BI")))
        pdf.set_font(family, clean_style, size)
        if mono:
            # render with light grey background highlight
            w = pdf.get_string_width(text) + 1.5
            x, y = pdf.get_x(), pdf.get_y()
            pdf.set_fill_color(*CODE_BG)
            # use write so we get wrapping; skip background highlight on wrap
            pdf.write(line_h, text, link=link or "")
        else:
            pdf.write(line_h, text, link=link or "")
    pdf.ln(line_h)


def render_paragraph(node, size=10.5, line_h=5.2, base_style=""):
    segs = inline_segments(node.get("children", []))
    write_inline(segs, size=size, line_h=line_h, base_style=base_style)
    pdf.ln(1.5)


def render_heading(node):
    level = node["attrs"]["level"]
    segs = inline_segments(node["children"])
    text = "".join(s[0] for s in segs)
    pdf.ln(2)
    if level == 1:
        pdf.set_font(SANS, "B", 16)
        pdf.set_text_color(*RED)
        pdf.cell(0, 9, text, new_x="LMARGIN", new_y="NEXT")
        pdf.set_draw_color(220, 220, 220)
        pdf.line(pdf.l_margin, pdf.get_y(), pdf.w - pdf.r_margin, pdf.get_y())
        pdf.ln(2)
    elif level == 2:
        pdf.set_font(SANS, "B", 13)
        pdf.set_text_color(*DARK)
        pdf.cell(0, 7, text, new_x="LMARGIN", new_y="NEXT")
        pdf.set_draw_color(210, 210, 210)
        pdf.line(pdf.l_margin, pdf.get_y(), pdf.w - pdf.r_margin, pdf.get_y())
        pdf.ln(1.5)
    else:
        pdf.set_font(SANS, "B", 11.5)
        pdf.set_text_color(60, 60, 60)
        pdf.cell(0, 6, text, new_x="LMARGIN", new_y="NEXT")
        pdf.ln(0.5)
    pdf.set_text_color(*DARK)


def render_list(node):
    ordered = node.get("attrs", {}).get("ordered", False)
    items = [c for c in node["children"] if c["type"] == "list_item"]
    indent = 6
    pdf.set_x(pdf.l_margin + 2)
    counter = node.get("attrs", {}).get("start") or 1
    for i, item in enumerate(items):
        bullet = f"{counter+i}." if ordered else "•"
        pdf.set_font(SANS, "B", 10.5)
        pdf.set_text_color(*RED if not ordered else DARK)
        pdf.set_x(pdf.l_margin + 2)
        pdf.cell(indent, 5.2, bullet)
        pdf.set_text_color(*DARK)
        # render item children inline (paragraphs / blocks)
        x_after = pdf.get_x()
        # save left margin temporarily
        old_lm = pdf.l_margin
        pdf.set_left_margin(old_lm + 2 + indent)
        first = True
        for c in item["children"]:
            if c["type"] == "block_text" or c["type"] == "paragraph":
                if not first:
                    pdf.set_x(pdf.l_margin)
                segs = inline_segments(c.get("children", []))
                write_inline(segs)
                first = False
            elif c["type"] == "list":
                pdf.set_left_margin(pdf.l_margin + 4)
                render_list(c)
                pdf.set_left_margin(old_lm + 2 + indent)
            else:
                render_block(c)
        pdf.set_left_margin(old_lm)
    pdf.ln(1.5)


def render_table(node):
    head = None
    body_rows = []
    for c in node["children"]:
        if c["type"] == "table_head":
            head = [inline_segments(cell.get("children", []))
                    for cell in c["children"]]
        elif c["type"] == "table_body":
            for row in c["children"]:
                body_rows.append([inline_segments(cell.get("children", []))
                                  for cell in row["children"]])
    if not head:
        return
    avail = pdf.w - pdf.l_margin - pdf.r_margin
    ncols = len(head)
    # estimate: first column 30%, others share 70% (override for tighter looks)
    if ncols == 2:
        widths = [avail*0.32, avail*0.68]
    elif ncols == 3:
        widths = [avail*0.22, avail*0.18, avail*0.60]
    else:
        widths = [avail/ncols] * ncols

    line_h = 5.0
    pdf.set_font(SANS, "B", 9.5)

    def cell_text(segs):
        return "".join(s[0] for s in segs)

    def row_height(cells):
        h = line_h
        pdf.set_font(SANS, "", 9.5)
        for w, segs in zip(widths, cells):
            txt = cell_text(segs)
            # crude line count
            pdf.set_font(SANS, "", 9.5)
            n = max(1, len(pdf.multi_cell(w-2, line_h, txt, split_only=True)))
            h = max(h, line_h * n + 2)
        return h

    # Header
    pdf.set_fill_color(*TH_BG)
    pdf.set_text_color(*DARK)
    pdf.set_draw_color(180, 180, 180)
    h = row_height(head) + 1
    if pdf.get_y() + h > pdf.h - pdf.b_margin:
        pdf.add_page()
    x_start = pdf.l_margin
    y0 = pdf.get_y()
    for w, segs in zip(widths, head):
        x = pdf.get_x(); y = pdf.get_y()
        pdf.rect(x, y, w, h, style="DF")
        pdf.set_xy(x+1, y+1)
        pdf.set_font(SANS, "B", 9.5)
        pdf.multi_cell(w-2, line_h, cell_text(segs))
        pdf.set_xy(x+w, y)
    pdf.set_xy(x_start, y0+h)

    # Body
    pdf.set_text_color(*DARK)
    for ridx, row in enumerate(body_rows):
        h = row_height(row) + 1
        if pdf.get_y() + h > pdf.h - pdf.b_margin:
            pdf.add_page()
        y0 = pdf.get_y()
        fill = ridx % 2 == 1
        if fill:
            pdf.set_fill_color(*ROW_ALT)
        for w, segs in zip(widths, row):
            x = pdf.get_x(); y = pdf.get_y()
            if fill:
                pdf.rect(x, y, w, h, style="DF")
            else:
                pdf.rect(x, y, w, h, style="D")
            pdf.set_xy(x+1, y+1)
            pdf.set_font(SANS, "", 9.5)
            # render with simple bold support
            txt_parts = segs
            # quick render: one-line multi_cell of joined text
            full = ""
            for t, style, mono, link in txt_parts:
                full += t
            pdf.multi_cell(w-2, line_h, full)
            pdf.set_xy(x+w, y)
        pdf.set_xy(pdf.l_margin, y0+h)
    pdf.ln(2)


def render_code_block(node):
    code = node.get("raw", "")
    pdf.ln(1)
    pdf.set_font(MONO, "", 9)
    line_h = 4.4
    lines = code.rstrip("\n").split("\n")
    avail_w = pdf.w - pdf.l_margin - pdf.r_margin
    block_h = line_h * len(lines) + 4
    if pdf.get_y() + block_h > pdf.h - pdf.b_margin:
        pdf.add_page()
    x0, y0 = pdf.l_margin, pdf.get_y()
    pdf.set_fill_color(*PRE_BG)
    pdf.rect(x0, y0, avail_w, block_h, style="F")
    pdf.set_text_color(*PRE_FG)
    pdf.set_xy(x0+2, y0+2)
    for ln in lines:
        pdf.set_x(x0+2)
        # truncate if too long
        if pdf.get_string_width(ln) > avail_w - 4:
            while ln and pdf.get_string_width(ln + "...") > avail_w - 4:
                ln = ln[:-1]
            ln += "..."
        pdf.cell(avail_w-4, line_h, ln)
        pdf.ln(line_h)
    pdf.set_text_color(*DARK)
    pdf.ln(2)


def render_thematic_break(node):
    pdf.ln(2)
    pdf.set_draw_color(190, 190, 190)
    pdf.set_line_width(0.2)
    pdf.line(pdf.l_margin, pdf.get_y(), pdf.w - pdf.r_margin, pdf.get_y())
    pdf.ln(3)


def render_blank(node):
    pdf.ln(2)


def render_block(node):
    t = node["type"]
    if t == "heading":
        render_heading(node)
    elif t == "paragraph":
        render_paragraph(node)
    elif t == "list":
        render_list(node)
    elif t == "table":
        render_table(node)
    elif t == "block_code":
        render_code_block(node)
    elif t == "thematic_break":
        render_thematic_break(node)
    elif t == "blank_line":
        render_blank(node)
    elif t == "block_quote":
        old_lm = pdf.l_margin
        pdf.set_left_margin(old_lm + 4)
        for c in node["children"]:
            render_block(c)
        pdf.set_left_margin(old_lm)
    else:
        if "children" in node:
            for c in node["children"]:
                render_block(c)
        elif "raw" in node:
            pdf.set_font(SANS, "", 10.5)
            pdf.multi_cell(0, 5.2, node["raw"])


for tok in tokens:
    render_block(tok)

pdf.output(str(OUT))
print(f"Wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size//1024} KB, {pdf.page_no()} pages)")
