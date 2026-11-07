# -*- coding: utf-8 -*-
"""把项目里的 Markdown 文档转成自带样式的单文件 HTML（离线可看、可打印）。

用法：
    python tools/md2html.py docs/用户使用手册.md [out.html] [--embed]

只覆盖本项目文档实际用到的语法：标题 / 段落 / 有序与无序列表 / 表格 /
围栏代码块 / 引用 / 行内 `code` / **粗体** / 图片 / 链接 / 分隔线。
图片路径按 md 文件所在目录解析成相对路径，生成的 html 与 md 放同一目录即可。
--embed 把图片转成 base64 内联，产出真正单文件（打包分发用，不用带 images/）。
"""
import base64
import html
import io
import mimetypes
import os
import re
import sys

CSS = """
:root {
  --fg: #1f2328; --muted: #5b6472; --line: #d8dee6; --bg: #ffffff;
  --soft: #f6f8fa; --accent: #1a56b0; --accent-soft: #eaf1fb;
}
* { box-sizing: border-box; }
body {
  margin: 0; padding: 40px 24px 80px; background: var(--bg); color: var(--fg);
  font: 15px/1.75 -apple-system, "Segoe UI", "Microsoft YaHei", "PingFang SC", sans-serif;
  -webkit-font-smoothing: antialiased;
}
main { max-width: 880px; margin: 0 auto; }
h1 { font-size: 30px; line-height: 1.3; margin: 0 0 8px; letter-spacing: .3px; }
h2 { font-size: 21px; margin: 40px 0 14px; padding-bottom: 8px;
     border-bottom: 1px solid var(--line); }
h3 { font-size: 17px; margin: 26px 0 10px; }
p { margin: 10px 0; }
a { color: var(--accent); }
strong { font-weight: 600; }
hr { border: 0; border-top: 1px solid var(--line); margin: 34px 0; }
ul, ol { margin: 10px 0 10px 0; padding-left: 26px; }
li { margin: 5px 0; }
code {
  background: var(--soft); border: 1px solid var(--line); border-radius: 4px;
  padding: 1px 5px; font: 13px/1.5 Consolas, "Courier New", monospace;
}
pre {
  background: var(--soft); border: 1px solid var(--line); border-radius: 6px;
  padding: 14px 16px; overflow-x: auto; margin: 14px 0;
}
pre code { background: none; border: 0; padding: 0; font-size: 13px; }
blockquote {
  margin: 14px 0; padding: 10px 16px; background: var(--accent-soft);
  border-left: 3px solid var(--accent); border-radius: 0 6px 6px 0; color: #24303f;
}
blockquote p { margin: 4px 0; }
table { border-collapse: collapse; width: 100%; margin: 16px 0; font-size: 14px; }
th, td { border: 1px solid var(--line); padding: 8px 12px; text-align: left; vertical-align: top; }
th { background: var(--soft); font-weight: 600; }
tbody tr:nth-child(even) { background: #fcfdfe; }
img { max-width: 100%; display: block; margin: 18px auto; border: 1px solid var(--line);
      border-radius: 8px; }
@media print {
  body { padding: 0; font-size: 11.5pt; }
  h2 { page-break-after: avoid; }
  table, img, pre { page-break-inside: avoid; }
  a { color: inherit; text-decoration: none; }
}
"""


def inline(text):
    """行内语法：先转义，再替换标记。"""
    s = html.escape(text, quote=False)
    s = re.sub(r"`([^`]+)`", lambda m: "<code>%s</code>" % m.group(1), s)
    s = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)", _img, s)
    s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", r'<a href="\2">\1</a>', s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", s)
    return s


def _img(m):
    alt, src = m.group(1), m.group(2)
    return '<img src="%s" alt="%s">' % (src, alt)


def is_table_row(line):
    return line.strip().startswith("|") and line.strip().endswith("|")


def split_row(line):
    body = line.strip()[1:-1]
    return [c.strip() for c in body.split("|")]


