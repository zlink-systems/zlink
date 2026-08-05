// SPDX-License-Identifier: MPL-2.0

'use strict';

const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createRunId,
  currentEpochNs,
  HEADER_SIZE,
  summarizeMetrics
} = require('../common/perf_metrics');
const { integerEnv } = require('../common/perf_args');
const { configureTlsClient } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const {
  POLLIN,
  applyAutoHwmMsgUnit,
  applyContextPolicy,
  applySocketPolicy,
  emitMultiSocketHwmDetail,
  pollEvents,
  pollEventHas,
  waitForConnectionReady
} = require('./perf_multi_runtime');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
const TOPIC = 'bench';

function isStopTokenPayload(buffer, size) {
  if (size !== STOP_TOKEN_BYTES.length) {
    return false;
  }
  for (let i = 0; i < STOP_TOKEN_BYTES.length; i += 1) {
    if (buffer[i] !== STOP_TOKEN_BYTES[i]) {
      return false;
    }
  }
  return true;
}

async function main() {
  const options = parseMultiArgs(process.argv.slice(2));
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'client', 'MULTI_PUBSUB');
  const subs = [];
  const receivedBySub = [];
  let rl = null;
  let collector = null;

  try {
    for (let i = 0; i < options.clients; i += 1) {
      const sub = zlink.createSubSocket(ctx);
      applySocketPolicy(sub);
      configureTlsClient(sub, options.transport);
      sub.setSubscription(TOPIC);
      await waitForConnectionReady(sub, () => sub.connect(options.endpoint));
      applyAutoHwmMsgUnit(ctx, options.msgSize);
      subs.push(sub);
      receivedBySub.push(new zlink.TopicMessage());
    }
    ctx.recalculateAutoHwm();
    for (const sub of subs) {
      emitMultiSocketHwmDetail(sub, 'endpoint', options.transport, options.msgSize);
    }

    console.log(`CLIENT_READY,${options.msgSize}`);
    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    for await (const line of rl) {
      if (line === `START,${options.msgSize}`) {
        const activeStartNs = currentEpochNs();
        const activeStopNs = activeStartNs + BigInt(Math.floor(options.duration * 1_000_000_000));
        collector = createMetricCollector({
          runId: createRunId(1),
          msgSize: options.msgSize,
          activeStartNs,
          activeStopNs,
          latencySampleStride: integerEnv('PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE', 32),
        });
        const poller = zlink.createPoller();
        const pollBuffer = zlink.createPollEvents(Math.max(1, subs.length));
        try {
          for (let i = 0; i < subs.length; i += 1) {
            poller.add(subs[i], pollEvents(POLLIN), i);
          }
          // C parity: bindings/c/perf/multi/src/perf_multi_pubsub_client
          // .cpp run_recv_duration (~166-228). Pure signal-driven `-1`
          // poller wait — NO zlink.Timer, NO duration+2s wall-clock bound.
          // The active window closes on the application clock (the
          // collector enforces the recvTs<=activeStop anchor —
          // perf_measurement.ts ~280) and the phase ends when the
          // server's wire stop token wakes the `-1` wait. The stop token
          // is checked BEFORE decoding the metric header (C lines 76-79 /
          // 203-206; mirrors the already-fixed cpp perf_pubsub_client.cpp
          // run_active_until_stop_token ~239-246).
          let stopReceived = false;
          while (!stopReceived) {
            const readyCount = poller.wait(pollBuffer, -1);
            if (readyCount === 0) {
              continue;
            }
            for (let offset = 0; offset < readyCount; offset += 1) {
              const index = pollBuffer.slot(offset);
              if (!Number.isInteger(index) || index < 0 || index >= subs.length
                  || !pollEventHas({ revents: pollBuffer.revents(offset) }, POLLIN)) {
                continue;
              }
              while (true) {
                const received = receivedBySub[index];
                if (!subs[index].subscribe(received, zlink.RecvFlags.DontWait)) {
                  break;
                }
                const data = received.singlePartOrThrow().data();
                if (isStopTokenPayload(data, data.length)) {
                  stopReceived = true;
                  continue;
                }
                collector.recordPayload(data, currentEpochNs());
              }
            }
          }
        } finally {
          pollBuffer.close();
          poller.close();
        }
        break;
      }
    }

    const result = collector ? await collector.finish() : { latenciesNs: [] };
    for (const line of summarizeMetrics(
      'MULTI_PUBSUB',
      options.transport,
      options.msgSize,
      result.latenciesNs,
      options.duration,
      'current',
      result.accepted
    )) {
      console.log(line);
    }
    console.log(`CLIENT_DONE,${options.msgSize}`);
  } finally {
    rl?.close();
    for (const received of receivedBySub) {
      received.close();
    }
    for (const sub of subs) {
      sub.close();
    }
    ctx.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
