'use strict';

// Run with a built baseline module path. Both versions alternate in this process.
// node perf/request-backpressure-cost.cjs /absolute/baseline/.../node-raw-binding-port.js
const assert = require('node:assert/strict');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const baseline = require(path.resolve(process.argv[2]));
const currentRaw = require('../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const zlink = require('@zlink-systems/zlink');
const { setImmediate: immediate } = require('node:timers/promises');
const count = 4096;
let endpointSequence = 0;

async function receiveRun(api) {
  const context = zlink.createContext();
  const host = new api.ZLinkNodeRawBindingPort(context).createHost();
  const target = host.createRouter();
  const source = zlink.createRouterSocket(context);
  const targetId = zlink.RoutingId.from('target');
  const endpoint = `inproc://bp-receive-cost-${process.pid}-${endpointSequence++}`;
  target.setRoutingId('target');
  target.bind(endpoint);
  source.setRoutingId(zlink.RoutingId.from('source'));
  source.options.setConnectRoutingId(targetId);
  source.connect(endpoint);
  const payload = Buffer.alloc(1024, 7);
  try {
    while (source.routesSnapshot().length === 0) await immediate();
    for (let batch = 0; batch < count; batch += 256) {
      for (let index = 0; index < 256; index++) source.send(targetId).message(payload).submit();
      let received = 0;
      while (received < 256) {
        const record = target.receive(true);
        if (record === undefined) {
          await immediate();
          continue;
        }
        assert.equal(record.parts[0].length, payload.length);
        record.close();
        received++;
      }
    }
  } finally {
    host.close();
    source.close();
    context.close();
  }
}

async function compare(run, baseline, current) {
  for (let warmup = 0; warmup < 6; warmup++) {
    await run(baseline);
    await run(current);
  }
  const samples = { baseline: [], current: [] };
  for (let round = 0; round < 9; round++) {
    for (const name of round % 2 === 0 ? ['baseline', 'current'] : ['current', 'baseline']) {
      const usage = process.cpuUsage();
      const start = performance.now();
      await run(name === 'baseline' ? baseline : current);
      const cpu = process.cpuUsage(usage);
      samples[name].push({
        ns: ((performance.now() - start) * 1e6) / count,
        cpuNs: ((cpu.user + cpu.system) * 1000) / count
      });
    }
  }
  const median = (values) => values.sort((a, b) => a - b)[Math.floor(values.length / 2)];
  const result = {};
  for (const [name, rows] of Object.entries(samples)) {
    result[name] = {
      nsPerWait: median(rows.map((row) => row.ns)),
      cpuNsPerWait: median(rows.map((row) => row.cpuNs))
    };
  }
  return { count, samples, result, ratio: result.current.nsPerWait / result.baseline.nsPerWait };
}

async function main() {
  console.log(
    JSON.stringify(
      {
        receive: await compare(receiveRun, baseline, currentRaw)
      },
      null,
      2
    )
  );
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
