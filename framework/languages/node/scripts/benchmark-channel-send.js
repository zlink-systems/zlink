#!/usr/bin/env node
// Usage: node scripts/benchmark-channel-send.js BASELINE_DIST CURRENT_DIST [iterations]
const path = require('node:path');
const { createHook } = require('node:async_hooks');
const { performance } = require('node:perf_hooks');
const { channelSendFixture } = require('../test/contract/fixtures/channel-send');
const { SubmitResult } = require('../packages/framework/dist/runtime/backend/runtime-values');

function configure(fixtures, scenario) {
  for (const fixture of fixtures) {
    fixture.setSubmit(() =>
      scenario === 'deferred'
        ? { result: SubmitResult.Backpressured, admitted: Promise.resolve() }
        : { result: SubmitResult.Ok }
    );
  }
  return scenario === 'no-target' ? 'missing' : 'channel';
}

async function main() {
  const [baseline, current, iterationsText = '5000'] = process.argv.slice(2);
  const iterations = Number(iterationsText);
  if (!baseline || !current || !Number.isSafeInteger(iterations) || iterations < 1) {
    throw new Error(
      'Usage: node scripts/benchmark-channel-send.js BASELINE_DIST CURRENT_DIST [iterations]'
    );
  }
  const records = [];
  const median = (values) => [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
  const createFixtures = (backend) =>
    Promise.all([baseline, current].map((dist) => channelSendFixture(path.resolve(dist), backend)));
  // CPU time helps distinguish scheduler delay from process work; it includes GC.
  // Finish every timing sample before enabling allocation hooks anywhere in this process.
  for (const backend of [false, true, 'spot']) {
    const fixtures = await createFixtures(backend);
    try {
      for (const scenario of ['immediate', 'no-target', 'deferred']) {
        const channel = configure(fixtures, scenario);
        for (const bytes of [1024, 4096]) {
          const payload = Buffer.alloc(bytes);
          const times = [[], []];
          const cpuTimes = [[], []];
          const run = async (index, count) => {
            const target = fixtures[index].target;
            const cpuStart = process.cpuUsage();
            const start = performance.now();
            for (let i = 0; i < count; i++) await target.sendToChannel(channel, payload);
            const wallNsPerOp = ((performance.now() - start) * 1e6) / count;
            const cpu = process.cpuUsage(cpuStart);
            return { wallNsPerOp, cpuNsPerOp: ((cpu.user + cpu.system) * 1000) / count };
          };
          await run(0, 10000);
          await run(1, 10000);
          for (let round = 0; round < 32; round++) {
            for (const index of round % 2 ? [1, 0] : [0, 1]) {
              const sample = await run(index, iterations);
              times[index].push(sample.wallNsPerOp);
              cpuTimes[index].push(sample.cpuNsPerOp);
            }
          }
          const before = median(times[0]);
          const after = median(times[1]);
          records.push({
            node: process.version,
            path: backend === 'spot' ? 'spot' : backend ? 'backend' : 'runtime',
            scenario,
            bytes,
            iterations,
            baselineNsPerOp: before,
            currentNsPerOp: after,
            changePercent: 100 * (after / before - 1),
            pairedWallChangePercent:
              100 * (median(times[1].map((value, round) => value / times[0][round])) - 1),
            baselineCpuNsPerOp: median(cpuTimes[0]),
            currentCpuNsPerOp: median(cpuTimes[1]),
            pairedCpuChangePercent:
              100 * (median(cpuTimes[1].map((value, round) => value / cpuTimes[0][round])) - 1),
            roundsNsPerOp: times,
            roundsCpuNsPerOp: cpuTimes
          });
        }
      }
    } finally {
      await Promise.all(fixtures.map((f) => f.close()));
    }
  }
  // Allocation measurement is separate from timing. Subtract the identical mock
  // submission baseline and exclude the caller's await from synchronous init counts.
  for (const backend of [false, true, 'spot']) {
    const fixtures = await createFixtures(backend);
    try {
      for (const record of records.filter(
        (row) => row.path === (backend === 'spot' ? 'spot' : backend ? 'backend' : 'runtime')
      )) {
        const channel = configure(fixtures, record.scenario);
        const payload = Buffer.alloc(record.bytes);
        const allocations = [];
        for (const fixture of fixtures) {
          let promises = 0;
          const hook = createHook({
            init(_id, type) {
              if (type === 'PROMISE') promises++;
            }
          });
          const count = 1000;
          for (let i = 0; i < count; i++) {
            let pending;
            hook.enable();
            try {
              pending = fixture.target.sendToChannel(channel, payload);
            } finally {
              hook.disable();
            }
            await pending;
          }
          const pathPromises = promises;
          promises = 0;
          for (let i = 0; i < count; i++) {
            let submission;
            hook.enable();
            try {
              submission = fixture.mockSubmit('peer', [payload]);
            } finally {
              hook.disable();
            }
            await submission.admitted;
          }
          allocations.push((pathPromises - promises) / count);
        }
        [record.baselinePromiseInitsPerOp, record.currentPromiseInitsPerOp] = allocations;
        console.log(JSON.stringify(record));
      }
    } finally {
      await Promise.all(fixtures.map((f) => f.close()));
    }
  }
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
