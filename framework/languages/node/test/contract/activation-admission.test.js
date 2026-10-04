'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { ZLinkActivationAdmission } = framework;

test('activation count changes do not publish descriptors, while weight changes publish current counts', async () => {
  const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
    builder.addRouteMesh('game').objects().server();
  });
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
  const manager = new framework.ZLinkSpotNodeRuntimeManager({
    ...host.createSpotNodeRuntimeOptions(
      undefined,
      host.createDispatchErrorReporter(host.runtimeOrPreStartErrorSink)
    )
  });
  host.spotNodeRuntime = manager;
  manager.meshNodes.set('game', {
    status: () => ({
      routingId: 'game-node',
      lifecycleGeneration: 1n,
      localEndpoint: 'tcp://127.0.0.1:9400'
    }),
    async setPlacementWeight() {}
  });
  const writes = [];
  manager.locationAutoConnect = {
    runtime: {
      currentOwnerToken: { ownerId: 'owner-a', leaseGeneration: 1n },
      async writeMeshNode(descriptor) {
        writes.push(descriptor);
        return { status: framework.ZLinkLocationWriteStatus.Stored, updatedAt: new Date() };
      }
    }
  };
  await manager.publishMeshNodeState(framework.ZLinkFrameworkRuntimeState.Serving);
  const initialRevision = writes[0].descriptorRevision;
  for (let index = 0; index < 8; index += 1) {
    const release = await host.activationAdmission.acquire('game');
    release();
  }
  // Drain any publication already submitted by admission changes.
  await manager.statePublication;
  assert.equal(writes.length, 1);
  assert.equal(
    manager.publishedMeshNodeDescriptors.get('game').descriptorRevision,
    initialRevision
  );

  const release = await host.activationAdmission.acquire('game');
  try {
    manager.setRuntimePlacementWeight('game', 250);
    await manager.waitForRuntimeWeightPublication();
    assert.equal(writes.length, 2);
    assert.equal(writes[1].descriptorRevision, initialRevision + 1n);
    assert.equal(writes[1].placementWeight, 250);
    assert.equal(writes[1].activationConcurrency.active, 1);
  } finally {
    release();
  }
  await manager.statePublication;
  assert.equal(writes.length, 2);
});

test('a full MeshNode rejects activation admission without waiting for a release', async () => {
  const admission = new ZLinkActivationAdmission(() => 1);
  const release = await admission.acquire('game');

  await assert.rejects(admission.acquire('game'), /no activation admission headroom/);
  assert.deepEqual(admission.current('game'), { active: 1, limit: 1 });

  release();
  const nextRelease = await admission.acquire('game');
  assert.deepEqual(admission.current('game'), { active: 1, limit: 1 });
  nextRelease();
  assert.deepEqual(admission.current('game'), { active: 0, limit: 1 });
});
