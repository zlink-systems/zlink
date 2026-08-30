// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const { currentEpochNs } = require('../perf/common/perf_metrics');
const { benchmarkEndpoint } = require('../perf/common/perf_endpoint');
const {
  configureTlsClient,
  configureTlsServer
} = require('../perf/common/perf_tls');
const {
  measurementParts,
  waitForConnectionReady
} = require('../perf/multi/perf_multi_runtime');
const {
  runDealerDealerSendRounds
} = require('../perf/multi/perf_multi_dealer_dealer_client');
const { STOP_TOKEN_BYTES } = require('../perf/perf_stop_token');

function nextTurn(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function within<T>(promise: Promise<T>, timeoutMs = 15_000): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`dealer/dealer fair-round contract timed out after ${timeoutMs}ms`)),
      timeoutMs
    );
    promise.then(
      (value) => { clearTimeout(timer); resolve(value); },
      (error) => { clearTimeout(timer); reject(error); }
    );
  });
}

async function runDealerDealerFairRoundContract(transport: 'tcp' | 'wss'): Promise<void> {
  const clientCount = 16;
  const rounds = 64;
  const payloadSize = 1024;
  const serverContext = zlink.createContext();
  const clientContext = zlink.createContext();
  serverContext.options.ioThreads = 4;
  clientContext.options.ioThreads = 4;
  serverContext.options.autoHwmEnabled = true;
  clientContext.options.autoHwmEnabled = true;
  const server = zlink.createDealerSocket(serverContext);
  const dealers = Array.from(
    { length: clientCount },
    () => zlink.createDealerSocket(clientContext)
  );
  const received = new zlink.Received();

  try {
    const endpoint = await benchmarkEndpoint(
      transport,
      `dealer-dealer-fair-round-contract-${process.pid}`,
      { suite: 'multi' }
    );
    configureTlsServer(server, transport);
    server.bind(endpoint);
    await Promise.all(dealers.map((dealer) => {
      configureTlsClient(dealer, transport);
      return waitForConnectionReady(dealer, () => dealer.connect(endpoint));
    }));
    serverContext.recalculateAutoHwm();
    clientContext.recalculateAutoHwm();

    const payloads = dealers.map(() => Buffer.alloc(payloadSize));
    const admissions = dealers.map(() => 0);
    const inFlight = dealers.map(() => 0);
    const maxInFlight = dealers.map(() => 0);
    const stableRecords = dealers.map(() => null);
    let sharedEmptyTail = null;
    let yields = 0;
    const dealerIndex = new Map(dealers.map((dealer, index) => [dealer, index]));
    const submit = (dealer, record) => {
      const index = dealerIndex.get(dealer);
      assert.notEqual(index, undefined);
      assert.equal(inFlight[index], 0);
      assert.equal(record.length, 2);
      assert.strictEqual(record[0], payloads[index]);
      if (stableRecords[index] === null) {
        stableRecords[index] = record;
      } else {
        assert.strictEqual(record, stableRecords[index]);
      }
      if (sharedEmptyTail === null) {
        sharedEmptyTail = record[1];
      } else {
        assert.strictEqual(record[1], sharedEmptyTail);
      }
      admissions[index] += 1;
      inFlight[index] += 1;
      maxInFlight[index] = Math.max(maxInFlight[index], inFlight[index]);
      let operation = dealer.send();
      for (const part of record) operation = operation.message(part);
      return operation.submit()
        .finally(() => { inFlight[index] -= 1; });
    };

    let senderResult = null;
    let senderDone = false;
    const sender = runDealerDealerSendRounds({
      dealers,
      payloads,
      msgSize: payloadSize,
      activeStopNs: currentEpochNs() + 10_000_000_000n,
      maxTurns: rounds,
      submit,
      yieldTurn: async () => {
        yields += 1;
        await nextTurn();
      }
    }).then((result) => {
      senderResult = result;
      senderDone = true;
      return result;
    });
    const receiver = (async () => {
      let count = 0;
      while (!senderDone || BigInt(count) < senderResult.sent) {
        while (server.recv(received, zlink.RecvFlags.DontWait)) {
          assert.equal(received.parts.length, 2);
          assert.equal(received.parts[0].data().length, payloadSize);
          assert.equal(received.parts[1].data().length, 0);
          count += 1;
        }
        await nextTurn();
      }
    })();

    const result = await within(Promise.all([sender, receiver]));
    assert.equal(result[0].turns, rounds);
    assert.ok(result[0].sent > 0n);
    assert.ok(admissions.every((count) => count > 0 && count <= rounds));
    assert.deepEqual(maxInFlight, dealers.map(() => 1));
    assert.ok(yields >= rounds);
  } finally {
    received.close();
    dealers.forEach((dealer) => dealer.close());
    server.close();
    clientContext.close();
    serverContext.close();
  }
}

