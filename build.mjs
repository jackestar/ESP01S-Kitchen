import { build } from 'esbuild';
import { minify } from 'html-minifier-terser';
import { readFileSync, writeFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

const SRC = 'web';
const OUT = 'Arduino/ESP01S-Kitchen/data';

const bundle = async (file) =>
  (await build({ entryPoints: [`${SRC}/${file}`], minify: true, write: false }))
    .outputFiles[0].text;

const [css, js] = await Promise.all([bundle('styles.css'), bundle('script.js')]);

let html = readFileSync(`${SRC}/index.html`, 'utf8')
  .replace(/<link rel="stylesheet"[^>]*>/, () => `<style>${css}</style>`)
  .replace(/<script src="script.js"><\/script>/, () => `<script>${js}</script>`);

html = await minify(html, {
  collapseWhitespace: true,
  removeComments: true,
  minifyCSS: true,
  minifyJS: true,
});

writeFileSync(`${OUT}/index.html.gz`, gzipSync(html, { level: 9 }));
console.log(`index.html.gz: ${gzipSync(html, { level: 9 }).length} bytes`);
