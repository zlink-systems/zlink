// SPDX-License-Identifier: MPL-2.0
'use strict';

const { parentPort, workerData } = require('node:worker_threads');
const zlink = require('@zlink-systems/zlink');
const {
  RESPONSE_ENVELOPE,
  encodeBenchPayloadMessage,
  decodeBenchPayloadBody
} = require('../shared/raw-wire');
const { PHASE_ACTIVE, decode, nowNs } = require('../shared/bench-metric-header');
const { EchoHandler, CommandHandler } = require('./bench-handlers');

const counters = new Int32Array(workerData.shared, 0, 3);
const latencyCount = new Int32Array(workerData.shared, 12, 1);
const latencies = new Float64Array(workerData.shared, 16, 500000);
const metrics = {
  record(payload) {
    Atomics.add(counters, 1, 1);
    const decoded = decode(payload);
    if (decoded === null || decoded.phase !== PHASE_ACTIVE) return;
    Atomics.add(counters, 0, 1);
    const index = Atomics.add(latencyCount, 0, 1);
    if (index < latencies.length) {
      const elapsed = nowNs() - decoded.sentTimestampNs;
      latencies[index] = elapsed > 0n ? Number(elapsed) : 0;
    }
  },
  recordError() { Atomics.add(counters, 2, 1); }
};

const echoHandler = new EchoHandler(metrics);
const commandHandler = new CommandHandler(metrics);
const context = zlink.createContext();
const router = zlink.createRouterSocket(context);
router.setRoutingId(zlink.RoutingId.from(Buffer.from('bench-server', 'ascii')));
router.options.mandatory = true;
router.bind(workerData.endpoint);
parentPort.postMessage({ ready: true });
const received = new zlink.Received();

const responseHeader = workerData.frameworkWire
  ? (bytes) => {
    const header = JSON.parse(bytes.subarray(1).toString('utf8'));
    header.kind = 2;
    return Buffer.concat([Buffer.from([0xf2]), Buffer.from(JSON.stringify(header), 'utf8')]);
  }
  : () => RESPONSE_ENVELOPE;

function dispatchOne() {
  const parts = received.parts;
  try {
    const header = parts[0].data();
    const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    if (payload === null) throw new Error('invalid benchmark payload');
    if (received.replyToken !== null) {
      echoHandler.handleL0({ body: payload });
      received.reply()
        .message(responseHeader(header))
        .message(encodeBenchPayloadMessage(payload))
        .submit();
    } else {
      commandHandler.handleL0({ body: payload });
    }
  } catch (error) {
    metrics.recordError();
    parentPort.postMessage({ error: error.message, counted: true });
  } finally {
    received.close();
  }
}

async function receive() {
  try {
    for (;;) {
      if (!router.recv(received, zlink.RecvFlags.None)) continue;
      do {
        dispatchOne();
      } while (router.recv(received, zlink.RecvFlags.DontWait));
    }
  } finally {
    router.close();
    context.close();
  }
}

receive().catch((error) => {
  parentPort.postMessage({ error: error.stack || error.message });
  process.exitCode = 1;
});
