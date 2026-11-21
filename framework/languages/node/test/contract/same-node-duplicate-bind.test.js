const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const connector = require('../../packages/stream-connector/dist');
const { createBrowserSafeStreamApp } = require('./helpers/browser-safe-stream-app');

const cases = JSON.parse(
  readFileSync('../../runtime/conformance/bound-session-relocation-v1.json', 'utf8')
).sameNodeDuplicateBind;

for (const callbackName of ['onDuplicateBind', 'onSessionBound']) {
  for (const mode of ['SpotWide', 'PerActor', 'Entry']) {
    test(`${callbackName} owns its lifecycle turn: ${mode}`, async () => {
      const {
        ZLinkSpotSerialTurnExecutor
      } = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
      const {
        ZLinkSpotSerialExecutor
      } = require('../../packages/framework/dist/runtime/spots/spot-serial-executor');
      const {
        SubmitResult
      } = require('../../packages/framework/dist/runtime/backend/runtime-values');
      const serial = new ZLinkSpotSerialTurnExecutor(mode === 'SpotWide');
      const executor = new ZLinkSpotSerialExecutor(
        serial,
        framework.ZLinkUserSpotExecutionMode[mode === 'Entry' ? 'PerActor' : mode],
        'spot-a'
      );
      const activation = { serialExecutor: executor };
      const manager = {
        options: {
          executeEntryActor: (_mesh, actorId, operation) =>
            framework.ZLinkEntrySpotActivation.prototype.executeActor.call(
              { spotSerialExecutor: executor },
              actorId,
              operation
            )
        },
        resolveApplicationActivation: () => activation,
        executeActorTurn: framework.DefaultZLinkSpotManager.prototype.executeActorTurn
      };
      let release;
      let entered;
      const held = new Promise((resolve) => {
        release = resolve;
      });
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const events = [];
      const actor = { actorId: 'actor-a', nodeRid: 'node-a', generation: 1n };
      const incoming = {
        sessionNodeRid: 'session-owner',
        sessionOwnerNodeGeneration: 4n,
        sessionRid: 'session-new',
        sessionOwnerId: 'owner',
        sessionOwnerLeaseGeneration: 1n,
        bindingGeneration: 5n
      };
      const current = { ...incoming, sessionRid: 'session-old', bindingGeneration: 3n };
      const turns = [];
      const state = {
        nativeActorRef: actor,
        actorId: 'actor-a',
        meshName: 'mesh-a',
        locationGeneration: 3n,
        isMoving: false,
        boundSession: current,
        actor: {
          context: { spotId: mode === 'Entry' ? undefined : 'spot-a' },
          async [callbackName]() {
            turns.push(
              require('../../packages/framework/dist/runtime/execution').captureZLinkSpotSerialTurn()
            );
            events.push('callback:start');
            entered();
            await held;
            turns.push(
              require('../../packages/framework/dist/runtime/execution').captureZLinkSpotSerialTurn()
            );
            events.push('callback:end');
          }
        },
        installBoundSessionBinding(binding) {
          this.boundSession = binding;
        }
      };
      const host = {
        actorManager: { getState: () => state },
        spotNodeRuntime: {
          meshNode: () => ({ status: () => ({ routingId: 'node-a', lifecycleGeneration: 2n }) })
        },
        boundSessionRelay: { boundSessions: { resolveRemoteBoundSessionTarget: () => incoming } },
        spotManager: manager,
        options: { registration: { sessionReplacementCallbackTimeoutMs: 30000 } }
      };
      let replies = 0;
      const binding = framework.ZLinkFrameworkRuntimeHost.prototype.dispatchMeshActorBinding.call(
        host,
        'mesh-a',
        {
          kindData: {
            kind: 'actorBinding',
            transition: 'active',
            actor,
            actorNodeGeneration: 2n,
            authorityOwnerGeneration: 3n,
            ...incoming
          },
          reply() {
            replies++;
            return SubmitResult.Ok;
          },
          replyFailure() {
            assert.fail('valid binding was rejected');
          }
        }
      );
      await Promise.race([
        started,
        binding.then(() => {
          throw new Error('Binding completed without invoking its callback.');
        })
      ]);
      const siblings = [
        executor.executeActor('actor-b', (actorSerial) =>
          actorSerial.execute(async () => {
            events.push('actor');
          })
        ),
        executor.executeSpot(async () => {
          events.push('spot');
        }),
        executor.executeTimer('timer', async () => {
          events.push('timer');
        })
      ];
      const sameActor = executor.executeActor('actor-a', (actorSerial) =>
        actorSerial.execute(async () => {
          events.push('same-actor');
        })
      );
      try {
        await new Promise((resolve) => setImmediate(resolve));
        assert.equal(replies, callbackName === 'onSessionBound' ? 1 : 0);
        assert.deepEqual(
          state.boundSession,
          callbackName === 'onSessionBound' ? incoming : current
        );
        if (mode === 'SpotWide') {
          assert.deepEqual(events, ['callback:start']);
        } else {
          await Promise.all(siblings);
          assert.deepEqual([...events].sort(), ['actor', 'callback:start', 'spot', 'timer']);
        }
        assert.ok(turns[0], 'callback must enter a framework handler turn');
        assert.equal(turns[0].work.lane, 'lifecycle');
        assert.equal(
          executor.actorExecutors.get('actor-a').scheduler.activeLifecycle.lane,
          'lifecycle'
        );
      } finally {
        release();
        await binding;
        await Promise.all(siblings);
        await sameActor;
        await executor.closeChildren();
      }
      assert.ok(turns[1], 'async continuation must retain its framework handler turn');
      assert.equal(turns[0], turns[1]);
      assert.equal(replies, 1);
      assert.equal(state.boundSession.sessionRid, incoming.sessionRid);
      if (mode === 'SpotWide') assert.equal(events[1], 'callback:end');
      assert.ok(events.indexOf('same-actor') > events.indexOf('callback:end'));
    });
  }
}

