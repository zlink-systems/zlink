'use strict';

const path = require('node:path');

const baselineDist = process.argv[2];
if (!baselineDist) {
  throw new Error(
    'Usage: NODE_PATH=<node_modules> node message-flow-gate-cost.cjs <baseline-framework-dist>'
  );
}

const baseline = require(path.join(path.resolve(baselineDist), 'internal.js'));
const current = require(path.resolve(__dirname, '../packages/framework/dist/internal.js'));
const rounds = 9;
const warmupCalls = 1_000_000;
const measurementCalls = 1_000_000;
const cases = [
  {
    name: 'errorsReceived',
    mode: 'errors',
    sampleRate: 1,
    outcome: 'received',
    returnsPoint: false
  },
  { name: 'offReceived', mode: 'off', sampleRate: 1, outcome: 'received', returnsPoint: false },
  {
    name: 'normalCachedPoint',
    mode: 'normal',
    sampleRate: 1,
    outcome: 'received',
    returnsPoint: true
  },
  { name: 'normalSample0', mode: 'normal', sampleRate: 0, outcome: 'received', returnsPoint: false }
];

function createAction(api, configuration) {
  const dispatch = {
    diagnostics: {
      messageFlow: configuration.mode,
      sampleRate: configuration.sampleRate,
      includeMessageSizes: false
    }
  };
  const cell = api.createMessageFlowModeCell(dispatch);
  const context = api.createDiagnosticsContext(dispatch, undefined, cell);
  const tracer = new api.ZLinkMessageFlowTracer(context, { reportRuntimeTaskException() {} });
  return () => tracer.begin(configuration.outcome);
}

function validateSink(sink, callCount, returnsPoint) {
  if (returnsPoint) {
    if (
      sink.returnedPoints !== callCount ||
      sink.undefinedReturns !== 0 ||
      sink.point === undefined
    ) {
      throw new Error('normalCachedPoint must return its cached trace point on every call');
    }
  } else if (
    sink.returnedPoints !== 0 ||
    sink.undefinedReturns !== callCount ||
    sink.point !== undefined
  ) {
    throw new Error('gated message-flow begin must return undefined on every call');
  }
}

function warm(action, callCount, returnsPoint) {
  const sink = { point: undefined, returnedPoints: 0, undefinedReturns: 0 };
  for (let index = 0; index < callCount; index++) {
    const point = action();
    sink.point = point;
    if (point === undefined) sink.undefinedReturns++;
    else sink.returnedPoints++;
  }
  validateSink(sink, callCount, returnsPoint);
}

function run(action, callCount, returnsPoint) {
  const sink = { point: undefined, returnedPoints: 0, undefinedReturns: 0 };
  const cpuBefore = process.cpuUsage();
  const wallBefore = process.hrtime.bigint();
  for (let index = 0; index < callCount; index++) {
    const point = action();
    sink.point = point;
    if (point === undefined) sink.undefinedReturns++;
    else sink.returnedPoints++;
  }
  const wallNs = Number(process.hrtime.bigint() - wallBefore);
  const cpu = process.cpuUsage(cpuBefore);
  const cpuUserUs = cpu.user;
  const cpuSystemUs = cpu.system;
  const cpuNs = (cpuUserUs + cpuSystemUs) * 1000;
  validateSink(sink, callCount, returnsPoint);
  return {
    callCount,
    returnedPoints: sink.returnedPoints,
    undefinedReturns: sink.undefinedReturns,
    sinkPointPresent: sink.point !== undefined,
    wallNs,
    wallNsPerCall: wallNs / callCount,
    cpuUserUs,
    cpuSystemUs,
    cpuNs,
    cpuNsPerCall: cpuNs / callCount
  };
}

function median(values) {
  const sorted = [...values].sort((left, right) => left - right);
  return sorted[Math.floor(sorted.length / 2)];
}

console.log(
  JSON.stringify({
    type: 'run',
    node: process.version,
    platform: process.platform,
    arch: process.arch,
    rounds,
    warmupCalls,
    measurementCalls,
    order: 'AB/BA alternating',
    baselineDist: path.resolve(baselineDist),
    currentDist: path.resolve(__dirname, '../packages/framework/dist')
  })
);

for (const configuration of cases) {
  const actions = {
    baseline: createAction(baseline, configuration),
    current: createAction(current, configuration)
  };
  const results = { baseline: [], current: [] };
  for (let round = 0; round < rounds; round++) {
    const order = round % 2 === 0 ? ['baseline', 'current'] : ['current', 'baseline'];
    for (const variant of order) {
      const action = actions[variant];
      warm(action, warmupCalls, configuration.returnsPoint);
      const measurement = run(action, measurementCalls, configuration.returnsPoint);
      results[variant].push(measurement);
      console.log(
        JSON.stringify({
          type: 'measurement',
          case: configuration.name,
          round: round + 1,
          order: order.join('-'),
          warmupCalls,
          variant,
          ...measurement
        })
      );
    }
  }
  for (const variant of ['baseline', 'current']) {
    console.log(
      JSON.stringify({
        type: 'median',
        case: configuration.name,
        variant,
        rounds,
        warmupCalls,
        measurementCalls,
        wallNsPerCall: median(results[variant].map((result) => result.wallNsPerCall)),
        cpuNsPerCall: median(results[variant].map((result) => result.cpuNsPerCall))
      })
    );
  }
}
