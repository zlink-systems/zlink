import { chromium } from 'playwright';
const [url, out, w, h] = process.argv.slice(2);
const b = await chromium.launch();
const p = await b.newPage({ viewport: { width: +w||1280, height: +h||1200 } });
await p.goto(url, { waitUntil: 'load' });
await p.waitForTimeout(3000);
await p.screenshot({ path: out, fullPage: true });
await b.close();
console.log('shot', out);
