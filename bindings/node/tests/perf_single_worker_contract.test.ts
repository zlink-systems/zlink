// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const zlink = require('@zlink-systems/zlink');
const {
  applyContextPolicy,
  applySocketPolicy,
  benchmarkEndpoint,
  closeSenderWorker,
  configureTlsClient,
  releaseSenderWorker,
  spawnSenderWorker,
  waitForMonitorConnectionReady,
  waitForPostReadySettle,
  workerBindEndpoint,
  workerBoundEndpoint,
} = require('../perf/single/perf_single_common');
const { STOP_TOKEN_BYTES } = require('../perf/perf_stop_token');

function waitForWorkerEvent(worker, expectedType: string, signal: AbortSignal) {
  signal.throwIfAborted();
  return new Promise<void>((resolve, reject) => {
    const cleanup = () => {
      worker.off('message', onMessage);
      worker.off('error', onError);
      worker.off('exit', onExit);
      signal.removeEventListener('abort', onAbort);
    };
    const onError = (error) => {
      cleanup();
      reject(error);
    };
    const onMessage = (message) => {
      if (message.type === 'error') {
        onError(new Error(message.message));
      } else if (message.type === expectedType) {
        cleanup();
        resolve();
      }
    };
    const onExit = (code) => onError(
      new Error(`sender worker exited with code ${code} before ${expectedType}`)
    );
    const onAbort = () => onError(signal.reason);
    worker.on('message', onMessage);
    worker.once('error', onError);
    worker.once('exit', onExit);
    signal.addEventListener('abort', onAbort, { once: true });
  });
}

test('worker event wait observes readiness, errors, exit and cancellation', async () => {
  for (const outcome of ['bound', 'reported-error', 'error', 'exit', 'abort']) {
    const worker = new EventEmitter();
    const controller = new AbortController();
    const waiting = waitForWorkerEvent(worker, 'bound', controller.signal);
    worker.emit('message', { type: 'connected' });
    if (outcome === 'bound') {
      worker.emit('message', { type: 'bound' });
      await waiting;
    } else {
      const failure = new Error('startup failed');
      if (outcome === 'reported-error') {
        worker.emit('message', { type: 'error', message: failure.message });
      } else if (outcome === 'error') {
        worker.emit('error', failure);
      } else if (outcome === 'exit') {
        worker.emit('exit', 0);
      } else {
        controller.abort(failure);
      }
      await assert.rejects(waiting, outcome === 'exit'
        ? /exited with code 0 before bound/ : /startup failed/);
    }
    assert.equal(worker.listenerCount('message'), 0);
    assert.equal(worker.listenerCount('error'), 0);
    assert.equal(worker.listenerCount('exit'), 0);
  }
});

