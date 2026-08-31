// Normalize every diagram's main-svg viewBox to a COMMON width (content centered),
// so all diagrams render at the same scale (uniform box/text size) at width:100%.
// usage: node pad-viewbox.mjs <commonWidth> <file.html...>
import { readFileSync, writeFileSync } from 'node:fs';
const CW = Number(process.argv[2]);
const files = process.argv.slice(3);
for (const f of files) {
  let h = readFileSync(f, 'utf8');
  // main svg viewBox = the one on the <svg ... role="img"> tag
  const m = h.match(/<svg viewBox="([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)"/);
  if (!m) { console.log('no viewBox:', f); continue; }
  let [ , x, y, w, ht ] = m.map(Number);
  x = m[1] * 1; y = m[2] * 1; w = m[3] * 1; ht = m[4] * 1;
  const width = Math.max(CW, w);
  const nx = x - (width - w) / 2;
  const nvb = `${Math.round(nx)} ${Math.round(y)} ${Math.round(width)} ${Math.round(ht)}`;
  h = h.replace(/(<svg viewBox=")[-\d. ]+(")/, `$1${nvb}$2`);
  writeFileSync(f, h);
  console.log('padded', f.split('/').pop(), '->', nvb);
}
