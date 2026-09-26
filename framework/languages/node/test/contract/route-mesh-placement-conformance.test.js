'use strict';

// Consumes framework/runtime/conformance/route-mesh-placement-v1.json: RouteMesh placement
// counts come from the reporting MeshNode's activation records, IsAvailable follows runtime
// monitoring §5 and MeshNode §5.1, and the placement reason comes from the single topology
// reason decision.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { RoutingId } = require('@zlink-systems/zlink');

const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const {
  runActorHandlerWithDeferredJoins
} = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');

const fixture = JSON.parse(fs.readFileSync(path.resolve(
  __dirname,
  '../../../../runtime/conformance/route-mesh-placement-v1.json'
), 'utf8'));

// The object kind a scenario keeps in flight. Its callback waits on `gate` until the
// expectations have been checked.
let held;

function holdGate(kind) {
  let enter;
  let release;
  const entered = new Promise((resolve) => { enter = resolve; });
  const gate = new Promise((resolve) => { release = resolve; });
  return { kind, entered, gate, enter, release };
}

async function waitIfHeld(kind) {
  if (held?.kind !== kind) return;
  held.enter();
  await held.gate;
}

class PlacementSpot {
  constructor(context) {
    this.context = context;
  }

  configure() {}

  async onInitialize() {
    await waitIfHeld('userSpot');
  }

  async onActorJoin() {
    await waitIfHeld('actorJoin');
    return { accepted: true };
  }

  async onJoinedActor() {}

  async onLeaveActor() {}
}

class PlacementInstanceSpot {
  constructor(context) {
    this.context = context;
  }

  configure() {}

  async onInitialize() {
    await waitIfHeld('instanceSpot');
  }
}

const createdActors = [];

class PlacementActor {
  constructor(context) {
    this.context = context;
  }
}

class PlacementActorFactory {
  async create(context) {
    await waitIfHeld('actor');
    const actor = new PlacementActor(context);
    createdActors.push(actor);
    return actor;
  }
}

// In-memory provider store whose transport a scenario can take away.
class ToggleLocationStore {
  constructor() {
    this.inner = new framework.ZLinkInMemoryProviderLocationStore();
    this.available = true;
  }

  read(key, signal) {
    return this.available ? this.inner.read(key, signal) : unavailable();
  }

  write(request, signal) {
    return this.available ? this.inner.write(request, signal) : unavailable();
  }

  scan(request, signal) {
    return this.available ? this.inner.scan(request, signal) : unavailable();
  }
}

function unavailable() {
  return Promise.reject(new Error('location store transport unavailable'));
}