test('dealer/dealer fair rounds preserve two-part TCP admission', async () => {
  await runDealerDealerFairRoundContract('tcp');
});

test('dealer/dealer fair rounds preserve two-part WSS admission', async () => {
  await runDealerDealerFairRoundContract('wss');
});

test('dealer/dealer fair scheduler advances available sockets past a pending admission', async () => {
  const dealers = [{ id: 0 }, { id: 1 }, { id: 2 }];
  const payloads = dealers.map(() => Buffer.alloc(64));
  const admissions = dealers.map(() => 0);
  let releaseSlow;
  const slowAdmission = new Promise<void>((resolve) => { releaseSlow = resolve; });
  let beforeSlowRelease = null;
  let yieldedTurns = 0;

  const result = await runDealerDealerSendRounds({
    dealers,
    payloads,
    msgSize: 64,
    activeStopNs: currentEpochNs() + 10_000_000_000n,
    maxTurns: 4,
    submit: (dealer) => {
      admissions[dealer.id] += 1;
      if (dealer.id === 0 && admissions[dealer.id] === 1) {
        return slowAdmission;
      }
      return Promise.resolve();
    },
    yieldTurn: async () => {
      yieldedTurns += 1;
      if (yieldedTurns === 3) {
        beforeSlowRelease = admissions.slice();
        releaseSlow();
      }
      await nextTurn();
    }
  });

  assert.deepEqual(beforeSlowRelease, [1, 3, 3]);
  assert.deepEqual(admissions, [2, 4, 4]);
  assert.equal(result.turns, 4);
  assert.equal(result.sent, 10n);
});

test('multi measurement records share only the empty tail and stop stays one-part', async () => {
  const firstPayload = Buffer.alloc(64);
  const secondPayload = Buffer.alloc(64);
  const firstRecord = measurementParts(firstPayload);
  const secondRecord = measurementParts(secondPayload);
  assert.equal(firstRecord.length, 2);
  assert.equal(secondRecord.length, 2);
  assert.strictEqual(firstRecord[0], firstPayload);
  assert.strictEqual(secondRecord[0], secondPayload);
  assert.strictEqual(firstRecord[1], secondRecord[1]);
  assert.equal(firstRecord[1].length, 0);

  const submitted = [];
  const socket = {
    send() {
      const parts = [];
      return {
        message(part) {
          parts.push(part);
          return this;
        },
        submit() {
          submitted.push(parts);
          return Promise.resolve();
        }
      };
    }
  };
  const { sendRouted } = require('../perf/multi/perf_multi_runtime');
  await sendRouted(socket, firstRecord);
  await sendRouted(socket, firstRecord);
  // The compatibility scalar path uses the same shared tail without first
  // allocating a transient measurement array.
  await sendRouted(socket, secondPayload);
  await sendRouted(socket, secondPayload);
  await sendRouted(socket, [STOP_TOKEN_BYTES]);

  assert.deepEqual(submitted.map((parts) => parts.length), [2, 2, 2, 2, 1]);
  assert.strictEqual(submitted[0][0], firstPayload);
  assert.strictEqual(submitted[0][1], firstRecord[1]);
  assert.strictEqual(submitted[1][0], firstPayload);
  assert.strictEqual(submitted[1][1], firstRecord[1]);
  assert.strictEqual(submitted[2][0], secondPayload);
  assert.strictEqual(submitted[2][1], firstRecord[1]);
  assert.strictEqual(submitted[3][0], secondPayload);
  assert.strictEqual(submitted[3][1], firstRecord[1]);
  assert.strictEqual(submitted[4][0], STOP_TOKEN_BYTES);
});
