const assert = require('node:assert/strict');
const path = require('node:path');

const baselinePath = process.argv[2];
if (baselinePath === undefined) {
  throw new Error('Usage: node framework-json-parse-cost.cjs <baseline-json.cjs>');
}

const implementations = [
  { name: 'baseline', module: require(path.resolve(baselinePath)) },
  {
    name: 'current',
    module: require('../packages/stream-wire/dist/internal/framework-json-v1')
  }
];

const cases = [
  {
    name: '1 KiB ASCII payload',
    text: JSON.stringify({ payload: 'x'.repeat(1024) })
  },
  {
    name: 'small header',
    text: JSON.stringify({ kind: 2, codec: 1, flags: 0, requestSeq: '41', name: 'Ping' })
  },
  {
    name: 'escaped string',
    text: JSON.stringify({ value: 'quote " slash \\ line\n snowman ☃ emoji 😃' })
  },
  {
    name: 'many small fields and array',
    text: JSON.stringify({
      ...Object.fromEntries(Array.from({ length: 48 }, (_, index) => [`f${index}`, `v${index}`])),
      items: Array.from({ length: 32 }, (_, index) => `i${index}`)
    })
  }
];

const warmupIterations = 5000;
const measuredIterations = 10000;
const repetitions = 9;
let sink = 0;

for (const item of cases) {
  const expected = implementations[0].module.parseFrameworkJsonV1(item.text);
  assert.deepEqual(implementations[1].module.parseFrameworkJsonV1(item.text), expected);
  for (const implementation of implementations) {
    for (let iteration = 0; iteration < warmupIterations; iteration += 1) {
      sink += implementation.module.parseFrameworkJsonV1(item.text) === null ? 0 : 1;
    }
  }
}

function median(values) {
  const sorted = [...values].sort((left, right) => left - right);
  return sorted[Math.floor(sorted.length / 2)];
}

function measure(implementation, text) {
  const heapBefore = process.memoryUsage().heapUsed;
  const cpuBefore = process.cpuUsage();
  const start = process.hrtime.bigint();
  for (let iteration = 0; iteration < measuredIterations; iteration += 1) {
    sink += implementation.module.parseFrameworkJsonV1(text) === null ? 0 : 1;
  }
  const elapsedNs = Number(process.hrtime.bigint() - start);
  const cpu = process.cpuUsage(cpuBefore);
  const heapAfter = process.memoryUsage().heapUsed;
  return {
    elapsedNs,
    cpuUserMicros: cpu.user,
    cpuSystemMicros: cpu.system,
    heapDeltaBytes: heapAfter - heapBefore
  };
}

const results = [];
for (const item of cases) {
  const samples = new Map(implementations.map(({ name }) => [name, []]));
  for (let repetition = 0; repetition < repetitions; repetition += 1) {
    const order = repetition % 2 === 0 ? implementations : [...implementations].reverse();
    for (const implementation of order) {
      samples.get(implementation.name).push(measure(implementation, item.text));
    }
  }

  const baseline = samples.get('baseline');
  const current = samples.get('current');
  const medianNsPerOp = (sampleSet) =>
    median(sampleSet.map(({ elapsedNs }) => elapsedNs / measuredIterations));
  const medianCpuNsPerOp = (sampleSet) =>
    median(
      sampleSet.map(
        ({ cpuUserMicros, cpuSystemMicros }) =>
          ((cpuUserMicros + cpuSystemMicros) * 1000) / measuredIterations
      )
    );
  const medianHeapDeltaBytesPerOp = (sampleSet) =>
    median(sampleSet.map(({ heapDeltaBytes }) => heapDeltaBytes / measuredIterations));
  const baselineNs = medianNsPerOp(baseline);
  const currentNs = medianNsPerOp(current);
  results.push({
    name: item.name,
    rawSamples: { baseline, current },
    medians: {
      baselineNsPerOp: baselineNs,
      currentNsPerOp: currentNs,
      currentToBaselineNs: currentNs / baselineNs,
      baselineCpuNsPerOp: medianCpuNsPerOp(baseline),
      currentCpuNsPerOp: medianCpuNsPerOp(current),
      baselineHeapDeltaBytesPerOp: medianHeapDeltaBytesPerOp(baseline),
      currentHeapDeltaBytesPerOp: medianHeapDeltaBytesPerOp(current)
    }
  });
}

console.log(
  JSON.stringify(
    {
      runtime: process.version,
      warmupIterations,
      measuredIterations,
      repetitions,
      heapSample: 'net heapUsed delta per operation, not an allocation counter',
      cases: results,
      sink
    },
    null,
    2
  )
);