for (const scenario of cases) {
  test(`public same-node duplicate bind: ${scenario.name}`, { timeout: 20000 }, async (t) => {
    let callbacks = 0;
    const notifications = [];
    let replacements = 0;
    let ingress = 0;
    let player;
    let connected;
    const clients = [];
    class Player {
      constructor(context) {
        this.context = context;
        player = this;
      }
      onSessionBound(incoming, signal) {
        assert.equal(signal.aborted, false);
        notifications.push(incoming);
      }
      async onDuplicateBind() {
        callbacks++;
        return scenario.rejectionMessage || undefined;
      }
    }
    class Factory {
      async create(context) {
        return new Player(context);
      }
    }
    class Entry {
      constructor(context) {
        this.context = context;
      }
      configure() {}
      async onJoinedActor() {}
      async onLeaveActor() {}
      async onDisconnectActor() {}
    }
    class ProbeHandler {
      async handle(_spot, _actor, _context, request) {
        ingress++;
        return request.value;
      }
    }
    nestjs.zlinkEntrySpotActorRequestHandler({
      entrySpot: () => Entry,
      actor: () => Player,
      packetName: 'Probe'
    })(ProbeHandler);
    class SessionFactory {
      async create(context) {
        const session = {
          context,
          async onConnected() {
            connected(session);
          },
          async onActorBindingReplaced() {
            replacements++;
          },
          async onDispatch(dispatch, message) {
            if (dispatch.actor) await dispatch.actor.relay(dispatch, message);
          }
        };
        return session;
      }
    }
    const builder = nestjs
      .zlinkFramework()
      .addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
    builder.configureDispatch().messageFlow('normal');
    builder
      .addRouteMesh('binding')
      .listen('tcp://127.0.0.1:0')
      .objects()
      .server()
      .addEntrySpot(Entry, 'player')
      .addActorFactory('player', Factory, (factory) => factory.disableRelocation());
    builder
      .addStreamNode('clients')
      .bind('ws://127.0.0.1:*')
      .enableActorDispatch()
      .registerSession(SessionFactory);
    class AppModule {}
    Module({
      imports: [nestjs.ZLinkModule.forRoot(builder.build())],
      providers: [Factory, SessionFactory, ProbeHandler]
    })(AppModule);
    const app = await createBrowserSafeStreamApp(
      () => NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false }),
      'clients'
    );
    t.after(async () => {
      for (const client of clients.reverse()) await client.close();
      await app.close();
    });
    const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
    await actors.create('same-node-player', 'player').inMesh('binding').submit();
    const actor = await actors.find('same-node-player');
    async function open() {
      const sessionReady = new Promise((resolve) => {
        connected = resolve;
      });
      const client = connector.zlinkStreamConnectorFactory.create({
        endpoint: app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).getListenerStatus('stream', 'clients')
          .endpoint,
        dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
        reconnect: { enabled: false }
      });
      clients.push(client);
      let actorBound;
      const bound = new Promise((resolve) => {
        actorBound = resolve;
      });
      client.onActorBound(actorBound);
      let closed;
      const disconnected = new Promise((resolve) => {
        closed = resolve;
      });
      client.onDisconnected(closed);
      await client.connect();
      return { client, session: await sessionReady, bound, disconnected };
    }
    const current = await open();
    await current.session.context.actors.bind(actor);
    const oldActor = await current.bound;
    assert.equal(
      await oldActor.request({ value: 'before' }).packetName('Probe').submit(),
      'before'
    );
    const incoming = await open();
    if (scenario.accepted) {
      await incoming.session.context.actors.bind(actor);
      const successor = await incoming.bound;
      assert.equal(
        await successor.request({ value: 'current' }).packetName('Probe').submit(),
        'current'
      );
      let oldPushes = 0;
      let pushed;
      const delivered = new Promise((resolve) => {
        pushed = resolve;
      });
      current.client.on('Push', () => {
        oldPushes++;
      });
      incoming.client.on('Push', pushed);
      await player.context.boundSession.send({ value: 'current' }).packetName('Push').submit();
      await delivered;
      assert.equal(oldPushes, 0);
      await assert.rejects(async () =>
        oldActor.request({ value: 'retired' }).packetName('Probe').submit()
      );
      await current.disconnected;
      assert.equal(ingress, 2);
    } else {
      await assert.rejects(
        () => incoming.session.context.actors.bind(actor),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Rejected
      );
      assert.ok(current.session.context.actors.find(actor.actorId));
      assert.equal(incoming.session.context.actors.find(actor.actorId), undefined);
      assert.equal(
        await oldActor.request({ value: 'current' }).packetName('Probe').submit(),
        'current'
      );
    }
    assert.equal(notifications.length, scenario.accepted ? 2 : 1);
    assert.ok(notifications.every((identity) => identity.bindingGeneration !== 0n));
    assert.equal(callbacks, scenario.callbackCount);
    assert.equal(replacements, scenario.replacementCount);
  });
}

