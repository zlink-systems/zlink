const assert = require('node:assert/strict');
const test = require('node:test');

const { Message } = require('@zlink-systems/zlink');
const {
  DefaultZLinkActorContext
} = require('../../packages/framework/dist/runtime/actors/actor-context');
const {
  deferActorJoin,
  runActorHandlerWithDeferredJoins
} = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
const {
  ZLinkSpotActorPacketDispatch
} = require('../../packages/framework/dist/runtime/spots/spot-actor-packet-dispatch');
const framework = require('../../packages/framework/dist/internal');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  ZLinkActorRuntimeState
} = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const {
  ZLinkActorDispatchMailbox
} = require('../../packages/framework/dist/runtime/actors/actor-mailbox');
const {
  ZLinkSpotSerialExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-executor');

function actorHarness(events, completionResult = { accepted: true }) {
  const state = new ZLinkActorRuntimeState('alice');
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'alice',
    generation: 7n
  };
  const coordinator = {
    async joinSpot(actor, runtimeState, spotId, request, timeoutMs) {
      events.push(`join:${actor.actorId}:${runtimeState.actorId}:${spotId}:${request.data()}:${timeoutMs}`);
      return {
        ...completionResult,
        actor: actorRef,
        reply: Message.from('joined')
      };
    },
    async joinEntrySpot() {
      throw new Error('unexpected Entry Spot join');
    }
  };
  const context = new DefaultZLinkActorContext(
    state,
    coordinator,
    undefined,
    undefined,
    () => 'game',
    undefined
  );
  const actor = {
    actorId: 'alice',
    context,
    async onJoinCompleted(completion) {
      events.push(`completion:${completion.status}:${completion.actor?.generation ?? '-'}`);
    }
  };
  state.bindActor(actor, context);
  return { actor, context, state };
}

