"""Minify the page the way the device serves it.

Shared by gen.py (what goes into net/httpd.c) and preview.py (what the
jsdom cases run), so the tests see the same page the board sends.  The
main <script> block and the parallel NAND <script data-pn> blocks are
minified (they hold no gen.py markers); the one <style> block loses its
comments.  The markup and the small theme script in <head> stay as
written.

The script is ~130 KB and U-Boot carries it LZMA-packed in a BL33 that
BL2 can barely fit (files/fip/check-bl33.sh); minified it costs ~8 KiB less.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BLOCK = re.compile(r"^<script>\n(.*?)^</script>$", re.S | re.M)
# The parallel NAND half, which gen.py compiles in only on boards that have
# one (<!--#if PNAND-->).  Minified the same way, on its own.
EXTRA = re.compile(r"^<script data-pn>\n(.*?)^</script>$", re.S | re.M)
# The CSS comments are notes for whoever edits page.html; on the board they
# are ~800 bytes of BL33 that XG-040G-MF has little of to spare.
STYLE = re.compile(r"^<style>\n(.*?)^</style>$", re.S | re.M)


def terse(body):
    if "@@" in body or "<!--#" in body:
        sys.exit("jsmin: gen.py markers inside a script")
    r = subprocess.run(["node", os.path.join(HERE, "minify.js")],
                       input=body.encode(), capture_output=True)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        sys.exit("jsmin: minify.js failed")
    code = r.stdout.decode()
    if "</script" in code.lower():
        sys.exit("jsmin: minified script contains </script")
    return code


def uncomment(css):
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    if "/*" in css or "*/" in css:
        sys.exit("jsmin: unbalanced comment in <style>")
    # a comment that had a line of its own leaves nothing behind
    return re.sub(r"^[ \t]*\n", "", css, flags=re.M)


def minify(html):
    blocks = list(BLOCK.finditer(html))
    if len(blocks) != 1:
        sys.exit("jsmin: expected one <script> block on lines of its own, "
                 "found %d" % len(blocks))
    m = blocks[0]
    html = html[:m.start(1)] + terse(m.group(1)) + "\n" + html[m.end(1):]
    for m in reversed(list(EXTRA.finditer(html))):
        html = html[:m.start(1)] + terse(m.group(1)) + "\n" + html[m.end(1):]
    styles = list(STYLE.finditer(html))
    if len(styles) != 1:
        sys.exit("jsmin: expected one <style> block on lines of its own, "
                 "found %d" % len(styles))
    m = styles[0]
    return html[:m.start(1)] + uncomment(m.group(1)) + html[m.end(1):]
