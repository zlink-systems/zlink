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
const latencies = new Float64Array(workerData.shared, 16, 500000);
const metrics = {
  record(payload) {
    Atomics.add(counters, 1, 1);
    const decoded = decode(payload);
    if (decoded === null || decoded.phase !== PHASE_ACTIVE) return;
    const index = Atomics.add(counters, 0, 1);
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
const poller = zlink.createPoller();
const events = zlink.createPollEvents(1);
poller.add(router, [zlink.PollEventFlag.PollIn], 0);
parentPort.postMessage({ ready: true });
const received = new zlink.Received();

function frameworkResponseHeader(bytes) {
  const header = JSON.parse(bytes.subarray(1).toString('utf8'));
  header.kind = 2;
  return Buffer.concat([Buffer.from([0xf2]), Buffer.from(JSON.stringify(header), 'utf8')]);
}

function dispatchOne() {
  const parts = received.parts;
  try {
    const header = workerData.frameworkWire ? parts[0].data() : null;
    const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    if (payload === null) throw new Error('invalid benchmark payload');
    if (received.replyToken !== null) {
      echoHandler.handleL0(payload);
      received.reply()
        .message(workerData.frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
        .message(encodeBenchPayloadMessage(payload))
        .submit();
    } else {
      commandHandler.handleL0(payload);
    }
  } catch (error) {
    metrics.recordError();
    parentPort.postMessage({ error: error.message, counted: true });
  }
}

async function receive() {
  try {
    for (;;) {
      poller.wait(events, -1);
      while (tryRecv()) dispatchOne();
    }
  } finally {
    events.close();
    poller.close();
    router.close();
    context.close();
  }
}

function tryRecv() {
  try {
    return router.recv(received, zlink.RecvFlags.DontWait);
  } catch (error) {
    if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) return false;
    throw error;
  }
}

receive().catch((error) => {
  parentPort.postMessage({ error: error.stack || error.message });
  process.exitCode = 1;
});
