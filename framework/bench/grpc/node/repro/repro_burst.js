// SPDX-License-Identifier: MPL-2.0
'use strict';
// No window loop, no bench code: submit K concurrent ROUTER->ROUTER requests in
// one go and report how many resolve. If some never resolve, the stall is a
// property of concurrent outstanding requests on the socket, not of any
// admission loop.

const zlink = require('@zlink-systems/zlink');
const argv = process.argv.slice(2);
const arg = (n, f) => { const i = argv.indexOf(n); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : f; };
const num = (n, f) => Number.parseInt(arg(n, String(f)), 10);

const endpoint = arg('--endpoint', 'tcp://127.0.0.1:5185');
const timeoutMs = num('--timeout-ms', 3000);
const payloadSize = num('--payload', 1024);
const rounds = num('--rounds', 3);

(async () => {
  const ctx = zlink.createContext();
  const peer = zlink.RoutingId.from(Buffer.from('repro-server', 'ascii'));
  const router = zlink.createRouterSocket(ctx);
  router.setRoutingId(zlink.RoutingId.from(Buffer.from(`repro-burst-${process.pid}`, 'ascii')));
  router.options.mandatory = true;
  router.options.setConnectRoutingId(peer);
  router.connect(endpoint);

  const header = Buffer.alloc(64, 0x5a);
  const payload = Buffer.alloc(payloadSize, 0xab);
  const one = () => router.request(peer).message(header).message(payload).timeout(timeoutMs).submit();

  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    try { const p = await one(); for (const x of p) x.close(); break; }
    catch (e) { await new Promise((r) => setTimeout(r, 20)); }
  }

  for (const k of [4, 8, 12, 16, 32, 100]) {
    for (let round = 0; round < rounds; round++) {
      const t0 = process.hrtime.bigint();
      const settled = await Promise.allSettled(Array.from({ length: k }, one));
      const elapsedMs = Number(process.hrtime.bigint() - t0) / 1e6;
      let ok = 0;
      for (const s of settled) {
        if (s.status === 'fulfilled') { ok += 1; for (const p of s.value) p.close(); }
      }
      process.stdout.write(
        `burst k=${String(k).padStart(4)} round=${round} resolved=${String(ok).padStart(4)}/${k}`
        + ` stalled=${String(k - ok).padStart(4)} wall=${elapsedMs.toFixed(1)}ms\n`
      );
    }
  }
  router.close();
  ctx.close();
})().catch((e) => { console.error('BURST FAILED:', e.message); process.exit(1); });
