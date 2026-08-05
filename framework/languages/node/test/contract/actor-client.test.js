const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const { Message, RequestResult } = require('@zlink-systems/zlink');

class ActorNotify { constructor(value) { this.value = value; } }
class ActorAsk { constructor(value) { this.value = value; } }

function actorRef(actorId = 'actor-1', generation = 1n) {
  return { nodeRid: 'node-a', actorId, objectGeneration: generation, meshName: 'play-mesh' };
}

function createReplyParts(value) {
  return [
    Message.from(Buffer.from(framework.encodeStreamHeader({
      kind: framework.ZLinkStreamMessageKind.Response,
      codec: framework.ZLinkStreamCodec.Json,
      flags: framework.ZLinkStreamHeaderFlags.None,
      name: 'ActorReply',
      metadata: new Map()
    }))),
    Message.from(Buffer.from(JSON.stringify(value)))
  ];
}

function createReplyFrame(value) {
  return [
    Message.from(Buffer.from(framework.encodeStreamFrame({
      kind: framework.ZLinkStreamMessageKind.Response,
      codec: framework.ZLinkStreamCodec.Json,
      flags: framework.ZLinkStreamHeaderFlags.None,
      name: 'ActorReply',
      metadata: new Map()
    }, Buffer.from(JSON.stringify(value)))))
  ];
}

function actorLocation(actorId = 'actor-1', generation = 1n, meshName = 'play-mesh') {
  const actor = actorRef(actorId, generation);
  return {
    meshName,
    actorId,
    actorType: 'Player',
    actorRef: actor,
    ownerNodeRid: actor.nodeRid,
    ownerNodeGeneration: 7n,
    spotKind: framework.ZLinkSpotKind.Entry,
    spotId: 'node-a-entry-test',
    spotGeneration: 1n,
    membershipEpoch: 1n,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 3n,
    authorityOwnerGeneration: 4n,
    authorityStoreVersion: 'store-version-1',
    updatedAt: new Date()
  };
}

function createResolver(resolve = ({ actorId }) => actorLocation(actorId)) {
  return {
    resolveDirectActorRoute: (actorId, signal) => resolve({ actorId }, signal),
    invalidateActorRoute() {}
  };
}

const operationId = Object.freeze({ high: 1n, low: 2n });

function completionTable(terminalResult, parts = []) {
  return {
    async wait(actualOperationId) {
      assert.deepEqual(actualOperationId, operationId);
      return {
        terminalResult,
        failureErrno: 0,
        operationKind: 0,
        kindData: null,
        parts
      };
    }
  };
}

test('actor client submit completes without exposing an admission result', async () => {
  const sends = [];
  const node = {
    sendToActor(actor, parts) {
      sends.push({ actor, parts });
      return 0;
    }
  };
  const resolver = createResolver();
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: () => node,
    locationResolver: () => resolver
  });

  const call = client.sendToActor(
    'actor-1',
    new ActorNotify('ping')
  );
  const submitted = await call.submit();

  assert.equal(submitted, undefined);
  assert.equal(sends.length, 1);
  assert.equal(sends[0].actor.actorId, 'actor-1');
  assert.equal(sends[0].parts.length, 2);
  await assert.rejects(() => call.submit(), (error) => {
    assert.equal(error instanceof framework.ZLinkFrameworkException, true);
    assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
    return true;
  });
  assert.equal(sends.length, 1);
});

test('actor client rejects an incomplete authority fence before transport submission', async () => {
  let sends = 0;
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: () => ({
      sendToActor() {
        sends += 1;
        return 0;
      }
    }),
    locationResolver: () => createResolver(({ actorId }) => ({
      ...actorLocation(actorId),
      ownerLeaseGeneration: 0n
    }))
  });

  await assert.rejects(
    () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  assert.equal(sends, 0);
});

