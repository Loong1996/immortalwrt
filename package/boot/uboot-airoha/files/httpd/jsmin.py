"""Minify the page's main <script> block, the way the device serves it.

Shared by gen.py (what goes into net/httpd.c) and preview.py (what the
jsdom cases run), so the tests see the same script the board sends.  Only
the one big block is touched: it holds no gen.py markers.  The CSS, the
markup and the small theme script in <head> stay as written.

The script is ~130 KB and U-Boot carries it LZMA-packed in a BL33 that
BL2 can barely fit (files/fip/check-bl33.sh); minified it costs ~8 KiB less.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BLOCK = re.compile(r"^<script>\n(.*?)^</script>$", re.S | re.M)


def minify(html):
    blocks = list(BLOCK.finditer(html))
    if len(blocks) != 1:
        sys.exit("jsmin: expected one <script> block on lines of its own, "
                 "found %d" % len(blocks))
    m = blocks[0]
    body = m.group(1)
    if "@@" in body or "<!--#" in body:
        sys.exit("jsmin: gen.py markers inside the main script")
    r = subprocess.run(["node", os.path.join(HERE, "minify.js")],
                       input=body.encode(), capture_output=True)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        sys.exit("jsmin: minify.js failed")
    code = r.stdout.decode()
    if "</script" in code.lower():
        sys.exit("jsmin: minified script contains </script")
    return html[:m.start(1)] + code + "\n" + html[m.end(1):]
