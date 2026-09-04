// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const {
  measurementParts,
  waitForConnectionReady
} = require('../perf/multi/perf_multi_runtime');
const {
  runRoutedSendSendRounds,
  trackPendingReplyTask
} = require('../perf/multi/perf_multi_routed_sendsend');
const { benchmarkEndpoint } = require('../perf/common/perf_endpoint');
const {
  configureTlsClient,
  configureTlsServer
} = require('../perf/common/perf_tls');

function nextTurn(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function within<T>(promise: Promise<T>, timeoutMs = 5_000): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`routed SENDSEND contract timed out after ${timeoutMs}ms`)),
      timeoutMs
    );
    promise.then(
      (value) => { clearTimeout(timer); resolve(value); },
      (error) => { clearTimeout(timer); reject(error); }
    );
  });
}

async function waitForMonitorEvent(
  monitor: { recv(flags?: number): { event: number } | null },
  expectedEvent: number,
  timeoutMs = 5_000
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const event = monitor.recv(zlink.RecvFlags.DontWait);
      if (event?.event === expectedEvent) return;
    } catch (error) {
      if (!(error instanceof zlink.RecvError
          && error.result === zlink.RecvResult.NoData)) {
        throw error;
      }
    }
    await nextTurn();
  }
  throw new Error(`monitor event ${expectedEvent} timed out after ${timeoutMs}ms`);
}

type ReceiveMode = 'reuse' | 'close-reuse' | 'close-fresh';

function advanceReceived(
  received: InstanceType<typeof zlink.Received>,
  mode: ReceiveMode
): InstanceType<typeof zlink.Received> {
  if (mode === 'reuse') {
    return received;
  }
  received.close();
  return mode === 'close-fresh' ? new zlink.Received() : received;
}

