// SPDX-License-Identifier: MPL-2.0

'use strict';

const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createPayload,
  createRunId,
  HEADER_SIZE,
  currentEpochNs,
  stampPayload,
  summarizeMetrics
} = require('../common/perf_metrics');
const { configureTlsClient, configureTlsServer } = require('../common/perf_tls');
const { STOP_TOKEN_BYTES, isStopTokenParts } = require('../perf_stop_token');
const { parseMultiArgs } = require('./perf_multi_common');
const {
  POLLIN,
  POLLOUT,
  applyContextPolicy,
  applySocketPolicy,
  emitMultiSocketHwmDetail,
  measurementPayload,
  pollEvents,
  pollEventHas,
  recvNoWaitInto,
  tryRoutedSocketSend,
  waitForConnectionReady,
  waitForConnectionReadyCount,
  waitPollerOne
} = require('./perf_multi_runtime');

const SERVER_ROUTING_ID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));

function resolveRoutedPattern(pattern, family) {
  const base = `MULTI_${family}`;
  const normalized = String(pattern || `${base}_REQREP`).trim().toUpperCase();
  if (normalized === base || normalized === `${base}_SENDSEND`) {
    return `${base}_SENDSEND`;
  }
  return `${base}_REQREP`;
}

function createClientSocket(ctx, routerClient) {
  return routerClient
    ? zlink.createRouterSocket(ctx)
    : zlink.createDealerSocket(ctx);
}

function sendPayload(socket, routerClient, payload, control = false) {
  const frames = control ? [payload] : payload;
  return routerClient
    ? tryRoutedSocketSend(socket, SERVER_ROUTING_ID, frames)
    : tryRoutedSocketSend(socket, frames);
}

async function sendStopTokenWithRetry(socket, routerClient, poller, pollBuffer) {
  const deadline = Date.now() + 5000;
  while (!sendPayload(socket, routerClient, STOP_TOKEN_BYTES, true)) {
    if (Date.now() >= deadline) {
      throw new Error('stop token send timeout');
    }
    poller.wait(pollBuffer, 50);
  }
}

