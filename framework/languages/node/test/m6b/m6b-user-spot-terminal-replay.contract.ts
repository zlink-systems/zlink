import assert from 'node:assert/strict';
import { test } from 'node:test';

import { RequestError, RequestResult } from '@zlink-systems/zlink';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts';
import type {
  RawServiceMeshRuntime
} from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import {
  ServiceStatefulRuntime
} from '../../packages/framework/src/runtime/foundation/service-stateful-runtime';
import {
  decodeStatefulHeader,
  encodeStatefulReply
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';

test('command 48 replays a lost reply with the same operation ID inside the original deadline', async () => {
  const requests: Buffer[] = [];
  const attemptTimeouts: number[] = [];
  const deadlineMs = Date.now() + 500;
  const raw = {
    setServiceIngress() {},
    async requestService(
      _targetNodeRid: string,
      parts: readonly Uint8Array[],
      timeoutMs: number
    ): Promise<readonly Buffer[]> {
      const head = Buffer.from(parts[0]!);
      requests.push(head);
      attemptTimeouts.push(timeoutMs);
      if (requests.length === 1) {
        await new Promise(resolve => setTimeout(resolve, 40));
        throw new Error('simulated reply-route disconnect');
      }
      const decoded = decodeStatefulHeader(head);
      assert.equal(decoded.kind, 'userSpotClose');
      return [
        encodeStatefulReply(
          decoded.correlation,
          RequestResult.Ok,
          0,
          { kind: 'userSpotClose', closed: true }
        )
      ];
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);

  const result = await runtime.requestUserSpotClose(
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
      deadlineUnixMs: BigInt(deadlineMs)
    },
    500
  );

  assert.equal(result.terminalResult, RequestResult.Ok);
  assert.deepEqual(result.tail, { kind: 'userSpotClose', closed: true });
  assert.equal(requests.length, 2);
  assert.deepEqual(requests[1], requests[0]);
  const first = decodeStatefulHeader(requests[0]!);
  const second = decodeStatefulHeader(requests[1]!);
  assert.equal(first.kind, 'userSpotClose');
  assert.equal(second.kind, 'userSpotClose');
  assert.deepEqual(second.operation, first.operation);
  assert(attemptTimeouts[0]! > 400);
  assert(attemptTimeouts[1]! > 0 && attemptTimeouts[1]! < attemptTimeouts[0]!);
  assert(Date.now() < deadlineMs);
  runtime.close();
});

test('command 48 gives a healthy first attempt the whole remaining deadline', async () => {
  const requests: Buffer[] = [];
  const attemptTimeouts: number[] = [];
  const healthyDurationMs = 180;
  const operationTimeoutMs = 300;
  const raw = {
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
        await new Promise(resolve => setTimeout(resolve, timeoutMs));
        throw new Error('healthy operation was cut short');
      }
      await new Promise(resolve => setTimeout(resolve, healthyDurationMs));
      const decoded = decodeStatefulHeader(head);
      assert.equal(decoded.kind, 'userSpotClose');
      return [
        encodeStatefulReply(
          decoded.correlation,
          RequestResult.Ok,
          0,
          { kind: 'userSpotClose', closed: true }
        )
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

test('command 48 exhaustion is Unavailable when no attempt was admitted', async () => {
  let attempts = 0;
  const raw = {
    setServiceIngress() {},
    async requestService(): Promise<readonly Buffer[]> {
      attempts += 1;
      throw new RequestError(RequestResult.NotConnected);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);

  try {
    await assert.rejects(
      () => runtime.requestUserSpotClose(
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
      (error: unknown) => error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.Unavailable
    );
    assert(attempts > 0);
  } finally {
    runtime.close();
  }
});

test('command 48 exhaustion is DeadlineExceeded after an admitted reply is withheld', async () => {
  let attempts = 0;
  const raw = {
    setServiceIngress() {},
    async requestService(
      _targetNodeRid: string,
      _parts: readonly Uint8Array[],
      timeoutMs: number
    ): Promise<readonly Buffer[]> {
      attempts += 1;
      await new Promise(resolve => setTimeout(resolve, timeoutMs));
      throw new RequestError(RequestResult.TimedOut);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);

  try {
    await assert.rejects(
      () => runtime.requestUserSpotClose(
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
      (error: unknown) => error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
    );
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
    const raw = {
      setServiceIngress() {},
      async requestService(_target: string, parts: readonly Uint8Array[], timeoutMs: number) {
        requests.push(Buffer.from(parts[0]!));
        budgets.push(timeoutMs);
        if (requests.length === 1) {
          elapsedMs += 40;
          t.mock.method(Date, 'now', () => wallNow + wallJumpMs);
          throw new Error('lost reply');
        }
        const record = decodeStatefulHeader(requests[1]!);
        assert.equal(record.kind, 'userSpotClose');
        return [encodeStatefulReply(record.correlation, RequestResult.Ok, 0,
          { kind: 'userSpotClose', closed: true })];
      }
    } as unknown as RawServiceMeshRuntime;
    const runtime = new ServiceStatefulRuntime(raw, 'source-node', 17n);
    t.after(() => runtime.close());
    const result = await runtime.requestUserSpotClose('target-node', {
      sourceNodeRid: 'source-node',
      sourceNodeGeneration: 17n,
      target: {
        spotId: 'clock-jump-spot', objectGeneration: 19n,
        targetNodeRid: 'target-node', targetNodeGeneration: 23n,
        authorityOwnerGeneration: 29n, expectedStoreVersion: 'version-31'
      },
      deadlineUnixMs
    }, 500);
    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.deepEqual(budgets, [500, 460]);
    assert.deepEqual(requests[1], requests[0]);
    const record = decodeStatefulHeader(requests[0]!);
    assert.equal(record.kind, 'userSpotClose');
    if (record.kind === 'userSpotClose') assert.equal(record.deadlineUnixMs, deadlineUnixMs);
  });
}