async function waitForEvents(events, count) {
  for (let attempt = 0; attempt < 20 && events.length < count; attempt += 1) {
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.ok(events.length >= count, `expected at least ${count} events, received ${events.length}`);
}

test('deferred Actor Join starts after the handler continuation and preserves its result', async () => {
  const events = [];
  const { context } = actorHarness(events);

  const result = await runActorHandlerWithDeferredJoins(async () => {
    events.push('handler:start');
    context.joinSpot('room-a', 'hello').timeout(25).defer();
    events.push('handler:end');
    return 'handled';
  });
  assert.equal(result, 'handled');
  assert.equal(events.length, 4);
  const timeoutMs = Number(events[2].split(':').at(-1));
  assert.match(events[2], /^join:alice:alice:room-a:"hello":\d+$/);
  assert.ok(timeoutMs > 0 && timeoutMs <= 25);
  assert.equal(events[3], 'completion:accepted:7');
  assert.equal(context.objectGeneration, 1n);
});

test('deferred Actor Join starts admission before the original reply but completes after it', async () => {
  const events = [];
  const state = new ZLinkActorRuntimeState('alice');
  const actorRef = { nodeRid: 'node-a', actorId: 'alice', generation: 7n };
  const coordinator = {
    beginDeferredJoin() {
      events.push('join:barrier');
    },
    async joinSpot() {
      events.push('join:start');
      return { accepted: true, actor: actorRef, reply: Message.from('joined') };
    },
    async joinEntrySpot() {
      throw new Error('unexpected Entry Spot join');
    },
    async abortDeferredJoin() {
      events.push('join:abort');
    }
  };
  const context = new DefaultZLinkActorContext(
    state,
    coordinator,
    undefined,
    undefined,
    () => 'game',
    undefined
  );
  const actor = {
    actorId: 'alice',
    context,
    async onJoinCompleted(completion) {
      events.push(`completion:${completion.status}`);
    }
  };
  state.bindActor(actor, context);

  const result = await runActorHandlerWithDeferredJoins(() => {
    events.push('handler:start');
    context.joinSpot('room-a').defer();
    events.push('handler:end');
    return 'handled';
  }, (reply) => {
    events.push(`reply:${reply}`);
    return reply;
  });

  assert.equal(result, 'handled');
  assert.deepEqual(events, [
    'handler:start',
    'handler:end',
    'join:barrier',
    'join:start',
    'reply:handled',
    'completion:accepted'
  ]);
});

test('deferred Actor Join barrier keeps the next Actor mailbox turn behind completion', async () => {
  const events = [];
  const mailbox = new ZLinkActorDispatchMailbox();
  let releaseJoin;
  const joinGate = new Promise(resolve => {
    releaseJoin = resolve;
  });

  const first = mailbox.submit(() => runActorHandlerWithDeferredJoins(() => {
    events.push('handler:first');
    deferActorJoin({
      requestBytes: 0,
      discard() {
        events.push('join:discard');
      },
      async execute() {
        events.push('join:start');
        await joinGate;
        events.push('join:completed');
      }
    });
  }));
  const second = mailbox.submit(async () => {
    events.push('handler:second');
  });

  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(events, ['handler:first', 'join:start']);
  releaseJoin();
  await Promise.all([first, second]);
  assert.deepEqual(events, [
    'handler:first',
    'join:start',
    'join:completed',
    'handler:second'
  ]);
});

test('SpotWide deferred Actor Join yields the shared Spot gate while waiting for the target', async () => {
  const events = [];
  let signalJoinStarted;
  const joinStarted = new Promise((resolve) => { signalJoinStarted = resolve; });
  const releaseJoin = {};
  const joinGate = new Promise((resolve) => { releaseJoin.resolve = resolve; });
  const state = new ZLinkActorRuntimeState('alice');
  const actorRef = { nodeRid: 'node-a', actorId: 'alice', generation: 7n };
  const coordinator = {
    async joinSpot(actor, runtimeState, spotId, request) {
      events.push(`join:start:${actor.actorId}:${runtimeState.actorId}:${spotId}:${request.data()}`);
      signalJoinStarted();
      await joinGate;
      events.push('join:target-complete');
      return { accepted: true, actor: actorRef, reply: Message.from('joined') };
    },
    async joinEntrySpot() {
      throw new Error('unexpected Entry Spot join');
    }
  };
  const context = new DefaultZLinkActorContext(
    state,
    coordinator,
    undefined,
    undefined,
    () => 'game',
    undefined
  );
  const actor = {
    actorId: 'alice',
    context,
    async onJoinCompleted() {
      events.push('join:completed');
    }
  };
  state.bindActor(actor, context);
  const serial = new ZLinkSpotSerialExecutor(true);

  const first = serial.execute(() => runActorHandlerWithDeferredJoins(() => {
    events.push('handler:first');
    context.joinSpot('room-a', 'hello').timeout(100).defer();
    events.push('handler:first-end');
  }));
  const second = serial.execute(() => {
    events.push('handler:second');
  });

  await joinStarted;
  await second;
  assert.deepEqual(events, [
    'handler:first',
    'handler:first-end',
    'join:start:alice:alice:room-a:"hello"',
    'handler:second'
  ]);

  releaseJoin.resolve();
  await first;
  assert.deepEqual(events, [
    'handler:first',
    'handler:first-end',
    'join:start:alice:alice:room-a:"hello"',
    'handler:second',
    'join:target-complete',
    'join:completed'
  ]);
});

test('Core-routed Actor request submits its reply before deferred Join completion', async () => {
  const events = [];
  class PlayerActor {
    constructor() {
      this.actorId = 'alice';
    }
  }
  class JoinHandler {
    async handle() {
      events.push('handler');
      deferActorJoin({
        requestBytes: 0,
        discard() {},
        async execute() {
          events.push('completion:0123456789abcdef:fedcba9876543210');
        }
      });
      return { accepted: true };
    }
  }
  const actor = new PlayerActor();
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime().addPacket({
    kind: framework.ZLinkActorPacketKind.Request,
    packetName: 'JoinRoom',
    actorType: PlayerActor,
    handlerType: JoinHandler
  });
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: { context: { meshName: 'game' } },
    spotId: () => 'entry-a',
    registry,
    resolveActor: () => actor,
    onDisconnectActor: async () => {}
  });
  const parts = [
    Message.from(Buffer.from(streamProtocol.encodeStreamHeader({
      kind: streamProtocol.ZLinkStreamMessageKind.Request,
      codec: streamProtocol.ZLinkStreamCodec.Json,
      flags: streamProtocol.ZLinkStreamHeaderFlags.None,
      requestSeq: 1n,
      name: 'JoinRoom',
      metadata: new Map()
    }))),
    Message.from(Buffer.from('{}'))
  ];
  let replies = 0;

  const result = await dispatch.dispatch(
    'alice',
    parts,
    true,
    undefined,
    undefined,
    (reply) => {
      replies += 1;
      events.push(`reply:${reply.accepted}`);
    }
  );

  assert.equal(result, undefined);
  assert.equal(replies, 1);
  assert.deepEqual(events, [
    'handler',
    'reply:true',
    'completion:0123456789abcdef:fedcba9876543210'
  ]);
});

test('deferred Join is discarded before target admission when reply encoding fails', async () => {
  const events = [];

  await assert.rejects(
    runActorHandlerWithDeferredJoins(
      () => {
        deferActorJoin({
          requestBytes: 0,
          prepare() {
            events.push('join:prepare');
          },
          discard() {
            events.push('join:discard');
          },
          async execute() {
            events.push('join:execute');
          }
        });
        return 'reply';
      },
      () => {
        events.push('reply:send');
      },
      () => {
        events.push('reply:encode');
        throw new Error('reply encoding failed');
      }
    ),
    /reply encoding failed/
  );

  assert.deepEqual(events, ['reply:encode', 'join:discard']);
});

test('deferred Join continues after reply transport fails once encoding succeeded', async () => {
  const events = [];

  await assert.rejects(
    runActorHandlerWithDeferredJoins(
      () => {
        deferActorJoin({
          requestBytes: 0,
          prepare() {
            events.push('join:prepare');
          },
          discard() {
            events.push('join:discard');
          },
          async execute() {
            events.push('join:execute');
          }
        });
        return 'reply';
      },
      () => {
        events.push('reply:transport');
        throw new Error('reply transport failed');
      },
      () => {
        events.push('reply:encode');
      }
    ),
    /reply transport failed/
  );

  assert.deepEqual(events, [
    'reply:encode',
    'join:prepare',
    'reply:transport',
    'join:execute'
  ]);
});