for (const scenario of ['none', 'first', 'same', 'different', 'overlapping']) {
  const overlapping = scenario === 'overlapping';
  const name = overlapping
    ? 'Queued duplicate bind uses the current binding in its Actor turn'
    : `Actor binding gate admission: ${scenario}`;
  test(name, async () => {
    const {
      ZLinkSerialExecutionQueue
    } = require('../../packages/framework/dist/runtime/execution/serial-execution-queue');
    const {
      RequestResult, SubmitResult
    } = require('../../packages/framework/dist/runtime/backend/runtime-values');
    const queue = new ZLinkSerialExecutionQueue(async (record) => {
      record.resolve(await record.operation());
      record.release();
    });
    let release;
    let entered;
    const started = new Promise((resolve) => {
      entered = resolve;
    });
    const held = new Promise((resolve) => {
      release = resolve;
    });
    const handler = overlapping
      ? undefined
      : queue.submit(() => {
          entered();
          return held;
        });
    if (handler !== undefined) await started;
    const callbackInputs = [];
    const replies = [];
    const actor = { actorId: 'actor-a', nodeRid: 'node-a', generation: 1n };
    const incoming = {
      sessionNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 4n,
      sessionRid: 'session-a',
      sessionOwnerId: 'owner',
      sessionOwnerLeaseGeneration: 1n,
      bindingGeneration: 5n
    };
    const next = { ...incoming, sessionRid: 'session-b', bindingGeneration: 6n };
    const rejectionMessage = 'The first replacement is current.';
    const firstReply = { session: incoming.sessionRid, result: RequestResult.Ok };
    const current =
      scenario === 'first'
        ? undefined
        : {
            ...incoming,
            sessionRid: scenario === 'same' ? 'session-a' : 'session-old',
            bindingGeneration: 3n
          };
    const state = {
      nativeActorRef: actor,
      actorId: 'actor-a',
      meshName: 'mesh-a',
      locationGeneration: 3n,
      isMoving: false,
      boundSession: current,
      actor: {
        context: {},
        ...(scenario === 'none'
          ? {}
          : {
              async onDuplicateBind(bound, candidate) {
                callbackInputs.push(bound.bindingGeneration);
                if (overlapping && candidate.bindingGeneration === incoming.bindingGeneration) {
                  entered();
                  await held;
                }
                if (
                  candidate.bindingGeneration === next.bindingGeneration &&
                  bound.bindingGeneration === incoming.bindingGeneration
                ) {
                  return rejectionMessage;
                }
              }
            })
      },
      installBoundSessionBinding(binding) {
        this.boundSession = binding;
      }
    };
    const host = {
      actorManager: { getState: () => state },
      spotNodeRuntime: {
        meshNode: () => ({ status: () => ({ routingId: 'node-a', lifecycleGeneration: 2n }) })
      },
      boundSessionRelay: { boundSessions: { resolveRemoteBoundSessionTarget: () => incoming } },
      spotManager: {
        executeActorTurn: (_mesh, _spot, _actor, operation) =>
          queue.submit(operation, { lane: 'lifecycle' })
      },
      options: { registration: { sessionReplacementCallbackTimeoutMs: 30000 } }
    };
    const bind = (session) =>
      framework.ZLinkFrameworkRuntimeHost.prototype.dispatchMeshActorBinding.call(
        host,
        'mesh-a',
        {
          kindData: {
            kind: 'actorBinding',
            transition: 'active',
            actor,
            actorNodeGeneration: 2n,
            authorityOwnerGeneration: 3n,
            ...session
          },
          reply() {
            replies.push({ session: session.sessionRid, result: RequestResult.Ok });
            return SubmitResult.Ok;
          },
          replyFailure(result, _failureCode, tail) {
            replies.push({ session: session.sessionRid, result, tail });
            return SubmitResult.Ok;
          }
        }
      );
    const binding = bind(incoming);
    let secondBinding;
    try {
      if (overlapping) {
        await Promise.race([
          started,
          binding.then(() => {
            assert.fail('Binding completed without invoking its callback.');
          })
        ]);
        assert.deepEqual(callbackInputs, [current.bindingGeneration]);
        secondBinding = bind(next);
        assert.equal(queue.lifecycle.records.length, 1);
        assert.deepEqual(callbackInputs, [current.bindingGeneration]);
        assert.deepEqual(replies, []);
        assert.equal(state.boundSession, current);
      } else {
        await new Promise((resolve) => setImmediate(resolve));
        assert.deepEqual(callbackInputs, []);
        if (scenario === 'different') {
          assert.deepEqual(replies, []);
          assert.equal(state.boundSession, current);
        } else {
          assert.deepEqual(replies, [firstReply]);
          assert.deepEqual(state.boundSession, incoming);
        }
      }
    } finally {
      release();
      await handler;
      await Promise.all([binding, secondBinding]);
    }
    const expectedReplies = [firstReply];
    const expectedCallbackInputs =
      overlapping || scenario === 'different' ? [current.bindingGeneration] : [];
    if (overlapping) {
      expectedCallbackInputs.push(incoming.bindingGeneration);
      expectedReplies.push({
        session: next.sessionRid,
        result: RequestResult.Rejected,
        tail: { kind: 'streamBindRejected', rejectionMessage }
      });
    }
    assert.deepEqual(
      { callbackInputs, replies, current: state.boundSession },
      { callbackInputs: expectedCallbackInputs, replies: expectedReplies, current: incoming }
    );
  });
}