async function runRoutedSendSendClient({ options, pattern, routerClient }) {
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'client', pattern);
  const sockets = [];
  const payloads = [];
  const sendPending = [];
  const replies = [];
  const poller = zlink.createPoller();
  const pollBuffer = zlink.createPollEvents(Math.max(1, options.clients));
  let rl = null;

  try {
    for (let i = 0; i < options.clients; i += 1) {
      const socket = createClientSocket(ctx, routerClient);
      applySocketPolicy(socket, { transport: options.transport });
      configureTlsClient(socket, options.transport);
      if (routerClient) {
        socket.setRoutingId(
          zlink.RoutingId.from(Buffer.from(`multi-router-client-${i}`, 'ascii'))
        );
        socket.options.setConnectRoutingId(SERVER_ROUTING_ID);
      } else {
        socket.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${i}`, 'ascii')));
      }
      sockets.push(socket);
      payloads.push(createPayload(options.msgSize));
      sendPending.push(false);
      replies.push(new zlink.Received());
    }

    for (let i = 0; i < sockets.length; i += 1) {
      await waitForConnectionReady(sockets[i], () => sockets[i].connect(options.endpoint));
      poller.add(sockets[i], pollEvents(POLLIN), i);
    }
    ctx.recalculateAutoHwm();
    for (const socket of sockets) {
      emitMultiSocketHwmDetail(socket, 'endpoint', options.transport, options.msgSize);
    }

    console.log(`CLIENT_READY,${options.msgSize}`);
    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    for await (const line of rl) {
      if (line === `START,${options.msgSize}`) {
        break;
      }
      if (line === 'STOP' || line === 'QUIT') {
        return;
      }
    }

    const runId = createRunId(1);
    const activeStartNs = currentEpochNs();
    const activeStopNs = activeStartNs
      + BigInt(Math.floor(options.duration * 1_000_000_000));
    const collector = createMetricCollector({
      suite: 'multi',
      runId,
      msgSize: options.msgSize,
      activeStartNs,
      activeStopNs,
      roundTrip: true
    });
    let seq = 1n;

    while (currentEpochNs() < activeStopNs) {
      for (let i = 0; i < sockets.length; i += 1) {
        if (sendPending[i]) {
          continue;
        }
        if (currentEpochNs() >= activeStopNs) {
          break;
        }
        stampPayload(payloads[i], {
          phase: 1,
          runId,
          msgSize: options.msgSize,
          seq
        });
        const sent = sendPayload(sockets[i], routerClient, payloads[i]);
        if (!sent) {
          sendPending[i] = true;
          poller.modify(sockets[i], pollEvents(POLLIN | POLLOUT));
          continue;
        }
        seq += 1n;
      }

      const remainingMs = Math.ceil(
        (Number(activeStopNs) - Number(currentEpochNs())) / 1_000_000
      );
      if (remainingMs <= 0) {
        break;
      }
      const readyCount = poller.wait(pollBuffer,
        sendPending.every(Boolean) ? Math.max(1, Math.min(remainingMs, 2_147_483_647)) : 0);
      for (let offset = 0; offset < readyCount; offset += 1) {
        const index = pollBuffer.slot(offset);
        if (!Number.isInteger(index) || index < 0 || index >= sockets.length) {
          continue;
        }
        const event = { revents: pollBuffer.revents(offset) };
        if (pollEventHas(event, POLLIN)) {
          const reply = replies[index];
          while (recvNoWaitInto(sockets[index], reply)) {
            try {
              const payload = measurementPayload(reply.parts);
              if (!payload) throw new Error('invalid multipart echo reply');
              collector.recordPayload(payload.data(), currentEpochNs());
            } finally {
              reply.close();
            }
          }
        }
        if (pollEventHas(event, POLLOUT)) {
          sendPending[index] = false;
          poller.modify(sockets[index], pollEvents(POLLIN));
        }
      }
    }

    // Send one wire-level stop token per client socket after the active
    // window. The receiver's tail drain consumes queued payloads and tokens.
    for (const socket of sockets) {
      await sendStopTokenWithRetry(socket, routerClient, poller, pollBuffer);
    }
    const result = await collector.finish();
    for (const metricLine of summarizeMetrics(
      pattern,
      options.transport,
      options.msgSize,
      result.latenciesNs,
      options.duration,
      'current',
      result.accepted,
      result.latencyMeanNs
    )) {
      console.log(metricLine);
    }
    console.log(`CLIENT_DONE,${options.msgSize}`);
  } finally {
    rl?.close();
    pollBuffer.close();
    poller.close();
    for (const reply of replies) {
      reply.close();
    }
    for (const socket of sockets) {
      socket.close();
    }
    ctx.close();
  }
}

async function runRoutedSendSendServer({ options, pattern, family }) {
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'server', pattern);
  const router = zlink.createRouterSocket(ctx);
  const poller = zlink.createPoller();
  const received = new zlink.Received();
  const pending = [];
  let pollBuffer = null;
  let rl = null;
  let pollMask = POLLIN;

  const drainPending = async () => {
    while (pending.length > 0) {
      const reply = pending[0];
      if (!tryRoutedSocketSend(
        router,
        reply.routingId,
        reply.parts
      )) {
        return;
      }
      pending.shift();
    }
  };

  try {
    applySocketPolicy(router, { transport: options.transport });
    configureTlsServer(router, options.transport);
    if (family === 'ROUTER_ROUTER') {
      router.setRoutingId(SERVER_ROUTING_ID);
    }
    router.bind(options.endpoint);
    ctx.recalculateAutoHwm();
    emitMultiSocketHwmDetail(router, 'endpoint', options.transport, options.msgSize);
    poller.add(router, pollEvents(POLLIN), 0);
    pollBuffer = zlink.createPollEvents(1);
    const readyBarrier = waitForConnectionReadyCount(router, options.clients);
    console.log(`READY,${options.endpoint}`);

    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    for await (const line of rl) {
      if (line !== `START,${options.msgSize}`) {
        if (line === 'STOP' || line === 'QUIT') {
          return;
        }
        continue;
      }

      await readyBarrier;
      let stopTokenCount = 0;
      while (stopTokenCount < options.clients) {
        const ready = waitPollerOne(
          poller,
          pollBuffer,
          process.platform === 'win32' ? 50 : -1
        );
        if (!ready) {
          continue;
        }
        if (pollEventHas(ready, POLLOUT)) {
          await drainPending();
        }
        if (pollEventHas(ready, POLLIN)) {
          while (router.recv(received, zlink.RecvFlags.DontWait)) {
            try {
              if (!received.routingId) {
                throw new Error('routed echo received without routing id');
              }
              if (isStopTokenParts(received.parts)) {
                stopTokenCount += 1;
                continue;
              }
              const expectedParts = process.env.PERF_PART_COUNT === '1' ? 1 : 2;
              if (received.parts.length !== expectedParts
                  || (expectedParts === 2 && received.parts[1].data().length !== 0)) {
                throw new Error('invalid multipart echo request');
              }
              if (pending.length === 0
                  && tryRoutedSocketSend(
                    router,
                    received.routingId,
                    received.parts
                  )) {
                continue;
              }
              pending.push({
                routingId: zlink.RoutingId.from(received.routingId.toBytes()),
                parts: received.parts.map((part) => Buffer.from(part.data()))
              });
            } finally {
              received.close();
            }
          }
          await drainPending();
        }
        const nextPollMask = pending.length > 0 ? POLLIN | POLLOUT : POLLIN;
        if (nextPollMask !== pollMask) {
          poller.modify(router, pollEvents(nextPollMask));
          pollMask = nextPollMask;
        }
      }
      break;
    }
  } finally {
    rl?.close();
    received.close();
    pollBuffer?.close();
    poller.close();
    router.close();
    ctx.close();
  }
}

module.exports = {
  resolveRoutedPattern,
  runRoutedSendSendClient,
  runRoutedSendSendServer
};
