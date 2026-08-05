import assert from 'node:assert/strict';
import { test } from 'node:test';

import { RequestResult } from '@zlink-systems/zlink';
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

test('command 48 transport retry preserves its operation ID and close fence', async () => {
  const requests: Buffer[] = [];
  const raw = {
    setServiceIngress() {},
    async requestService(
      _targetNodeRid: string,
      parts: readonly Uint8Array[]
    ): Promise<readonly Buffer[]> {
      const head = Buffer.from(parts[0]!);
      requests.push(head);
      if (requests.length === 1) {
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
      deadlineUnixMs: BigInt(Date.now() + 5_000)
    },
    1_000
  );

  assert.equal(result.terminalResult, RequestResult.Ok);
  assert.deepEqual(result.tail, { kind: 'userSpotClose', closed: true });
  assert.equal(requests.length, 2);
  assert.deepEqual(requests[1], requests[0]);
  runtime.close();
});
