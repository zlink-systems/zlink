// usage: node build-diagram.mjs <type> <ir.json> <out.html>   (render archify IR -> HTML, cropped to content)
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { chromium } from 'playwright';
// Requires a local archify install (see README) and playwright. Full workflow:
// doc/principal/documentation/diagram-authoring-guide.ko.md
const A = process.env.ARCHIFY_DIR;
if (!A) { console.error('Set ARCHIFY_DIR to your archify checkout (the dir with bin/archify.mjs).'); process.exit(1); }
const [type, ir, out] = process.argv.slice(2);
execFileSync('node', [A + '/bin/archify.mjs', 'render', type, ir, out], { stdio: 'inherit' });

// "×N" multi-instance nodes: draw 2 offset copies BEHIND the node (peek bottom-right)
// to read as a stack of many. Applied to nodes whose label mentions × / 서버들 / 서비스들.
function insertStacks(html) {
  const re = /(<g id="node-\w+"([^>]*)>)([\s\S]*?<rect x="([\d.-]+)" y="([\d.-]+)" width="([\d.-]+)" height="([\d.-]+)" rx="6" class="c-mask"\/>\s*<rect x="[\d.-]+" y="[\d.-]+" width="[\d.-]+" height="[\d.-]+" rx="6" class="(c-[a-z]+)"[^>]*\/>)/g;
  return html.replace(re, (m, gopen, attrs, rects, x, y, w, hh, cls) => {
    if (!/×|서버들|서비스들/.test(attrs)) return m;
    const X = +x, Y = +y, W = +w, H = +hh;
    // "stacked cards" peeking to the bottom-right: solid outlines in the kind's border
    // color (theme-adaptive, saturated → visible on light AND dark). Bigger offset +
    // thicker stroke so the stack is unmistakable, not a hairline.
    const copy = (o) => `<rect x="${X + o}" y="${Y + o}" width="${W}" height="${H}" rx="6" class="${cls}" style="fill:none" stroke-width="2.2"/>`;
    return gopen + copy(22) + copy(11) + rects;
  });
}
writeFileSync(out, insertStacks(readFileSync(out, 'utf8')));

// crop viewBox to actual content so the diagram fills the frame (bigger boxes/text)
const b = await chromium.launch();
const p = await b.newPage();
await p.goto('file://' + out, { waitUntil: 'load' });
await p.waitForTimeout(1200);
const vb = await p.evaluate(() => {
  const svg = document.querySelector('svg'); if (!svg) return null;
  const sel = svg.querySelectorAll('rect[class*="c-"], rect[data-graph-role], text, path[class*="m-"], path[class*="a-"]');
  let x0=1e9,y0=1e9,x1=-1e9,y1=-1e9,n=0;
  sel.forEach(e=>{ try{ const bb=e.getBBox(); if(bb.width>0&&bb.width<5000){ x0=Math.min(x0,bb.x); y0=Math.min(y0,bb.y); x1=Math.max(x1,bb.x+bb.width); y1=Math.max(y1,bb.y+bb.height); n++; } }catch(_){} });
  return n?[x0,y0,x1,y1]:null;
});
await b.close();
// strip interactive viewer chrome (toolbar + PATH/MAP/LENS nav + guided views + overlays)
// so the embedded figure is just the diagram + title + cards, like a clean doc illustration.
const CHROME_STRIP = '<style id="zlink-embed-clean">.toolbar,.no-print{display:none!important}.container{padding-top:8px!important}</style>';
let h=readFileSync(out,'utf8');
if(!h.includes('zlink-embed-clean')) h=h.replace('</head>', CHROME_STRIP + '</head>');
if (vb){ const pad=28; const nvb=`${Math.round(vb[0]-pad)} ${Math.round(vb[1]-pad)} ${Math.round(vb[2]-vb[0]+2*pad)} ${Math.round(vb[3]-vb[1]+2*pad)}`;
  h=h.replace(/viewBox="[^"]*"/, `viewBox="${nvb}"`); writeFileSync(out,h); console.log('built+cropped+clean:', out, nvb); }
else { writeFileSync(out,h); console.log('built+clean (no crop):', out); }