def convert(md_text):
    lines = md_text.replace("\r\n", "\n").split("\n")
    out, i, n = [], 0, len(lines)

    while i < n:
        line = lines[i]
        stripped = line.strip()

        # 围栏代码块
        if stripped.startswith("```"):
            i += 1
            buf = []
            while i < n and not lines[i].strip().startswith("```"):
                buf.append(lines[i])
                i += 1
            i += 1
            out.append("<pre><code>%s</code></pre>" % html.escape("\n".join(buf)))
            continue

        # 分隔线
        if re.fullmatch(r"-{3,}|\*{3,}", stripped):
            out.append("<hr>")
            i += 1
            continue

        # 标题
        m = re.match(r"^(#{1,4})\s+(.*)$", stripped)
        if m:
            lvl = len(m.group(1))
            out.append("<h%d>%s</h%d>" % (lvl, inline(m.group(2)), lvl))
            i += 1
            continue

        # 表格：当前行是 |...| 且下一行是分隔行
        if is_table_row(line) and i + 1 < n and re.match(r"^\|[\s:|-]+\|$", lines[i + 1].strip()):
            head = split_row(line)
            i += 2
            body = []
            while i < n and is_table_row(lines[i]):
                body.append(split_row(lines[i]))
                i += 1
            out.append("<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>" % (
                "".join("<th>%s</th>" % inline(c) for c in head),
                "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c) for c in row)
                        for row in body)))
            continue

        # 引用
        if stripped.startswith(">"):
            buf = []
            while i < n and lines[i].strip().startswith(">"):
                buf.append(lines[i].strip()[1:].strip())
                i += 1
            out.append("<blockquote><p>%s</p></blockquote>" % inline(" ".join(buf)))
            continue

        # 列表（支持一级 / 二级缩进）
        if re.match(r"^\s*(-|\d+\.)\s+", line):
            ordered = bool(re.match(r"^\s*\d+\.", line))
            tag = "ol" if ordered else "ul"
            items = []
            base_indent = len(line) - len(line.lstrip())
            while i < n and re.match(r"^\s*(-|\d+\.)\s+", lines[i]):
                cur = lines[i]
                indent = len(cur) - len(cur.lstrip())
                text = re.sub(r"^\s*(-|\d+\.)\s+", "", cur)
                if indent > base_indent and items:
                    items[-1] = items[-1] + "<br>" + inline(text)
                else:
                    items.append(inline(text))
                i += 1
            out.append("<%s>%s</%s>" % (tag, "".join("<li>%s</li>" % t for t in items), tag))
            continue

        # 空行
        if not stripped:
            i += 1
            continue

        # 段落：吃到空行或下一个块级元素
        buf = [stripped]
        i += 1
        while i < n:
            nxt = lines[i].strip()
            if not nxt or nxt.startswith(("#", ">", "```")) or is_table_row(lines[i]) \
                    or re.match(r"^(-{3,}|\*{3,})$", nxt) or re.match(r"^(-|\d+\.)\s+", nxt):
                break
            buf.append(nxt)
            i += 1
        out.append("<p>%s</p>" % inline(" ".join(buf)))

    return "\n".join(out)


def embed_images(page, base_dir):
    """把 <img src="相对路径"> 换成 data URI，产出真正单文件。"""

    def repl(m):
        src = m.group(1)
        if src.startswith(("data:", "http://", "https://")):
            return m.group(0)
        path = os.path.join(base_dir, src.replace("/", os.sep))
        if not os.path.exists(path):
            print("警告：找不到图片 %s，保留原引用" % path)
            return m.group(0)
        mime = mimetypes.guess_type(path)[0] or "image/png"
        data = base64.b64encode(open(path, "rb").read()).decode("ascii")
        return '<img src="data:%s;base64,%s"' % (mime, data)

    return re.sub(r'<img src="([^"]+)"', repl, page)


def main():
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    src = args[0]
    dst = args[1] if len(args) > 1 else os.path.splitext(src)[0] + ".html"
    text = io.open(src, encoding="utf-8").read()

    title = "文档"
    for line in text.split("\n"):
        if line.startswith("# "):
            title = line[2:].strip()
            break

    body = convert(text)
    page = ("<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "<title>%s</title>\n<style>%s</style>\n</head>\n<body>\n<main>\n%s\n</main>\n"
            "</body>\n</html>\n" % (html.escape(title), CSS, body))

    if "--embed" in flags:
        page = embed_images(page, os.path.dirname(os.path.abspath(src)))

    io.open(dst, "w", encoding="utf-8", newline="\n").write(page)
    print("wrote %s (%d bytes)" % (dst, len(page.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
