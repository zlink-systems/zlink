'use strict';

const path = require('node:path');

const WARMUP_ITERATIONS = 5000;
const MEASURE_ITERATIONS = 10000;
const ROUNDS = 9;
const CHANNEL_NAME = 'channel-header-index-cost';
const PACKET_NAME = 'ChannelHeaderIndexCostPayload';
const CORRELATION_ID = '00112233445566778899aabbccddeeff';
const DEADLINE_UNIX_MS = 2_000_000_000_000;
const PAYLOAD = { body: 'x'.repeat(1013) };
const FLOW = {
  flowId: '00000000-0000-7000-8000-000000000001',
  flowOrigin: 'Application'
};
const CASES = [
  { name: 'command/no-flow', kind: 'command', flow: undefined },
  { name: 'request/no-flow', kind: 'request', flow: undefined },
  { name: 'reply/no-flow', kind: 'reply', flow: undefined },
  { name: 'command/application-flow', kind: 'command', flow: FLOW },
  { name: 'request/application-flow', kind: 'request', flow: FLOW },
  { name: 'reply/application-flow', kind: 'reply', flow: FLOW }
];

const [baselineArgument, currentArgument] = process.argv.slice(2);
if (baselineArgument === undefined || currentArgument === undefined) {
  throw new Error('usage: node channel-header-index-cost.cjs <baseline-dist> <current-dist>');
}

function loadRuntime(distArgument) {
  const dist = path.resolve(distArgument);
  return {
    codec: require(path.join(dist, 'runtime/channels/channel-envelope.js')),
    flowContext: require(path.join(dist, 'runtime/diagnostics/flow-context.js')),
    ZLinkBufferMessage: require(path.join(dist, 'runtime/backend/runtime-message.js'))
      .ZLinkBufferMessage
  };
}

const runtimes = {
  baseline: loadRuntime(baselineArgument),
  current: loadRuntime(currentArgument)
};

function createCall(runtime, scenario, flow) {
  const codec = runtime.codec;
  const codecs = { serializers: new Map() };
  let sink = 0;
  const keepResult = (parts) => {
    sink += parts[0].byteLength + parts[1].byteLength;
  };
  const encodeRequest = () =>
    codec.encodeChannelEnvelopePartsAtDeadline(
      1,
      CHANNEL_NAME,
      PACKET_NAME,
      PAYLOAD,
      DEADLINE_UNIX_MS,
      undefined,
      codecs,
      CORRELATION_ID,
      flow !== undefined
    );
  let call;
  const prepare = () => {
    if (scenario === 'command') {
      call = () =>
        keepResult(
          codec.encodeChannelEnvelopePartsAtDeadline(
            3,
            CHANNEL_NAME,
            PACKET_NAME,
            PAYLOAD,
            undefined,
            undefined,
            codecs,
            undefined,
            flow !== undefined
          )
        );
    } else if (scenario === 'request') {
      call = () => keepResult(encodeRequest());
    } else {
      const requestParts = encodeRequest();
      const request = codec.decodeChannelHeader(
        requestParts.map((part) => runtime.ZLinkBufferMessage.fromOwned(part))
      );
      call = () => keepResult(codec.encodeChannelReplyParts(request, PAYLOAD, codecs));
    }
  };
  if (flow === undefined) prepare();
  else runtime.flowContext.runWithFlow(flow, prepare);

  return () => {
    const warm = () => {
      for (let index = 0; index < WARMUP_ITERATIONS; index++) call();
    };
    const measure = () => {
      warm();
      const cpuStart = process.cpuUsage();
      const wallStart = process.hrtime.bigint();
      for (let index = 0; index < MEASURE_ITERATIONS; index++) call();
      const wallNs = Number(process.hrtime.bigint() - wallStart);
      const cpu = process.cpuUsage(cpuStart);
      return {
        warmupIterations: WARMUP_ITERATIONS,
        measureIterations: MEASURE_ITERATIONS,
        wallNs,
        processCpuNs: (cpu.user + cpu.system) * 1000,
        sink
      };
    };
    return flow === undefined ? measure() : runtime.flowContext.runWithFlow(flow, measure);
  };
}

function median(values) {
  const ordered = [...values].sort((left, right) => left - right);
  return ordered[Math.floor(ordered.length / 2)];
}

const samples = [];
for (let round = 0; round < ROUNDS; round++) {
  for (let caseIndex = 0; caseIndex < CASES.length; caseIndex++) {
    const scenario = CASES[caseIndex];
    const order = (round + caseIndex) % 2 === 0 ? ['baseline', 'current'] : ['current', 'baseline'];
    for (const backend of order) {
      const elapsed = createCall(runtimes[backend], scenario.kind, scenario.flow)();
      samples.push({ round: round + 1, case: scenario.name, backend, ...elapsed });
    }
  }
}

const summary = [];
for (const scenario of CASES) {
  const row = { case: scenario.name };
  for (const backend of ['baseline', 'current']) {
    const selected = samples.filter(
      (sample) => sample.case === scenario.name && sample.backend === backend
    );
    row[backend] = {
      medianWallNsPerEncode: median(
        selected.map((sample) => sample.wallNs / sample.measureIterations)
      ),
      medianProcessCpuNsPerEncode: median(
        selected.map((sample) => sample.processCpuNs / sample.measureIterations)
      )
    };
  }
  row.currentOverBaselineWall =
    row.current.medianWallNsPerEncode / row.baseline.medianWallNsPerEncode;
  row.currentOverBaselineProcessCpu =
    row.current.medianProcessCpuNsPerEncode / row.baseline.medianProcessCpuNsPerEncode;
  summary.push(row);
}

process.stdout.write(
  `${JSON.stringify(
    {
      node: process.version,
      platform: process.platform,
      arch: process.arch,
      baselineDist: path.resolve(baselineArgument),
      currentDist: path.resolve(currentArgument),
      rounds: ROUNDS,
      caseOrderAlternatesByRound: true,
      payloadJsonBytes: Buffer.byteLength(JSON.stringify(PAYLOAD)),
      summary,
      samples
    },
    null,
    2
  )}\n`
);
