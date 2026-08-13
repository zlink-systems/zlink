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

const SERVER_ID = Buffer.from('SERVER', 'ascii');
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
      router.options.setConnectRoutingId(SERVER_ROUTING_ID);
      routers.push(router);
      payloads.push(createPayload(options.msgSize));
      replyMessages.push(new zlink.Received());
      waiting.push(false);
      sendPending.push(false);
    }
    for (let i = 0; i < routers.length; i += 1) {
      await waitForConnectionReady(routers[i], () => routers[i].connect(options.endpoint));
      applyAutoHwmMsgUnit(ctx, options.msgSize);
      poller.add(routers[i], pollEvents(POLLIN), i);
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
      const echoed = replyMessages[index];
      if (!recvNoWaitInto(routers[index], echoed)) {
        return false;
      }
      waiting[index] = false;
      collector.recordPayload(echoed.parts[0].data(), currentEpochNs());
      // HOT PATH: each ROUTER client has one request in flight.  C waits for
      // this socket's POLLIN event, receives one reply, then permits its next
      // send.  Do not probe every socket with recv(DONT_WAIT) each round.
      return true;
    };
    while (currentEpochNs() < activeStopNs) {
      for (let i = 0; i < routers.length; i += 1) {
        if (waiting[i] || sendPending[i]) {
          continue;
        }
        stampPayload(payloads[i], { phase: 1, runId, msgSize: options.msgSize, seq });
        const sent = trySocketSend(routers[i], SERVER_ROUTING_ID, payloads[i]);
        if (!sent) {
          sendPending[i] = true;
          // HOT PATH: match the C requester and subscribe to POLLOUT only
          // for a socket that actually backpressured. An always-writable
          // socket otherwise wakes the poller while replies are pending.
          poller.modify(routers[i], pollEvents(POLLIN | POLLOUT));
          continue;
        }
        waiting[i] = true;
        seq += 1n;
      }

      // Match C's active window: wait only for registered socket readiness
      // and never probe a non-ready socket through the binding boundary.
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
        if (!Number.isInteger(index) || index < 0 || index >= routers.length) {
          continue;
        }
        const event = { revents: pollBuffer.revents(offset) };
        if (pollEventHas(event, POLLOUT)) {
          sendPending[index] = false;
          // The next send is attempted eagerly; until it backpressures again,
          // only a reply should wake the socket's poll registration.
          poller.modify(routers[index], pollEvents(POLLIN));
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
