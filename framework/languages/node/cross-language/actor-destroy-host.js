/* SPDX-License-Identifier: Elastic-2.0 */

const assert = require('node:assert/strict');
const fs = require('node:fs');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../packages/framework/dist');
const nestjs = require('../packages/nestjs/dist');
const { ZLinkRedisLocationStore } = require('../packages/framework-locations-redis/dist');
const {
  DEFAULT_REQUEST_TIMEOUT_MS
} = require('../packages/framework/dist/contracts/Configuration/Registration');
const { ActorDestroyStore } = require('./actor-destroy-store');
const { observeDestroySubmissions } = require('./actor-destroy-wire');
const { until } = require('./outage-control');

const ACTOR_TYPE = 'cross-lang-actor-destroy-type';
const DESTROY_EVENTS = {
  duplicate: 'ownerDuplicateReturned',
  fail: 'failedDestroyReturned'
};
const DESTROY_LABELS = ['remote', 'duplicate', 'stale', 'fail', 'retry', 'admission'];

function escapeField(value) {
  return String(value).replace(/[%|\r\n]/g, (char) => encodeURIComponent(char));
}

function nowUs() {
  return Math.trunc((performance.timeOrigin + performance.now()) * 1000);
}

class ClosingCasFailure extends Error {}

function failure(error) {
  const messages = [];
  let reason;
  for (let cause = error; cause !== undefined; cause = cause?.cause) {
    messages.push(String(cause?.message ?? cause));
    if (cause instanceof ClosingCasFailure) reason = 'injectedClosingCasFailure';
  }
  const kind =
    error instanceof framework.ZLinkFrameworkException
      ? framework.ZLinkFrameworkErrorKind[error.kind]
      : 'error';
  return {
    kind: reason ? 'error' : kind === 'DeadlineExceeded' ? 'deadline' : kind,
    ...(reason && { reason }),
    message: messages.join(' · ')
  };
}