const spotType = (meshName) => `placement-spot-${meshName}`;
const instanceSpotType = (meshName) => `placement-instance-${meshName}`;
const topologyStates = {
  ready: framework.ZLinkTopologyState.Ready,
  degraded: framework.ZLinkTopologyState.Degraded
};
async function runScenario(lease, scenario, index) {
  const store = new ToggleLocationStore();
  const builder = nestjs.zlinkFramework().addLocationStore(store);
  builder.configureLocations()
    .pollingIntervalMs(10)
    .ownerLeaseRenewIntervalMs(lease.renewIntervalMs)
    .ownerLeaseRenewTimeoutMs(lease.renewTimeoutMs)
    .ownerLeaseTtlMs(lease.ttlMs)
    .ownerLeaseFencingMarginMs(lease.fencingMarginMs);
  for (const node of scenario.meshNodes) {
    assert.equal(node.spotRelocation, 'disabled', `${scenario.name}:${node.meshName}`);
    const server = builder.addRouteMesh(node.meshName)
      .listen(`inproc://placement-${index}-${node.meshName}-${process.pid}`)
      .setActorLimit(node.actorLimit)
      .setSpotLimit(node.spotLimit)
      .setActivationConcurrency(node.activationConcurrency)
      .objects().server()
      .addSpotFactory(spotType(node.meshName), PlacementSpot, (factory) => factory.disableRelocation());
    if (node.actorFactory) {
      server.addActorFactory('placement-actor', PlacementActorFactory, (factory) => factory.disableRelocation());
    }
    if (node.instanceSpotFactory) {
      server.addInstanceSpotFactory(
        instanceSpotType(node.meshName),
        PlacementInstanceSpot,
        (factory) => factory.disableRelocation()
      );
    }
  }
  class PlacementModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())] })(PlacementModule);
  const app = await NestFactory.createApplicationContext(PlacementModule, {
    logger: false,
    abortOnError: false
  });
  held = undefined;
  createdActors.length = 0;
  let heldOperation;
  try {
    const spots = app.get(nestjs.ZLINK_SPOT_MANAGER, { strict: false });
    const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER, { strict: false });
    const host = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
    const runtime = app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME, { strict: false });
    const runtimeOptions = app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME_OPTIONS, { strict: false });
    const spotIds = [];
    let objectIndex = 0;
    const start = (kind, meshName) => {
      const name = `${scenario.name}-${kind}-${objectIndex++}`;
      switch (kind) {
        case 'actor':
          return actors.create(name, 'placement-actor').inMesh(meshName).timeout(10_000).submit();
        case 'userSpot':
          return spots.create(spotType(meshName)).inMesh(meshName).timeout(10_000).submit()
            .then((result) => { spotIds.push(result.spot.spotId); });
        case 'instanceSpot':
          // Cold activation as the target MeshNode receives it for a message to an Instance Spot id
          // that has no activation yet. An in-process message to the host's own MeshNode does not
          // reach this entry, so the consumer enters it directly.
          return host.spotManager.materializeInstance(
            meshName,
            instanceSpotType(meshName),
            RoutingId.from(name),
            1n
          );
        case 'actorJoin': {
          const actor = createdActors.at(-1);
          const spotId = spotIds.at(-1);
          assert.ok(actor !== undefined && spotId !== undefined, `${scenario.name}:actorJoin`);
          return new Promise((resolve, reject) => {
            actor.onJoinCompleted = async (completion) => {
              if (completion.status === 'accepted') resolve();
              else reject(new Error(`${scenario.name}: Actor Join ended ${completion.status}`));
            };
            runActorHandlerWithDeferredJoins(() => {
              actor.context.joinSpot(spotId).timeout(10_000).defer();
            }).catch(reject);
          });
        }
        default:
          throw new Error(`unknown object kind ${kind}`);
      }
    };
    for (const { kind, meshName, count, hold } of scenario.objects) {
      for (let created = 0; created < count; created += 1) {
        if (!hold) {
          await start(kind, meshName);
          continue;
        }
        held = holdGate(kind);
        heldOperation = start(kind, meshName);
        heldOperation.catch(() => undefined);
        // A send completes at submission, so its callback may start later; a failed operation
        // ends the wait with its error.
        let timer;
        await Promise.race([
          held.entered,
          heldOperation.then(() => held.entered),
          new Promise((_, reject) => {
            timer = setTimeout(
              () => reject(new Error(`${scenario.name}: held ${kind} never reached its callback`)),
              10_000
            );
          })
        ]).finally(() => clearTimeout(timer));
      }
    }
    const observations = new Map(scenario.meshNodes.map((node) => [
      node.meshName,
      runtime.observe(node.meshName, 8)[Symbol.asyncIterator]()
    ]));
    for (const node of scenario.meshNodes) {
      runtimeOptions.mesh(node.meshName).placementWeight = node.placementWeightAfterStartup;
    }
    for (const expected of scenario.expected) {
      const expectedState = topologyStates[expected.state];
      const events = observations.get(expected.meshName);
      const label = `${scenario.name}:${expected.meshName}`;
      let status = runtime.snapshot(expected.meshName);
      try {
        while (status.placement.isAvailable !== expected.isAvailable || status.state !== expectedState) {
          const observed = await Promise.race([
            events.next(),
            new Promise((_, reject) => {
              const timeout = setTimeout(() => reject(new Error(`${label}: status event not observed`)), 5_000);
              timeout.unref();
            })
          ]);
          assert.equal(observed.done, false, `${label}: observation ended`);
          status = observed.value.status;
        }
      } finally {
        await events.return?.();
      }
      assert.equal(status.placement.activeActorCount, expected.activeActorCount, `${label}:activeActorCount`);
      assert.equal(status.placement.activeSpotCount, expected.activeSpotCount, `${label}:activeSpotCount`);
      assert.equal(status.placement.isAvailable, expected.isAvailable, `${label}:isAvailable`);
      assert.equal(status.state, expectedState, `${label}:state`);
    }
  } finally {
    held?.release();
    await heldOperation?.catch(() => undefined);
    held = undefined;
    await app.close();
  }
}

test('RouteMesh placement consumes the shared per-MeshNode fixture', async (t) => {
  assert.equal(fixture.fixture, 'zlink.framework.route-mesh-placement');
  assert.equal(fixture.version, 1);
  let index = 0;
  for (const scenario of fixture.scenarios) {
    await t.test(scenario.name, async () => {
      await runScenario(fixture.ownerLease, scenario, index++);
    });
  }
});
