// Minify the page's main script: stdin in, stdout out.  Run by jsmin.py.
//
// Top-level names stay as written (terser only renames locals unless told
// otherwise): the markup calls them from onclick= and friends, and so do
// the HTML strings the script builds.  terser is pinned in test/package.json
// so that everyone who runs gen.py gets the same bytes, and CI's gen.py
// Sync step with them.
'use strict';
const path = require('path');
let terser;
try {
  terser = require(path.join(__dirname, 'test', 'node_modules', 'terser'));
} catch (e) {
  process.stderr.write('terser not found: cd ' + path.join(__dirname, 'test') +
                       ' && npm install\n');
  process.exit(2);
}
let src = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (d) => { src += d; });
process.stdin.on('end', async () => {
  try {
    const r = await terser.minify(src, {
      ecma: 5,
      compress: true,
      mangle: true,
      format: { ascii_only: false, comments: false },
    });
    process.stdout.write(r.code);
  } catch (e) {
    process.stderr.write('terser: ' + e.message + '\n');
    process.exit(1);
  }
});
