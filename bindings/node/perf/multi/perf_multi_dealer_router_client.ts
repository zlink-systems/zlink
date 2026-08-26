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
  applyContextPolicy,
  applySocketPolicy,
  emitMultiSocketHwmDetail,
  pollEvents,
  pollEventHas,
  recvNoWaitInto,
  sendStopTokenOnce,
  tryRoutedSocketSend,
  measurementPayload,
  waitForConnectionReady
} = require('./perf_multi_runtime');
const {
  resolveRoutedPattern,
  runRoutedSendSendClient
} = require('./perf_multi_routed_sendsend');

const PATTERN = 'MULTI_DEALER_ROUTER_REQREP';

async function main() {
  const options = parseMultiArgs(process.argv.slice(2));
  const pattern = resolveRoutedPattern(
    process.env.PERF_MULTI_PATTERN,
    'DEALER_ROUTER'
  );
  if (pattern.endsWith('_SENDSEND')) {
    await runRoutedSendSendClient({
      options,
      pattern,
      routerClient: false
    });
    return;
  }
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'client', PATTERN);
  const dealers = [];
  const payloads = [];
  const replyMessages = [];
  const waiting = [];
  const sendPending = [];
  const poller = zlink.createPoller();
  const pollBuffer = zlink.createPollEvents(Math.max(1, options.clients));

  try {
    for (let i = 0; i < options.clients; i += 1) {
      const dealer = zlink.createDealerSocket(ctx);
      applySocketPolicy(dealer);
      configureTlsClient(dealer, options.transport);
      dealer.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${i}`, 'ascii')));
      dealers.push(dealer);
      payloads.push(createPayload(options.msgSize));
      replyMessages.push(new zlink.Received());
      waiting.push(false);
      sendPending.push(false);
    }
    for (let i = 0; i < dealers.length; i += 1) {
      await waitForConnectionReady(dealers[i], () => dealers[i].connect(options.endpoint));
      poller.add(dealers[i], pollEvents(POLLIN), i);
    }
    ctx.recalculateAutoHwm();
    for (const dealer of dealers) {
      emitMultiSocketHwmDetail(dealer, 'endpoint', options.transport, options.msgSize);
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
      const echoed = replyMessages[index];
      if (!recvNoWaitInto(dealers[index], echoed)) {
        return false;
      }
      waiting[index] = false;
      const payload = measurementPayload(echoed.parts);
      if (!payload) throw new Error('invalid multipart echo reply');
      collector.recordPayload(payload.data(), currentEpochNs());
      // HOT PATH: C receives only after this socket's POLLIN event, then
      // allows its next send.  Avoid probing every non-ready socket through
      // the Node/native boundary on each round.
      return true;
    };
    while (currentEpochNs() < activeStopNs) {
      for (let i = 0; i < dealers.length; i += 1) {
        if (waiting[i] || sendPending[i]) {
          continue;
        }
        stampPayload(payloads[i], { phase: 1, runId, msgSize: options.msgSize, seq });
        const sent = await tryRoutedSocketSend(dealers[i], payloads[i]);
        if (!sent) {
          sendPending[i] = true;
          // HOT PATH: C registers POLLOUT only after this socket reports
          // backpressure. Keeping writable sockets in every wait turns the
          // round-trip loop into a busy poll and hides binding receive cost.
          poller.modify(dealers[i], pollEvents(POLLIN | POLLOUT));
          continue;
        }
        waiting[i] = true;
        seq += 1n;
      }

      // Match C's active deadline while waiting only for registered readiness.
      const remainingMs = Math.ceil(
        (Number(activeStopNs) - Number(currentEpochNs())) / 1_000_000
      );
      if (remainingMs <= 0) {
        break;
      }
      const readyCount = poller.wait(
        pollBuffer,
        Math.max(1, Math.min(remainingMs, 2_147_483_647))
      );
      if (readyCount === 0) {
        continue;
      }
      for (let offset = 0; offset < readyCount; offset += 1) {
        const index = pollBuffer.slot(offset);
        if (!Number.isInteger(index) || index < 0 || index >= dealers.length) {
          continue;
        }
        const event = { revents: pollBuffer.revents(offset) };
        if (pollEventHas(event, POLLOUT)) {
          sendPending[index] = false;
          // The eager send loop performs the next attempt. Remove POLLOUT
          // first so sockets awaiting replies wake this poller only on data.
          poller.modify(dealers[index], pollEvents(POLLIN));
        }
        if (pollEventHas(event, POLLIN)) {
          drainReply(index);
        }
      }
    }

    // PERF_MULTI_TEST_POLICY § 1.3.1: signal phase end to the echo server
    // via the wire-level stop token. The server's recv loop exits on the
    // first stop token observed.
    await sendStopTokenOnce(
      dealers[0],
      (bytes) => tryRoutedSocketSend(dealers[0], [bytes])
    );

    const result = await collector.finish();
    for (const metricLine of summarizeMetrics(
      PATTERN,
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
    for (const dealer of dealers) {
      dealer.close();
    }
    ctx.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