async function runRoutedSendSendContract({
  transport,
  clientCount,
  sendsPerClient,
  payloadSize,
  receiveMode = 'reuse'
}: {
  transport: 'tcp' | 'wss';
  clientCount: number;
  sendsPerClient: number;
  payloadSize: number;
  receiveMode?: ReceiveMode;
}): Promise<void> {
  const serverContext = zlink.createContext();
  const clientContext = zlink.createContext();
  serverContext.options.ioThreads = 4;
  clientContext.options.ioThreads = 4;
  serverContext.options.autoHwmEnabled = true;
  clientContext.options.autoHwmEnabled = true;
  const router = zlink.createRouterSocket(serverContext);
  const dealers = Array.from({ length: clientCount }, () => zlink.createDealerSocket(clientContext));
  let serverReceived = new zlink.Received();
  const clientReceived = dealers.map(() => new zlink.Received());
  const expected = dealers.length * sendsPerClient;
  const pendingReplies: Promise<void>[] = [];

  try {
    const endpoint = await benchmarkEndpoint(
      transport,
      `routed-sendsend-contract-${process.pid}`,
      { suite: 'multi' }
    );
    configureTlsServer(router, transport);
    router.bind(endpoint);
    await Promise.all(dealers.map((dealer, index) => {
      dealer.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${index}`)));
      configureTlsClient(dealer, transport);
      return waitForConnectionReady(dealer, () => dealer.connect(endpoint));
    }));
    serverContext.recalculateAutoHwm();
    clientContext.recalculateAutoHwm();

    const payloads = dealers.map(() => Buffer.alloc(payloadSize));
    const inFlightByClient = dealers.map(() => 0);
    const maxInFlightByClient = dealers.map(() => 0);
    const senders = dealers.map(async (dealer, clientIndex) => {
      for (let sequence = 0; sequence < sendsPerClient; sequence += 1) {
        const payload = payloads[clientIndex];
        payload.fill(clientIndex + sequence);
        assert.equal(inFlightByClient[clientIndex], 0);
        inFlightByClient[clientIndex] += 1;
        maxInFlightByClient[clientIndex] = Math.max(
          maxInFlightByClient[clientIndex],
          inFlightByClient[clientIndex]
        );
        try {
          await dealer.send()
            .message(payload)
            .message(Buffer.alloc(0))
            .submit();
        } finally {
          inFlightByClient[clientIndex] -= 1;
        }
        // Each socket advances after its own admission. This progress point is
        // not a barrier with any other socket.
        await nextTurn();
      }
    });

    const serverPump = (async () => {
      let receivedCount = 0;
      while (receivedCount < expected) {
        while (router.recv(serverReceived, zlink.RecvFlags.DontWait)) {
          assert.ok(serverReceived.routingId);
          assert.equal(serverReceived.parts.length, 2);
          assert.equal(serverReceived.parts[0].data().length, payloadSize);
          assert.equal(serverReceived.parts[1].data().length, 0);
          const sourceParts = serverReceived.parts.slice();
          let replyOperation = serverReceived.send();
          for (const part of sourceParts) {
            replyOperation = replyOperation.message(part);
          }
          const reply = replyOperation.submit();
          // Admission consumes these Messages immediately. If admission is
          // backpressured, submit() first takes an immutable packet snapshot,
          // consumes the wrappers, and later retries without retaining them.
          assert.equal(sourceParts[0].size(), 0);
          assert.equal(sourceParts[1].size(), 0);
          pendingReplies.push(reply);
          receivedCount += 1;
          serverReceived = advanceReceived(serverReceived, receiveMode);
          await nextTurn();
        }
        await nextTurn();
      }
      await Promise.all(pendingReplies);
    })();

    const clientPump = (async () => {
      let replyCount = 0;
      while (replyCount < expected) {
        for (let index = 0; index < dealers.length; index += 1) {
          while (dealers[index].recv(clientReceived[index], zlink.RecvFlags.DontWait)) {
            const received = clientReceived[index];
            assert.equal(received.parts.length, 2);
            assert.equal(received.parts[0].data().length, payloadSize);
            assert.equal(received.parts[1].data().length, 0);
            replyCount += 1;
            clientReceived[index] = advanceReceived(received, receiveMode);
            await nextTurn();
          }
        }
        await nextTurn();
      }
    })();

    await within(Promise.all([...senders, serverPump, clientPump]), 30_000);
    assert.deepEqual(maxInFlightByClient, dealers.map(() => 1));
  } finally {
    serverReceived.close();
    clientReceived.forEach((received) => received.close());
    dealers.forEach((dealer) => dealer.close());
    router.close();
    clientContext.close();
    serverContext.close();
  }
}

test('routed scheduler advances available sockets while one admission remains pending', async () => {
  const sockets = [{ id: 0 }, { id: 1 }, { id: 2 }];
  const payloads = sockets.map(() => Buffer.alloc(64));
  const records = payloads.map((payload) => measurementParts(payload));
  const admissions = sockets.map(() => 0);
  const inFlight = sockets.map(() => 0);
  const maxInFlight = sockets.map(() => 0);
  let now = 0n;
  let turns = 0;
  let replyDrains = 0;
  let releaseSlow;
  const slowAdmission = new Promise<void>((resolve) => { releaseSlow = resolve; });
  let slowOwnedPayload = null;
  let beforeSlowRelease = null;

  const result = await runRoutedSendSendRounds({
    sockets,
    payloads,
    measurementRecords: records,
    routerClient: false,
    msgSize: 64,
    runId: 1,
    activeStopNs: 4n,
    sendDrainStopNs: 20n,
    nowNs: () => now,
    submit: (socket, _routerClient, record) => {
      const index = socket.id;
      assert.equal(inFlight[index], 0);
      assert.strictEqual(record, records[index]);
      admissions[index] += 1;
      inFlight[index] += 1;
      maxInFlight[index] = Math.max(maxInFlight[index], inFlight[index]);
      const admission = index === 0 && admissions[index] === 1
        ? slowAdmission
        : Promise.resolve();
      if (index === 0 && admissions[index] === 1) {
        slowOwnedPayload = Buffer.from(payloads[index]);
      }
      return admission.finally(() => { inFlight[index] -= 1; });
    },
    drainReplies: async () => { replyDrains += 1; },
    yieldTurn: async () => {
      turns += 1;
      if (turns === 3) {
        beforeSlowRelease = admissions.slice();
        assert.deepEqual(payloads[0], slowOwnedPayload);
        releaseSlow();
      }
      now += 1n;
      await nextTurn();
    }
  });

  assert.deepEqual(beforeSlowRelease, [1, 3, 3]);
  assert.deepEqual(admissions, [2, 4, 4]);
  assert.deepEqual(maxInFlight, [1, 1, 1]);
  assert.equal(result.sent, 10n);
  assert.ok(replyDrains >= 4);
});

test('routed scheduler stops new submits at deadline and drains owned admission', async () => {
  const sockets = [{ id: 0 }];
  const payloads = [Buffer.alloc(64)];
  const records = payloads.map((payload) => measurementParts(payload));
  const submitTimes = [];
  let now = 0n;
  let releasePending;
  const pending = new Promise<void>((resolve) => { releasePending = resolve; });

  const result = await runRoutedSendSendRounds({
    sockets,
    payloads,
    measurementRecords: records,
    routerClient: false,
    msgSize: 64,
    runId: 1,
    activeStopNs: 2n,
    sendDrainStopNs: 10n,
    nowNs: () => now,
    submit: () => {
      submitTimes.push(now);
      return pending;
    },
    yieldTurn: async () => {
      now += 1n;
      if (now === 4n) releasePending();
      await nextTurn();
    }
  });

  assert.deepEqual(submitTimes, [0n]);
  assert.ok(submitTimes.every((submittedAt) => submittedAt < 2n));
  assert.equal(result.sent, 1n);
  assert.ok(now >= 4n);
});

test('routed server reply tracking has no 4096-operation application cap', async () => {
  const pendingTasks = new Set<Promise<void>>();
  const tasks = [];
  const releases = [];
  const failures = [];

  for (let index = 0; index < 4097; index += 1) {
    let release;
    const task = new Promise<void>((resolve) => { release = resolve; });
    tasks.push(task);
    releases.push(release);
    trackPendingReplyTask(pendingTasks, task, (error) => failures.push(error));
  }

  assert.equal(pendingTasks.size, 4097);
  releases.forEach((release) => release());
  await Promise.all(tasks);
  await Promise.resolve();
  assert.equal(pendingTasks.size, 0);
  assert.deepEqual(failures, []);
});

test('routed SENDSEND reuses receive envelopes and echoes exactly two application parts', async () => {
  await runRoutedSendSendContract({
    transport: 'tcp',
    clientCount: 4,
    sendsPerClient: 32,
    payloadSize: 64
  });
});

test('routed SENDSEND preserves two-part records under concurrent TCP traffic', async () => {
  await runRoutedSendSendContract({
    transport: 'tcp',
    clientCount: 100,
    sendsPerClient: 512,
    payloadSize: 1024
  });
});

test('routed SENDSEND preserves two-part records under concurrent WSS traffic', async () => {
  await runRoutedSendSendContract({
    transport: 'wss',
    clientCount: 100,
    sendsPerClient: 512,
    payloadSize: 1024
  });
});

test('routed multipart replacement is identical for reuse, close-reuse, and close-fresh', async () => {
  for (const receiveMode of ['reuse', 'close-reuse', 'close-fresh'] as const) {
    await runRoutedSendSendContract({
      transport: 'tcp',
      clientCount: 32,
      sendsPerClient: 256,
      payloadSize: 1024,
      receiveMode
    });
  }
});

test('routed async multipart boundary preserves inline and overflow part storage', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const routed = new zlink.Received();
  const echoed = new zlink.Received();

  try {
    const endpoint = await benchmarkEndpoint(
      'tcp',
      `routed-sendsend-small-storage-${process.pid}`,
      { suite: 'multi' }
    );
    router.bind(endpoint);
    dealer.setRoutingId(zlink.RoutingId.from(Buffer.from('small-storage-client')));
    await waitForConnectionReady(dealer, () => dealer.connect(endpoint));

    let stableRoutingId: InstanceType<typeof zlink.RoutingId> | null = null;
    for (const partCount of [2, 8, 9]) {
      const expected = Array.from(
        { length: partCount },
        (_, index) => Buffer.from(`part-${partCount}-${index}`)
      );
      let send = dealer.send();
      for (const part of expected) send = send.message(part);
      await send.submit();

      const routedDeadline = Date.now() + 5_000;
      let routedReady = router.recv(routed, zlink.RecvFlags.DontWait);
      while (!routedReady && Date.now() < routedDeadline) {
        await nextTurn();
        routedReady = router.recv(routed, zlink.RecvFlags.DontWait);
      }
      assert.equal(routedReady, true);
      assert.equal(routed.parts.length, partCount);
      assert.ok(routed.routingId);
      if (stableRoutingId === null) {
        stableRoutingId = routed.routingId;
      } else {
        // Caller-owned Received storage keeps the validated route facade; the
        // native multipart materializer must not allocate then overwrite a
        // duplicate routing-id Buffer for the same peer.
        assert.strictEqual(routed.routingId, stableRoutingId);
      }
      for (let index = 0; index < partCount; index += 1) {
        assert.deepEqual(routed.parts[index].data(), expected[index]);
      }

      const sourceParts = routed.parts.slice();
      let reply = routed.send();
      for (const part of sourceParts) reply = reply.message(part);
      const admission = reply.submit();
      for (const part of sourceParts) assert.equal(part.size(), 0);
      await admission;

      const echoDeadline = Date.now() + 5_000;
      let echoReady = dealer.recv(echoed, zlink.RecvFlags.DontWait);
      while (!echoReady && Date.now() < echoDeadline) {
        await nextTurn();
        echoReady = dealer.recv(echoed, zlink.RecvFlags.DontWait);
      }
      assert.equal(echoReady, true);
      assert.equal(echoed.parts.length, partCount);
      for (let index = 0; index < partCount; index += 1) {
        assert.deepEqual(echoed.parts[index].data(), expected[index]);
      }
    }
  } finally {
    routed.close();
    echoed.close();
    dealer.close();
    router.close();
    context.close();
  }
});

test('received routed send preserves stale-route ownership across immediate or waiting failure', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const disconnected = router.monitorOpen([zlink.MonitorEventType.Disconnected]);
  const received = new zlink.Received();

  try {
    const endpoint = await benchmarkEndpoint(
      'tcp',
      `routed-sendsend-stale-${process.pid}`,
      { suite: 'multi' }
    );
    router.options.linger = 0;
    dealer.options.linger = 0;
    router.bind(endpoint);
    dealer.setRoutingId(zlink.RoutingId.from(Buffer.from('stale-client')));
    await waitForConnectionReady(dealer, () => dealer.connect(endpoint));
    await dealer.send()
      .message(Buffer.alloc(64, 0x61))
      .message(Buffer.alloc(0))
      .submit();

    const receiveDeadline = Date.now() + 5_000;
    while (!router.recv(received, zlink.RecvFlags.DontWait)
        && Date.now() < receiveDeadline) {
      await nextTurn();
    }
    assert.ok(received.routingId);
    assert.equal(received.parts.length, 2);

    dealer.close();
    await waitForMonitorEvent(disconnected, zlink.MonitorEventType.Disconnected);

    const sourceParts = received.parts.slice();
    const submission = received.send()
      .message(sourceParts[0])
      .message(sourceParts[1])
      .submit();
    if (sourceParts[0].size() === 0) {
      // A stale physical pipe may first report BACKPRESSURED. The binding has
      // already snapshotted the record, so closing its sender terminates the
      // WRITABLE wait without retaining the caller's Message wrappers.
      assert.equal(sourceParts[1].size(), 0);
      router.close();
      await assert.rejects(submission, (error: unknown) =>
        error instanceof zlink.SubmitError
        && (error as { result: number }).result === zlink.SubmitResult.Terminated);
    } else {
      // If Core has already retired the route, target selection fails before
      // snapshot/acceptance and Received keeps its parts for close or refill.
      await assert.rejects(submission, (error: unknown) =>
        error instanceof zlink.SubmitError
        && ((error as { result: number }).result === zlink.SubmitResult.NotConnected
          || (error as { result: number }).result === zlink.SubmitResult.NotFound));
      assert.equal(sourceParts[0].size(), 64);
      assert.equal(sourceParts[1].size(), 0);
    }
  } finally {
    received.close();
    disconnected.close();
    dealer.close();
    router.close();
    context.close();
  }
});
