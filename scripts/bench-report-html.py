#!/usr/bin/env python3
"""Render docs/benchmark.md to docs/benchmark/report.html (no dependencies).

Handles the subset benchmark.md uses: headings, paragraphs, bullet lists,
pipe tables, images, links, `code` and **bold**. Image links to
charts/*.png are swapped for the .svg twin so the page stays vector.
"""
import html, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "docs/benchmark.md")
DST = os.path.join(ROOT, "docs/benchmark/report.html")

def inline(s):
    out, i = [], 0
    for m in re.finditer(r"`([^`]+)`", s):
        out.append(("t", s[i:m.start()])); out.append(("c", m.group(1))); i = m.end()
    out.append(("t", s[i:]))
    res = ""
    for k, v in out:
        if k == "c":
            res += "<code>%s</code>" % html.escape(v)
            continue
        v = html.escape(v, quote=False)
        v = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", lambda m: "<img alt='%s' src='%s'>" % (m.group(1), fix(m.group(2), True)), v)
        v = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", lambda m: "<a href='%s'>%s</a>" % (fix(m.group(2), False), m.group(1)), v)
        v = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", v)
        res += v
    # links whose text is code: [`x`](y) -> handled because code split first; fix leftovers
    res = re.sub(r"\[(<code>[^<]*</code>)\]\(([^)]+)\)", lambda m: "<a href='%s'>%s</a>" % (fix(m.group(2), False), m.group(1)), res)
    return res

def fix(url, img):
    if url.startswith("benchmark/"):
        url = url[len("benchmark/"):]
    if img and url.startswith("charts/") and url.endswith(".png"):
        url = url[:-4] + ".svg"
    return url

def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]

def main():
    lines = open(SRC, encoding="utf-8").read().splitlines()
    body, i, para = [], 0, []
    def flush():
        if para:
            body.append("<p>%s</p>" % inline(" ".join(para))); para.clear()
    while i < len(lines):
        l = lines[i]
        if not l.strip():
            flush(); i += 1; continue
        m = re.match(r"^(#+)\s+(.*)", l)
        if m:
            flush(); n = len(m.group(1)); body.append("<h%d>%s</h%d>" % (n, inline(m.group(2)), n)); i += 1; continue
        if l.startswith("|") and i + 1 < len(lines) and re.match(r"^\|[-:| ]+\|$", lines[i + 1].strip()):
            flush()
            head = cells(l); aligns = cells(lines[i + 1]); i += 2
            t = "<table><tr>" + "".join("<th>%s</th>" % inline(c) for c in head) + "</tr>"
            while i < len(lines) and lines[i].startswith("|"):
                t += "<tr>" + "".join("<td>%s</td>" % inline(c) for c in cells(lines[i])) + "</tr>"; i += 1
            body.append(t + "</table>"); continue
        if l.startswith("- "):
            flush(); items = []
            while i < len(lines) and lines[i].startswith("- "):
                items.append(lines[i][2:]); i += 1
                while i < len(lines) and lines[i].startswith("  ") and lines[i].strip():
                    items[-1] += " " + lines[i].strip(); i += 1
            body.append("<ul>" + "".join("<li>%s</li>" % inline(x) for x in items) + "</ul>"); continue
        para.append(l.strip()); i += 1
    flush()
    css = ("body{font-family:'Noto Sans CJK SC','Noto Sans CJK TC',sans-serif;max-width:1100px;margin:32px auto;"
           "padding:0 20px;color:#0f172a;line-height:1.6}img{max-width:100%;height:auto;display:block;margin:12px 0}"
           "table{border-collapse:collapse;font-size:13px;margin:12px 0}td,th{border-bottom:1px solid #e2e8f0;"
           "padding:4px 8px;text-align:right}td:first-child,th:first-child{text-align:left}"
           "code{background:#f1f5f9;padding:1px 4px;border-radius:3px}h1,h2,h3{font-weight:650}")
    doc = ("<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>\n<title>goc 跑分</title>\n<style>%s</style></head><body>\n%s\n</body></html>\n"
           % (css, "\n".join(body)))
    open(DST, "w", encoding="utf-8").write(doc)
    print("wrote", DST)

if __name__ == "__main__":
    main()
