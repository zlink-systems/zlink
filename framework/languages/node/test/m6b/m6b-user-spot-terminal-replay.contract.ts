import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  RequestResult,
  SubmitResult,
  ZLinkBackendResultError
} from '../../packages/framework/src/runtime/backend/runtime-values';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts';
import type { RawServiceMeshRuntime } from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import { ServiceStatefulRuntime } from '../../packages/framework/src/runtime/foundation/service-stateful-runtime';
import {
  decodeStatefulHeader,
  encodeStatefulReply
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';

test('command 48 returns the first binding failure without resubmitting', async () => {
  const requests: Buffer[] = [];
  const attemptTimeouts: number[] = [];
  const deadlineUnixMs = BigInt(Date.now() + 500);
  const failure = new ZLinkBackendResultError('request', RequestResult.NotConnected);
  const raw = {
    observePeerRoute: () => ({ generation: 1n, ready: true }),
    observePeerRouteChanged: () => () => {},
    observePeerConnectionIntentRemoved() {
      return () => {};
    },
    setServiceIngress() {},
    async requestService(
      _targetNodeRid: string,
      parts: readonly Uint8Array[],
      timeoutMs: number
    ): Promise<readonly Buffer[]> {
      const head = Buffer.from(parts[0]!);
      requests.push(head);
      attemptTimeouts.push(timeoutMs);
      throw failure;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);

  const result = runtime.requestUserSpotClose(
    'target-node',
    {
      sourceNodeRid: 'source-node',
      sourceNodeGeneration: 17n,
      target: {
        spotId: 'terminal-replay-spot',
        objectGeneration: 19n,
        targetNodeRid: 'target-node',
        targetNodeGeneration: 23n,
        authorityOwnerGeneration: 29n,
        expectedStoreVersion: 'version-31'
      },
      deadlineUnixMs
    },
    500
  );

  await assert.rejects(result, (error) => error === failure);
  assert.equal(requests.length, 1);
  const first = decodeStatefulHeader(requests[0]!);
  assert.equal(first.kind, 'userSpotClose');
  if (first.kind === 'userSpotClose') assert.equal(first.deadlineUnixMs, deadlineUnixMs);
  assert(attemptTimeouts[0]! > 0);
  runtime.close();
});

test('command 48 gives a healthy first attempt the whole remaining deadline', async () => {
  const requests: Buffer[] = [];
  const attemptTimeouts: number[] = [];
  const healthyDurationMs = 180;
  const operationTimeoutMs = 300;
  const raw = {
    observePeerRoute: () => ({ generation: 1n, ready: true }),
    observePeerRouteChanged: () => () => {},
    observePeerConnectionIntentRemoved() {
      return () => {};
    },
    setServiceIngress() {},
    async requestService(
      _targetNodeRid: string,
      parts: readonly Uint8Array[],
      timeoutMs: number
    ): Promise<readonly Buffer[]> {
      const head = Buffer.from(parts[0]!);
      requests.push(head);
      attemptTimeouts.push(timeoutMs);
      if (timeoutMs <= healthyDurationMs) {
        await new Promise((resolve) => setTimeout(resolve, timeoutMs));
        throw new Error('healthy operation was cut short');
      }
      await new Promise((resolve) => setTimeout(resolve, healthyDurationMs));
      const decoded = decodeStatefulHeader(head);
      assert.equal(decoded.kind, 'userSpotClose');
      return [
        encodeStatefulReply(decoded.correlation, RequestResult.Ok, 0, {
          kind: 'userSpotClose',
          closed: true
        })
      ];
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);

  try {
    const result = await runtime.requestUserSpotClose(
      'target-node',
      {
        sourceNodeRid: 'source-node',
        sourceNodeGeneration: 17n,
        target: {
          spotId: 'timeout-replay-spot',
          objectGeneration: 19n,
          targetNodeRid: 'target-node',
          targetNodeGeneration: 23n,
          authorityOwnerGeneration: 29n,
          expectedStoreVersion: 'version-31'
        },
        deadlineUnixMs: BigInt(Date.now() + operationTimeoutMs)
      },
      operationTimeoutMs
    );

    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.deepEqual(result.tail, { kind: 'userSpotClose', closed: true });
    assert.equal(requests.length, 1);
    assert(attemptTimeouts[0]! > healthyDurationMs);
    assert(attemptTimeouts[0]! > operationTimeoutMs / 2);
  } finally {
    runtime.close();
  }
});

test('command 48 first submit failure is returned once', async () => {
  let attempts = 0;
  const failure = new ZLinkBackendResultError('submit', SubmitResult.NotConnected);
  const raw = {
    observePeerRoute: () => ({ generation: 1n, ready: true }),
    observePeerRouteChanged: () => () => {},
    observePeerConnectionIntentRemoved() {
      return () => {};
    },
    setServiceIngress() {},
    async requestService(): Promise<readonly Buffer[]> {
      attempts += 1;
      throw failure;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);

  try {
    await assert.rejects(
      () =>
        runtime.requestUserSpotClose(
          'absent-node',
          {
            sourceNodeRid: 'source-node',
            sourceNodeGeneration: 17n,
            target: {
              spotId: 'absent-route-spot',
              objectGeneration: 19n,
              targetNodeRid: 'absent-node',
              targetNodeGeneration: 23n,
              authorityOwnerGeneration: 29n,
              expectedStoreVersion: 'version-31'
            },
            deadlineUnixMs: BigInt(Date.now() + 80)
          },
          80
        ),
      (error: unknown) => error === failure
    );
    assert.equal(attempts, 1);
  } finally {
    runtime.close();
  }
});

test('command 48 exhaustion is DeadlineExceeded after an admitted reply is withheld', async (t) => {
  let now = 1000;
  t.mock.method(performance, 'now', () => now);
  let attempts = 0;
  let finish!: () => void;
  const raw = {
    observePeerRoute: () => ({ generation: 1n, ready: true }),
    observePeerRouteChanged: () => () => {},
    observePeerConnectionIntentRemoved() {
      return () => {};
    },
    setServiceIngress() {},
    async requestService(
      _targetNodeRid: string,
      _parts: readonly Uint8Array[],
      _timeoutMs: number,
      _onAdmitted?: () => void
    ): Promise<readonly Buffer[]> {
      attempts += 1;
      await new Promise<void>((resolve) => {
        finish = resolve;
      });
      throw new ZLinkBackendResultError('request', RequestResult.TimedOut);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);

  try {
    const result = assert.rejects(
      () =>
        runtime.requestUserSpotClose(
          'target-node',
          {
            sourceNodeRid: 'source-node',
            sourceNodeGeneration: 17n,
            target: {
              spotId: 'withheld-reply-spot',
              objectGeneration: 19n,
              targetNodeRid: 'target-node',
              targetNodeGeneration: 23n,
              authorityOwnerGeneration: 29n,
              expectedStoreVersion: 'version-31'
            },
            deadlineUnixMs: BigInt(Date.now() + 80)
          },
          80
        ),
      (error: unknown) =>
        error instanceof ZLinkFrameworkException &&
        error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
    );
    await new Promise((resolve) => setImmediate(resolve));
    now += 80;
    assert.equal(runtime.expireOperations(now, Number.POSITIVE_INFINITY), 1);
    await result;
    finish();
    assert.equal(attempts, 1);
  } finally {
    runtime.close();
  }
});

for (const wallJumpMs of [60_000, -60_000]) {
  test(`durable operation preserves its budget and wire deadline across a ${wallJumpMs} ms clock jump`, async (t) => {
    const wallNow = Date.now();
    const deadlineUnixMs = BigInt(wallNow + 500);
    let elapsedMs = 0;
    t.mock.method(performance, 'now', () => elapsedMs);
    const requests: Buffer[] = [];
    const budgets: number[] = [];
    let ready = false;
    let changed!: (rid: string) => void;
    const raw = {
      observePeerRoute: () => ({ generation: 1n, ready }),
      observePeerRouteChanged: (listener: (rid: string) => void) => {
        changed = listener;
        return () => {};
      },
      observePeerConnectionIntentRemoved() {
        return () => {};
      },
      setServiceIngress() {},
      async requestService(_target: string, parts: readonly Uint8Array[], timeoutMs: number) {
        requests.push(Buffer.from(parts[0]!));
        budgets.push(timeoutMs);
        const record = decodeStatefulHeader(requests[0]!);
        assert.equal(record.kind, 'userSpotClose');
        return [
          encodeStatefulReply(record.correlation, RequestResult.Ok, 0, {
            kind: 'userSpotClose',
            closed: true
          })
        ];
      }
    } as unknown as RawServiceMeshRuntime;
    const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);
    t.after(() => runtime.close());
    const operation = runtime.requestUserSpotClose(
      'target-node',
      {
        sourceNodeRid: 'source-node',
        sourceNodeGeneration: 17n,
        target: {
          spotId: 'clock-jump-spot',
          objectGeneration: 19n,
          targetNodeRid: 'target-node',
          targetNodeGeneration: 23n,
          authorityOwnerGeneration: 29n,
          expectedStoreVersion: 'version-31'
        },
        deadlineUnixMs
      },
      500
    );
    assert.equal(requests.length, 0);
    elapsedMs += 40;
    t.mock.method(Date, 'now', () => wallNow + wallJumpMs);
    ready = true;
    changed('target-node');
    const result = await operation;
    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.deepEqual(budgets, [460]);
    assert.equal(requests.length, 1);
    const record = decodeStatefulHeader(requests[0]!);
    assert.equal(record.kind, 'userSpotClose');
    if (record.kind === 'userSpotClose') assert.equal(record.deadlineUnixMs, deadlineUnixMs);
  });
}
