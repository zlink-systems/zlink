// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createPayload,
  createRunId,
  HEADER_SIZE,
  currentEpochNs,
  summarizeMetrics,
  stampPayload
} = require('../common/perf_metrics');
const { configureTlsClient } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const {
  POLLIN,
  POLLOUT,
  applyAutoHwmMsgUnit,
  applyContextPolicy,
  applySocketPolicy,
  emitMultiSocketHwmDetail,
  pollEvents,
  pollEventHas,
  recvNoWaitInto,
  sendStopTokenOnce,
  trySocketSend,
  waitForConnectionReady
} = require('./perf_multi_runtime');

const SERVER_ID = Buffer.from('multi-router-router-server', 'ascii');
const SERVER_ROUTING_ID = zlink.RoutingId.from(SERVER_ID);

async function main() {
  const options = parseMultiArgs(process.argv.slice(2));
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'client', 'MULTI_ROUTER_ROUTER');
  const routers = [];
  const payloads = [];
  const replyMessages = [];
  const waiting = [];
  const sendPending = [];
  const poller = zlink.createPoller();
  const pollBuffer = zlink.createPollEvents(Math.max(1, options.clients));

  try {
    for (let i = 0; i < options.clients; i += 1) {
      const router = zlink.createRouterSocket(ctx);
      applySocketPolicy(router);
      configureTlsClient(router, options.transport);
      router.setRoutingId(
        zlink.RoutingId.from(Buffer.from(`multi-router-client-${i}`, 'ascii'))
      );
      routers.push(router);
      payloads.push(createPayload(options.msgSize));
      replyMessages.push(new zlink.Received());
      waiting.push(false);
      sendPending.push(false);
    }
    for (let i = 0; i < routers.length; i += 1) {
      await waitForConnectionReady(routers[i], () => routers[i].connect(options.endpoint));
      applyAutoHwmMsgUnit(ctx, options.msgSize);
      poller.add(routers[i], pollEvents(POLLIN | POLLOUT), i);
    }
    ctx.recalculateAutoHwm();
    for (const router of routers) {
      emitMultiSocketHwmDetail(router, 'endpoint', options.transport, options.msgSize);
    }
    const runId = createRunId(1);
    const activeStartNs = currentEpochNs();
    const activeStopNs = activeStartNs + BigInt(Math.floor(options.duration * 1_000_000_000));
    const collector = createMetricCollector({
      runId,
      msgSize: options.msgSize,
      activeStartNs,
      activeStopNs,
      roundTrip: true,
    });
    let seq = 1n;

    const drainReply = (index) => {
      let progressed = false;
      while (true) {
        const echoed = replyMessages[index];
        if (!recvNoWaitInto(routers[index], echoed)) {
          break;
        }
        waiting[index] = false;
        collector.recordPayload(echoed.parts[0].data(), currentEpochNs());
        progressed = true;
      }
      return progressed;
    };
    while (currentEpochNs() < activeStopNs) {
      let progressed = false;
      for (let i = 0; i < routers.length; i += 1) {
        if (waiting[i] || sendPending[i]) {
          continue;
        }
        stampPayload(payloads[i], { phase: 1, runId, msgSize: options.msgSize, seq });
        const sent = trySocketSend(routers[i], SERVER_ROUTING_ID, payloads[i]);
        if (!sent) {
          sendPending[i] = true;
          continue;
        }
        waiting[i] = true;
        seq += 1n;
        progressed = true;
      }
      for (let i = 0; i < routers.length; i += 1) {
        progressed = drainReply(i) || progressed;
      }
      if (progressed) {
        continue;
      }

      // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven `-1` wait.
      const readyCount = poller.wait(
        pollBuffer,
        process.platform === 'win32' ? 50 : -1
      );
      if (readyCount === 0) {
        continue;
      }
      for (let offset = 0; offset < readyCount; offset += 1) {
        const index = pollBuffer.slot(offset);
        if (!Number.isInteger(index) || index < 0 || index >= routers.length) {
          continue;
        }
        const event = { revents: pollBuffer.revents(offset) };
        if (pollEventHas(event, POLLOUT)) {
          sendPending[index] = false;
        }
        if (pollEventHas(event, POLLIN)) {
          drainReply(index);
        }
      }
    }

    // PERF_MULTI_TEST_POLICY § 1.3.1: signal phase end via wire stop token.
    await sendStopTokenOnce(
      routers[0],
      (bytes) => trySocketSend(routers[0], SERVER_ROUTING_ID, bytes)
    );

    const result = await collector.finish();
    for (const metricLine of summarizeMetrics(
      'MULTI_ROUTER_ROUTER',
      options.transport,
      options.msgSize,
      result.latenciesNs,
      options.duration,
      'current',
      result.accepted
    )) {
      console.log(metricLine);
    }
  } finally {
    pollBuffer.close();
    poller.close();
    for (const reply of replyMessages) {
      reply.close();
    }
    for (const router of routers) {
      router.close();
    }
    ctx.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
