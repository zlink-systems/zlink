// SPDX-License-Identifier: MPL-2.0
'use strict';
// Minimal ROUTER->ROUTER request client at a fixed outstanding-request window.
// Uses bindings/node directly; shares no code with the with-grpc harness.

const zlink = require('@zlink-systems/zlink');

const argv = process.argv.slice(2);
const arg = (name, fallback) => {
  const i = argv.indexOf(name);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : fallback;
};
const num = (name, fallback) => Number.parseInt(arg(name, String(fallback)), 10);

const endpoint = arg('--endpoint', 'tcp://127.0.0.1:5185');
const windowSize = num('--window', 100);
const durationSeconds = num('--duration', 2);
const payloadSize = num('--payload', 1024);
const warmup = num('--warmup', 20);
const timeoutMs = num('--timeout-ms', 30000);
const partCount = num('--parts', 2);
const traceMs = num('--trace-ms', 0);
const mode = arg('--mode', 'sliding');

const nowNs = () => process.hrtime.bigint();

(async () => {
  const ctx = zlink.createContext();
  const peer = zlink.RoutingId.from(Buffer.from('repro-server', 'ascii'));
  const router = zlink.createRouterSocket(ctx);
  router.setRoutingId(zlink.RoutingId.from(Buffer.from(`repro-client-${process.pid}`, 'ascii')));
  router.options.mandatory = true;
  router.options.setConnectRoutingId(peer);
  router.connect(endpoint);

  const header = Buffer.alloc(64, 0x5a);
  const payload = Buffer.alloc(payloadSize, 0xab);

  const request = () => {
    let op = router.request(peer);
    if (partCount === 2) op = op.message(header);
    return op.message(payload).timeout(timeoutMs).submit();
  };

  const once = async () => {
    const parts = await request();
    for (const part of parts) part.close();
  };

  // bounded route readiness, before the measured window
  const deadline = Date.now() + 15000;
  let ready = false;
  let last = null;
  while (Date.now() < deadline) {
    try { await once(); ready = true; break; }
    catch (error) { last = error; await new Promise((r) => setTimeout(r, 20)); }
  }
  if (!ready) throw new Error(`route not ready: ${last && last.message}`);
  for (let i = 0; i < warmup; i++) await once();

  let inFlight = 0;
  let peak = 0;
  let completed = 0;
  let errors = 0;
  let wake = null;
  const latencies = [];
  const release = () => {
    inFlight -= 1;
    if (wake !== null) { const r = wake; wake = null; r(); }
  };

  const startNs = nowNs();
  const untilNs = startNs + BigInt(durationSeconds) * 1000000000n;
  let tracer = null;
  if (traceMs > 0) {
    tracer = setInterval(() => {
      process.stderr.write(
        `[trace] t=${(Number(nowNs() - startNs) / 1e6).toFixed(0)}ms completed=${completed}`
        + ` errors=${errors} inFlight=${inFlight}\n`
      );
    }, traceMs);
    tracer.unref();
  }

  if (mode === 'batch') {
    // Same sustained load, different admission shape: submit `window` requests,
    // wait for all of them, repeat. If this sustains while `sliding` collapses,
    // the refill pattern is implicated rather than the depth.
    while (nowNs() < untilNs) {
      const batchStart = Array.from({ length: windowSize }, () => {
        const t0 = nowNs();
        return request().then(
          (parts) => { for (const p of parts) p.close(); completed += 1; latencies.push(Number(nowNs() - t0) / 1000); },
          () => { errors += 1; latencies.push(Number(nowNs() - t0) / 1000); }
        );
      });
      peak = windowSize;
      await Promise.all(batchStart);
    }
  } else
  while (nowNs() < untilNs) {
    if (inFlight >= windowSize) {
      await new Promise((resolve) => { wake = resolve; });
      continue;
    }
    inFlight += 1;
    if (inFlight > peak) peak = inFlight;
    const t0 = nowNs();
    request().then(
      (parts) => { for (const p of parts) p.close(); completed += 1; latencies.push(Number(nowNs() - t0) / 1000); },
      () => { errors += 1; latencies.push(Number(nowNs() - t0) / 1000); }
    ).then(release, release);
  }
  if (tracer !== null) clearInterval(tracer);
  const settleUntil = Date.now() + 5000;
  while (inFlight > 0 && Date.now() < settleUntil) await new Promise((r) => setTimeout(r, 1));
  const abandoned = inFlight;
  const elapsed = Number(nowNs() - startNs) / 1e9;

  const sorted = latencies.slice().sort((a, b) => a - b);
  const pct = (f) => (sorted.length ? sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil(f * sorted.length) - 1))] : 0);
  const mean = sorted.length ? sorted.reduce((a, b) => a + b, 0) / sorted.length : 0;
  const max = sorted.length ? sorted[sorted.length - 1] : 0;
  process.stdout.write(JSON.stringify({
    window: windowSize,
    payload: payloadSize,
    parts: partCount,
    throughput_per_second: completed / elapsed,
    completed, errors, abandoned,
    peak_in_flight: peak,
    latency_mean_ms: mean / 1000,
    latency_p50_ms: pct(0.5) / 1000,
    latency_p95_ms: pct(0.95) / 1000,
    latency_p99_ms: pct(0.99) / 1000,
    latency_max_ms: max / 1000
  }) + '\n');
  router.close();
  ctx.close();
})().catch((error) => { console.error('REPRO CLIENT FAILED:', error.message); process.exit(1); });
