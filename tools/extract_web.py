#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 firmware/src/web_ui.h 里提取内嵌的 HTML，输出到 web/index.html。

用途：
  1. 想改网页时，可以直接在浏览器里预览 web/index.html（接口会失败，但布局/JS 能看）
  2. 可以拿 web/index.html 做前端语法检查

真机上的网页代码仍然以 firmware/src/web_ui.h 为准（改完重新编译烧录才会生效）。

用法： python tools/extract_web.py
"""
import re
import sys
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "firmware" / "src" / "web_ui.h"
out = root / "web" / "index.html"

text = src.read_text(encoding="utf-8")
m = re.search(r'R"=====\((.*)\)=====";', text, re.S)
if not m:
    sys.exit("未在 web_ui.h 中找到 HTML 原始字符串 (R\"=====( ... )=====\")")

html = m.group(1)
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(html, encoding="utf-8")
print("已生成 %s (%d 字节)" % (out, len(html.encode("utf-8"))))