test('actor client selects the MeshNode and completion table from the current Actor authority', async () => {
  const selected = [];
  const nodes = new Map([
    ['mesh-a', {
      requestToActor() {
        selected.push('node:mesh-a');
        return operationId;
      }
    }],
    ['mesh-b', {
      requestToActor() {
        selected.push('node:mesh-b');
        return operationId;
      }
    }]
  ]);
  const completions = new Map([
    ['mesh-a', {
      async wait() {
        selected.push('completion:mesh-a');
        return {
          terminalResult: RequestResult.Ok,
          failureErrno: 0,
          operationKind: 0,
          kindData: null,
          parts: createReplyParts({ mesh: 'mesh-a' })
        };
      }
    }],
    ['mesh-b', {
      async wait() {
        selected.push('completion:mesh-b');
        return {
          terminalResult: RequestResult.Ok,
          failureErrno: 0,
          operationKind: 0,
          kindData: null,
          parts: createReplyParts({ mesh: 'mesh-b' })
        };
      }
    }]
  ]);
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: (meshName) => nodes.get(meshName),
    completionTableProvider: (meshName) => completions.get(meshName),
    locationResolver: () => createResolver(({ actorId }) =>
      actorLocation(actorId, 1n, actorId === 'actor-a' ? 'mesh-a' : 'mesh-b'))
  });

  const first = await client
    .requestToActor('actor-a', new ActorAsk('ping'))
    .submit();
  const second = await client
    .requestToActor('actor-b', new ActorAsk('ping'))
    .submit();

  assert.deepEqual(first, { mesh: 'mesh-a' });
  assert.deepEqual(second, { mesh: 'mesh-b' });
  assert.deepEqual(selected, [
    'node:mesh-a',
    'completion:mesh-a',
    'node:mesh-b',
    'completion:mesh-b'
  ]);
  assert.throws(
    () => client.requestToActor('', new ActorAsk('ping')),
    /Actor ID must contain 1\.\.255 UTF-8 bytes/
  );
  assert.throws(
    () => client.requestToActor('가'.repeat(86), new ActorAsk('ping')),
    /Actor ID must contain 1\.\.255 UTF-8 bytes/
  );
});

test('actor client request decodes the handler reply and never auto-creates a missing actor', async () => {
  const node = {
    createActor() {
      throw new Error('actor client must not auto-create actors');
    },
    requestToActor(actor, parts, options) {
      assert.equal(actor.actorId, 'actor-1');
      assert.equal(parts.length, 2);
      assert.ok(options.timeoutMs > 0 && options.timeoutMs <= 100);
      return operationId;
    }
  };
  const completions = completionTable(
    RequestResult.Ok,
    createReplyParts({ value: 'pong' })
  );
  const resolver = createResolver();
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: () => node,
    completionTableProvider: () => completions,
    locationResolver: () => resolver
  });

  const reply = await client.requestToActor('actor-1', new ActorAsk('ping'))
    .timeout(100)
    .submit();

  assert.deepEqual(reply, { value: 'pong' });
});

test('actor client request decodes a single framed handler reply through stream protocol', async () => {
  const node = {
    requestToActor() {
      return operationId;
    }
  };
  const completions = completionTable(
    RequestResult.Ok,
    createReplyFrame({ value: 'framed-pong' })
  );
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: () => node,
    completionTableProvider: () => completions,
    locationResolver: () => createResolver()
  });

  const reply = await client.requestToActor('actor-1', new ActorAsk('ping'))
    .submit();

  assert.deepEqual(reply, { value: 'framed-pong' });
});

test('actor client invalidates a stale resolved route without retrying the operation', async () => {
  const first = actorRef('actor-1', 1n);
  const sends = [];
  const node = {
    sendToActor(actor) {
      sends.push(actor);
      throw framework.createInternalFrameworkException(
        framework.ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        'stale'
      );
    }
  };
  let invalidations = 0;
  const resolver = createResolver(() => actorLocation('actor-1', 1n));
  resolver.invalidateActorRoute = () => { invalidations += 1; };
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: () => node,
    locationResolver: () => resolver
  });

  await assert.rejects(
    () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );

  assert.deepEqual(sends.map((actor) => actor.generation), [1n]);
  assert.equal(invalidations, 1);
});

test('actor client submit maps native terminal outcomes to operation-specific errors', async () => {
  const results = [
    [2, framework.ZLinkFrameworkErrorKind.Unavailable],
    [3, framework.ZLinkFrameworkErrorKind.NotFound],
    [4, framework.ZLinkFrameworkErrorKind.ShuttingDown]
  ];
  const accepted = new framework.DefaultZLinkActorClient({
    nodeProvider: () => ({ sendToActor: () => 0 }),
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver(),
    meshSubmitters: new framework.ZLinkMeshSubmitterRegistry(5, 4096)
  });
  assert.equal(
    await accepted.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    undefined
  );

  for (const [nativeResult, expectedKind] of results) {
    const client = new framework.DefaultZLinkActorClient({
      nodeProvider: () => ({
        sendToActor() {
          return nativeResult;
        }
      }),
      completionTableProvider: () => undefined,
      locationResolver: () => createResolver(),
      meshSubmitters: new framework.ZLinkMeshSubmitterRegistry(5, 4096)
    });
    await assert.rejects(
      () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
      (error) => error.kind === expectedKind
    );
  }

  for (const nativeResult of [1, 13]) {
    const client = new framework.DefaultZLinkActorClient({
      nodeProvider: () => ({ sendToActor: () => nativeResult }),
      completionTableProvider: () => undefined,
      locationResolver: () => createResolver(),
      meshSubmitters: new framework.ZLinkMeshSubmitterRegistry(5, 4096)
    });
    await assert.rejects(
      () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
    );
  }

  const invalid = new framework.DefaultZLinkActorClient({
    nodeProvider: () => ({
      sendToActor() {
        return 6;
      }
    }),
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver(),
    meshSubmitters: new framework.ZLinkMeshSubmitterRegistry(5, 4096)
  });
  await assert.rejects(
    () => invalid.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    /submit result 6/
  );
});

