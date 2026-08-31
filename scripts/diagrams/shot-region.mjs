import { chromium } from 'playwright';
const [url, out, needle] = process.argv.slice(2);
const b = await chromium.launch(); const p = await b.newPage();
await p.setViewportSize({ width: 1100, height: 1000 });
await p.goto(url, { waitUntil: 'load' });
await p.waitForTimeout(2500);
const h = await p.evaluate((nd) => {
  const els = [...document.querySelectorAll('h3,h4,strong,p')];
  const t = els.find(e => e.textContent.includes(nd));
  if (t) { t.scrollIntoView(); return true; } return false;
}, needle);
await p.waitForTimeout(1500);
await p.screenshot({ path: out });
await b.close(); console.log('region shot', out, 'found=', h);