test('WSS PUB worker survives blocking flow control and delivers its wire stop', { timeout: 120_000 }, async (t) => {
  const endpoint = await benchmarkEndpoint(
    'wss',
    `node-single-pubsub-contract-${process.pid}-${Date.now()}`
  );
  const ctx = zlink.createContext();
  applyContextPolicy(ctx);
  const sub = zlink.createSubSocket(ctx);
  const monitor = sub.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
  let worker = null;

  try {
    applySocketPolicy(sub, {
      recvTimeoutMs: 100,
      recvHwm: 4_096,
      policySocketOverrides: true,
    });
    ctx.recalculateAutoHwm();
    configureTlsClient(sub, 'wss');
    sub.setSubscription('');

    worker = spawnSenderWorker({
      kind: 'pubsub',
      transport: 'wss',
      endpoint: workerBindEndpoint(endpoint),
      duration: 0.1,
      msgSize: 1024,
      runId: 1,
      topic: 'bench',
      options: {
        sendTimeoutMs: 25,
        recvTimeoutMs: 100,
        sendHwm: 4_096,
        policySocketOverrides: true,
        noDrop: true,
      },
    });
    await waitForWorkerEvent(worker, 'bound', t.signal);
    sub.connect(workerBoundEndpoint(worker, endpoint));
    waitForMonitorConnectionReady(monitor, 2_000);
    await waitForWorkerEvent(worker, 'connected', t.signal);
    waitForPostReadySettle(1_000);
    releaseSenderWorker(worker);

    // Hold the subscriber briefly so PUB/NODROP reaches its tiny HWM and the
    // blocking publish terminal returns SNDTIMEO at least once. The worker must
    // retry the same logical sample and still terminate via the wire stop.
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 250);

    const received = new zlink.TopicMessage();
    let sawPayload = false;
    let sawStop = false;
    const deadline = Date.now() + 5_000;
    try {
      while (!sawStop && Date.now() < deadline) {
        try {
          if (!sub.subscribe(received, zlink.RecvFlags.None)) continue;
        } catch (error) {
          if (error instanceof zlink.RecvError
              && error.result === zlink.RecvResult.NoData) {
            continue;
          }
          throw error;
        }
        if (received.parts.length === 1
            && received.parts[0].data().equals(STOP_TOKEN_BYTES)) {
          sawStop = true;
        } else {
          assert.equal(received.parts.length, 2);
          assert.equal(received.parts[0].data().length, 1024);
          assert.equal(received.parts[1].data().length, 0);
          sawPayload = true;
        }
      }
    } finally {
      received.close();
    }

    assert.equal(sawPayload, true);
    assert.equal(sawStop, true);
    await waitForWorkerEvent(worker, 'done', t.signal);
  } finally {
    await closeSenderWorker(worker);
    monitor.close();
    sub.close();
    ctx.close();
  }
});

test('single REQREP worker starts with complete data and echoes two parts', { timeout: 120_000 }, async (t) => {
  const serverRoutingId = zlink.RoutingId.from(Buffer.from('SERVER'));
  for (const routedClient of [false, true]) {
    const endpoint = await benchmarkEndpoint(
      'tcp',
      `node-single-reqrep-contract-${routedClient ? 'router' : 'dealer'}-${process.pid}`
    );
    const ctx = zlink.createContext();
    applyContextPolicy(ctx);
    const client = routedClient
      ? zlink.createRouterSocket(ctx)
      : zlink.createDealerSocket(ctx);
    const monitor = client.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    let worker = null;
    try {
      applySocketPolicy(client, { recvTimeoutMs: 100 });
      if (routedClient) {
        client.setRoutingId(zlink.RoutingId.from(Buffer.from('CLIENT')));
        client.options.setConnectRoutingId(serverRoutingId);
        client.options.mandatory = true;
      }
      ctx.recalculateAutoHwm();
      worker = spawnSenderWorker({
        kind: 'socket_reqrep_replier',
        transport: 'tcp',
        endpoint: workerBindEndpoint(endpoint),
        duration: 0,
        msgSize: 1024,
        runId: 1,
        options: { recvTimeoutMs: 100 },
      });
      await waitForWorkerEvent(worker, 'bound', t.signal);
      client.connect(workerBoundEndpoint(worker, endpoint));
      waitForMonitorConnectionReady(monitor, 2_000);
      releaseSenderWorker(worker);

      const request = routedClient
        ? client.request(serverRoutingId)
        : client.request();
      const reply = request
        .message(Buffer.alloc(1024, 7))
        .message(Buffer.alloc(0))
        .timeout(1_000)
        .submit_sync();
      try {
        assert.equal(reply.length, 2);
        assert.equal(reply[0].data().length, 1024);
        assert.equal(reply[1].data().length, 0);
      } finally {
        for (const part of reply) part.close();
      }

      const stop = routedClient ? client.send(serverRoutingId) : client.send();
      stop.message(STOP_TOKEN_BYTES).submit_sync();
      await waitForWorkerEvent(worker, 'done', t.signal);
    } finally {
      await closeSenderWorker(worker);
      monitor.close();
      client.close();
      ctx.close();
    }
  }
});