test('deferred Actor Join is discarded when request reply encoding fails', async () => {
  const events = [];
  class PlayerActor {
    constructor() {
      this.actorId = 'alice';
    }
  }
  class JoinHandler {
    async handle() {
      deferActorJoin({
        requestBytes: 0,
        discard() {
          events.push('discard');
        },
        async execute() {
          events.push('execute');
        }
      });
      return { accepted: true };
    }
  }
  const actor = new PlayerActor();
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime().addPacket({
    kind: framework.ZLinkActorPacketKind.Request,
    packetName: 'JoinRoom',
    actorType: PlayerActor,
    handlerType: JoinHandler
  });
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: { context: { meshName: 'game' } },
    spotId: () => 'entry-a',
    registry,
    resolveActor: () => actor,
    onDisconnectActor: async () => {}
  });
  const parts = [
    Message.from(Buffer.from(streamProtocol.encodeStreamHeader({
      kind: streamProtocol.ZLinkStreamMessageKind.Request,
      codec: streamProtocol.ZLinkStreamCodec.Json,
      flags: streamProtocol.ZLinkStreamHeaderFlags.None,
      requestSeq: 1n,
      name: 'JoinRoom',
      metadata: new Map()
    }))),
    Message.from(Buffer.from('{}'))
  ];

  await assert.rejects(
    dispatch.dispatch(
      'alice',
      parts,
      true,
      undefined,
      undefined,
      () => { throw new Error('reply encoding failed'); }
    ),
    /reply encoding failed/
  );
  assert.deepEqual(events, ['discard']);
});

test('deferred Actor Join is discarded when the handler fails', async () => {
  const events = [];
  const { context } = actorHarness(events);

  await assert.rejects(
    runActorHandlerWithDeferredJoins(async () => {
      context.joinSpot('room-a').defer();
      throw new Error('handler failed');
    }),
    /handler failed/
  );
  assert.deepEqual(events, []);
});

test('deferred Actor Join rejects a request over 1 MiB without retaining partial registrations', async () => {
  const events = [];

  await assert.rejects(
    runActorHandlerWithDeferredJoins(() => {
      deferActorJoin({
        requestBytes: 1,
        discard() {
          events.push('first:discard');
        },
        async execute() {
          events.push('first:execute');
        }
      });
      deferActorJoin({
        requestBytes: 1024 * 1024 + 1,
        discard() {
          events.push('oversized:discard');
        },
        async execute() {
          events.push('oversized:execute');
        }
      });
    }),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotConfigured
  );

  assert.deepEqual(events, [
    'oversized:discard',
    'first:discard'
  ]);
});

test('Actor Join defer rejects detached use, duplicate terminal, and a second pending transition', async () => {
  const events = [];
  const { context, state } = actorHarness(events);

  assert.throws(() => context.joinSpot('room-a').defer(), /handler scope/);
  await runActorHandlerWithDeferredJoins(async () => {
    const call = context.joinSpot('room-a');
    call.defer();
    assert.throws(
      () => call.defer(),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
    );
    assert.throws(
      () => context.joinSpot('room-b').defer(),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
    );
    assert.throws(
      () => state.beginMove(),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
        && !('isRetriable' in error)
    );
  });
  await waitForEvents(events, 2);
  const [join, completion] = events.slice(-2);
  assert.match(join, /^join:alice:alice:room-a::\d+$/);
  const remainingTimeout = Number(join.split(':').at(-1));
  assert.ok(remainingTimeout > 0 && remainingTimeout <= 5_000);
  assert.equal(completion, 'completion:accepted:7');
});

test('deferred Actor Join reports an application failure with the public InternalFailure kind', async () => {
  const events = [];
  const failure = new Error('application admission failed');
  const coordinator = {
    async joinSpot() {
      throw failure;
    },
    async joinEntrySpot() {
      throw new Error('unexpected Entry Spot join');
    }
  };
  const state = new ZLinkActorRuntimeState('bob');
  const failingContext = new DefaultZLinkActorContext(
    state,
    coordinator,
    undefined,
    undefined,
    () => 'game',
    undefined
  );
  const failingActor = {
    actorId: 'bob',
    context: failingContext,
    async onJoinCompleted(completion) {
      events.push(completion);
    }
  };
  state.bindActor(failingActor, failingContext);

  await runActorHandlerWithDeferredJoins(async () => {
    failingContext.joinSpot('room-a').defer();
  });
  await waitForEvents(events, 1);

  assert.equal(events[0].status, 'failed');
  assert.equal(events[0].kind, framework.ZLinkFrameworkErrorKind.InternalFailure);
  assert.equal('isRetriable' in events[0], false);
  assert.equal(typeof events[0].operationId.high, 'bigint');
  assert.equal(typeof events[0].operationId.low, 'bigint');
});