test('Mesh submit cancellation removes pending admission and prevents late replay', async () => {
  let attempts = 0;
  const registry = new framework.ZLinkMeshSubmitterRegistry(1000, 4096);
  const controller = new AbortController();
  const pending = registry.submit('play-mesh', () => {
    attempts += 1;
    return { status: ZLinkSubmitStatus.Backpressured };
  }, controller.signal);
  controller.abort();
  await assert.rejects(pending, (error) => error?.name === 'AbortError');
  registry.notify('play-mesh');

  assert.equal(attempts, 1);
  registry.dispose();
});

test('Mesh submit shutdown rejects pending and future admission without late replay', async () => {
  let attempts = 0;
  const registry = new framework.ZLinkMeshSubmitterRegistry(1000, 4096);
  const pending = registry.submit('play-mesh', () => {
    attempts += 1;
    return { status: ZLinkSubmitStatus.Backpressured };
  });

  registry.dispose();
  assert.deepEqual(
    await pending,
    { status: ZLinkSubmitStatus.Shutdown }
  );
  registry.notify('play-mesh');
  assert.equal(attempts, 1);

  assert.deepEqual(
    await registry.submit('play-mesh', () => {
      attempts += 1;
      return { status: ZLinkSubmitStatus.Submitted };
    }),
    { status: ZLinkSubmitStatus.Shutdown }
  );
  assert.equal(attempts, 1);
});

test('Mesh submit uses the configured per-mesh timeout and releases capacity after timeout', async () => {
  let attempts = 0;
  let ready = false;
  const registry = new framework.ZLinkMeshSubmitterRegistry(
    (meshName) => meshName === 'play-mesh' ? 50 : 500,
    1
  );
  const started = Date.now();
  const first = registry.submit('play-mesh', () => {
    attempts += 1;
    return { status: ZLinkSubmitStatus.Backpressured };
  });
  await new Promise((resolve) => setTimeout(resolve, 20));
  const second = registry.submit('play-mesh', () => {
    attempts += 1;
    return {
      status: ready
        ? ZLinkSubmitStatus.Submitted
        : ZLinkSubmitStatus.Backpressured
    };
  });

  assert.deepEqual(await first, { status: ZLinkSubmitStatus.TimedOut });
  assert.equal(Date.now() - started < 500, true);
  ready = true;
  registry.notify('play-mesh');
  assert.deepEqual(await second, { status: ZLinkSubmitStatus.Submitted });
  assert.equal(attempts, 3);
  registry.dispose();
});

test('pre-aborted Actor call does not read authority or submit transport work', async () => {
  const controller = new AbortController();
  controller.abort();
  let attempts = 0;
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: () => ({
      sendToActor() {
        attempts += 1;
        return 0;
      }
    }),
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver(),
    staleActorRefPredicate: () => true
  });

  await assert.rejects(
    () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(controller.signal),
    (error) => error?.name === 'AbortError'
  );
  assert.equal(attempts, 0);
});

test('actor client maps stale and disconnected route failures', async () => {
  const noNode = new framework.DefaultZLinkActorClient({
    nodeProvider: () => undefined,
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver()
  });
  await assert.rejects(
    () => noNode.requestToActor('missing', new ActorAsk('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );

  const staleNode = {
    requestToActor() {
      return operationId;
    }
  };
  const stale = new framework.DefaultZLinkActorClient({
    nodeProvider: () => staleNode,
    completionTableProvider: () => completionTable(RequestResult.Conflict),
    locationResolver: () => createResolver(({ actorId }) => actorLocation(actorId, 1n))
  });
  await assert.rejects(
    () => stale.requestToActor('actor-1', new ActorAsk('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );

  const disconnectedNode = {
    requestToActor() {
      return operationId;
    }
  };
  const disconnected = new framework.DefaultZLinkActorClient({
    nodeProvider: () => disconnectedNode,
    completionTableProvider: () => completionTable(RequestResult.NotConnected),
    locationResolver: () => createResolver()
  });
  await assert.rejects(
    () => disconnected.requestToActor('actor-1', new ActorAsk('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      && !('isRetriable' in error)
  );
});

test('actor client preserves ActorRouteNotFound for a missing actor route', async () => {
  const missingNode = {
    requestToActor() {
      return operationId;
    }
  };
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: () => missingNode,
    completionTableProvider: () => completionTable(RequestResult.NotFound),
    locationResolver: () => createResolver(({ actorId }) => actorLocation(actorId))
  });

  await assert.rejects(
    () => client.requestToActor('missing-actor', new ActorAsk('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
});