async function actorDestroyHost(owner, args, required, appendEvent, writeReady) {
  const scenario = required('actor-destroy-scenario');
  const actorId = required('actor-id');
  const meshName = required('mesh-name');
  const nodeRid = required('node-rid');
  const peerRid = required('peer-rid');
  const runId = required('actor-destroy-run-id');
  const commandFile = `${required('actor-destroy-control-prefix')}.command`;
  const startFile = required('start-file');
  required('event-file');
  assert(['a', 'b', 'c', 'd', 'e', 'f', 'admissionCheck'].includes(scenario), 'invalid scenario');

  const emit = (event, fields = {}, timeUs = nowUs()) => {
    const suffix = Object.entries(fields)
      .map(([key, value]) => `|${key}=${escapeField(value)}`)
      .join('');
    appendEvent(`actor-destroy|run=${escapeField(runId)}|event=${event}|timeUs=${timeUs}${suffix}`);
  };
  const delegate = new ZLinkRedisLocationStore({
    url: `redis://${required('redis-endpoint')}`,
    keyPrefix: `${required('redis-key-prefix')}:location`
  });
  const store = new ActorDestroyStore(delegate, args, owner, emit);
  const restoreSubmissionObserver =
    !owner && scenario === 'admissionCheck' ? observeDestroySubmissions(emit) : undefined;

  class Actor {
    constructor(context) {
      this.context = context;
    }
  }
  class Factory {
    async create(context) {
      if (store.creating) emit('newFactoryStarted');
      return new Actor(context);
    }
  }
  class EntrySpot {
    async onCreateActor() {
      return { accepted: true };
    }
    async onJoinedActor() {}
    async onLeaveActor() {}
  }
  class ActorDestroyProbe {
    constructor(label) {
      this.label = label;
    }
  }
  class ProbeHandler {
    async handle(_spot, actor, _context, request) {
      this.handled = true;
      emit('probeHandled', { label: request.label });
      return { generation: String(actor.context.objectGeneration) };
    }

    onModuleDestroy() {
      if (this.handled) emit('cleanup');
    }
  }
  Injectable()(Factory);
  Injectable()(EntrySpot);
  nestjs.zlinkEntrySpotActorRequestHandler({
    actor: () => Actor,
    entrySpot: () => EntrySpot,
    packetName: 'ActorDestroyProbe'
  })(ProbeHandler);

  const builder = nestjs.zlinkFramework().addLocationStore(store);
  builder.configureDispatch().messageFlow('normal');
  const meshBuilder = builder
    .addRouteMesh(meshName)
    .listen(required('bind-endpoint'))
    .routingId(nodeRid)
    .setPlacementWeight(owner ? 100 : 0);
  if (args['advertise-host']) meshBuilder.setAdvertiseHost(args['advertise-host']);
  meshBuilder.channel(meshName).server();
  meshBuilder
    .objects()
    .server()
    .addEntrySpot(EntrySpot, ACTOR_TYPE)
    .addActorFactory(ACTOR_TYPE, Factory, (factory) => factory.disableRelocation());
  class HostModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [Factory, EntrySpot, ProbeHandler]
  })(HostModule);
  let app;
  const operations = new Map();
  let oldRef;
  const abort = new AbortController();
  try {
    app = await NestFactory.createApplicationContext(HostModule, {
      logger: false,
      abortOnError: false
    });
    const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
    const client = app.get(nestjs.ZLINK_ACTOR_CLIENT);
    const mesh = app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME);
    const options = app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME_OPTIONS).mesh(meshName);
    const getOrCreate = async () => {
      const result = await actors
        .getOrCreate(actorId, ACTOR_TYPE)
        .inMesh(meshName)
        .submit(abort.signal);
      assert(result.status !== 'rejected', 'GetOrCreate was rejected by the Entry Spot');
      return result;
    };
    const meshDescriptor = async (rid) => {
      const page = await store.observer.listMeshNodes(meshName);
      return page.items.find((node) => String(node.rid) === rid);
    };
    const waitForPlacement = (rid, weight) =>
      until(
        async () => (await meshDescriptor(rid))?.placementWeight === weight,
        `placement descriptor for ${rid}`
      );
    await until(
      async () => (await meshDescriptor(nodeRid))?.entrySpotId,
      `Entry Spot for ${nodeRid}`
    );

    function start(
      key,
      invoke,
      complete = (_, error) => {
        if (error) emit('error', { operation: key, ...error });
      }
    ) {
      assert(!operations.get(key)?.pending, `operation ${key} is already pending`);
      const operation = { pending: true };
      operations.set(key, operation);
      operation.done = invoke()
        .then(
          (value) => complete(value),
          (error) => complete(undefined, failure(error))
        )
        .finally(() => {
          operation.pending = false;
        });
    }

    async function probe(label) {
      let result;
      try {
        const reply = await client
          .requestToActor(actorId, new ActorDestroyProbe(label))
          .submit(abort.signal);
        result = { kind: 'return', generation: reply.generation };
      } catch (error) {
        result = failure(error);
      }
      emit('probeReturned', { label, ...result });
      return result;
    }

    const commands = {
      create() {
        assert(owner && oldRef === undefined, 'create requires the initial owner');
        start('initial-create', async () => {
          const result = await actors
            .create(actorId, ACTOR_TYPE)
            .inMesh(meshName)
            .submit(abort.signal);
          assert.equal(result.status, 'created');
          oldRef = result.actor;
          await store.rememberOwner();
          const observed = await probe('initial');
          assert.equal(observed.generation, String(oldRef.objectGeneration), observed.message);
          emit('created', { generation: oldRef.objectGeneration });
        });
      },
      async placement(value) {
        assert.match(value ?? '', /^\d+$/);
        const weight = Number(value);
        options.placementWeight = weight;
        await waitForPlacement(nodeRid, weight);
      },
      async 'peer-placement'(value) {
        assert.match(value ?? '', /^\d+$/);
        await waitForPlacement(peerRid, Number(value));
      },
      async peer() {
        await until(
          () =>
            mesh
              .snapshot(meshName)
              .peers.some(
                (peer) =>
                  String(peer.nodeRid) === peerRid && peer.state === framework.ZLinkPeerState.Ready
              ),
          `peer ${peerRid}`
        );
      },
      find(label) {
        start(`find:${label}`, async () => {
          const found = await actors.find(actorId, abort.signal);
          if (found && !oldRef) {
            oldRef = found;
            await store.rememberOwner();
          }
          emit('found', { label, generation: found?.objectGeneration ?? 'none' });
        });
      },
      async authority(label) {
        emit('authority', { label, ...(await store.authority()) });
      },
      'hold-delete': () => store.arm('delete'),
      'resume-delete': () => store.release('delete', 'ownerDeleteResumed'),
      'hold-cas': () => store.arm('cas'),
      'fail-cas': () => {
        store.release(
          'cas',
          'closingCasFailed',
          new ClosingCasFailure('Injected Actor Closing CAS failure')
        );
      },
      destroy(label) {
        assert(DESTROY_LABELS.includes(label), `invalid destroy label ${label}`);
        assert(oldRef, 'the first ActorRef was not observed');
        const started = nowUs();
        start(
          label,
          () => store.observeOperation(label, () => actors.destroy(oldRef, abort.signal)),
          (value, error) => {
            emit(DESTROY_EVENTS[label] ?? 'destroyReturned', {
              label,
              ...(error ?? { kind: value })
            });
          }
        );
        if (label === 'admission') {
          emit('destroyDeadline', {}, started + DEFAULT_REQUEST_TIMEOUT_MS * 1000);
        }
      },
      'pending-destroy'(label) {
        emit(label === 'duplicate' ? 'ownerDuplicatePending' : 'destroyPending', {
          label,
          value: operations.get(label)?.pending === true
        });
      },
      'get-or-create'() {
        assert(!owner, 'get-or-create requires the caller');
        store.creating = true;
        start(
          'create',
          () => store.observeOperation('get-or-create', getOrCreate),
          (value, error) => {
            store.creating = false;
            emit(
              'getOrCreateReturned',
              error
                ? { ...error, kind: 'error', generation: 'none' }
                : {
                    kind: value.status === 'created' ? 'Created' : 'Existing',
                    generation: value.actor.objectGeneration
                  }
            );
          }
        );
      },
      'pending-create'() {
        emit('createPending', { value: operations.get('create')?.pending === true });
      },
      probe(label) {
        start(`probe:${label}`, () => probe(label));
      },
      async lease(label) {
        emit('lease', { label, kind: await store.lease() });
      },
      async 'wait-lease-expired'() {
        await until(
          async () => (await store.lease()) === 'expired',
          'original owner lease expiration'
        );
        emit('ownerLeaseExpired');
      },
      'end-lease': () => store.endLease(),
      'allow-admission'() {
        assert(!owner && scenario === 'admissionCheck', 'allow-admission requires its caller');
        fs.writeFileSync(`${required('actor-destroy-control-prefix')}.admission`, '');
      },
      async finish() {
        abort.abort();
        store.cancelHolds();
        await Promise.all([...operations.values()].map((operation) => operation.done));
        store.stopping = true;
        await app.close();
        app = undefined;
      }
    };

    emit('hostReady', { pid: process.pid });
    writeReady();
    await until(() => fs.existsSync(startFile), 'start file');
    for (let sequence = 1; ; sequence += 1) {
      const [verb, ...parameters] = await until(
        () => {
          if (args['stop-file'] && fs.existsSync(args['stop-file'])) return [undefined];
          let line;
          try {
            line = fs.readFileSync(commandFile, 'utf8');
          } catch (error) {
            if (error.code === 'ENOENT') return undefined;
            throw error;
          }
          const [run, number, ...command] = line.replace(/\r?\n$/, '').split('\t');
          if (run !== runId || Number(number) < sequence) return undefined;
          assert.equal(number, String(sequence), 'command sequence skipped');
          assert(
            command.length > 0 && line.endsWith('\n') && !/[\r\n]/.test(command.join('')),
            'invalid command line'
          );
          return command;
        },
        `actor-destroy command ${sequence}`,
        null
      );
      if (verb === undefined) {
        await commands.finish();
        return;
      }
      try {
        assert(Object.hasOwn(commands, verb), `unknown actor-destroy command ${verb}`);
        const command = commands[verb];
        assert(
          parameters.length === command.length && parameters.every(Boolean),
          `invalid arguments for actor-destroy command ${verb}`
        );
        await command(...parameters);
      } catch (error) {
        emit('error', { seq: sequence, verb, ...failure(error) });
        throw error;
      } finally {
        emit('ack', { seq: sequence });
      }
      if (verb === 'finish') return;
    }
  } finally {
    abort.abort();
    store.cancelHolds();
    store.stopping = true;
    try {
      await app?.close();
    } finally {
      restoreSubmissionObserver?.();
    }
  }
}

module.exports = { actorDestroyHost };
