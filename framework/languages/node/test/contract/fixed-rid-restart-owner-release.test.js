'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { randomUUID } = require('node:crypto');
const { mkdtemp, readFile, rm } = require('node:fs/promises');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const test = require('node:test');
const {
  ZLinkFrameworkRuntimeState,
  ZLinkFrameworkRuntimeHost,
  ZLinkFrameworkTerminationOutcome,
  ZLinkInMemoryLocationStore,
  ZLinkInMemoryProviderLocationStore,
  ZLinkInternalMeshNodeWriteIntent,
  ZLinkLocationStoreRepository,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  createFrameworkRegistrationWithBuilder
} = require('../../packages/framework/dist/internal');
const {
  ZLinkOwnerLeaseReleaseDelayPendingError
} = require('../../packages/framework/dist/runtime/locations/owner-lease-release-delay');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');
const { aggregateDescriptor } = require('./helpers/location-aggregate-fixture');
const { connectTestRedis, testRedisUrl } = require('./helpers/test-redis');

const meshName = 'cross.entry-join';
const nodeRid = 'node-entry-join-target';

async function claimOwner(repository, ownerId) {
  const result = await repository.claimOwnerLease(ownerId, 30_000);
  assert.equal(result.kind, 'claimed');
  return result.token;
}

for (const storeKind of ['repository', 'in-memory']) {
  test(`fixedRidRestartClaimsOnlyAfterPreviousOwnerRelease (${storeKind})`, async (t) => {
    for (const reissued of [false, true]) {
      await t.test(
        reissued ? 'different token at predecessor key' : 'missing predecessor lease',
        async (t) => {
          const marginMs = 5;
          const storeNow = new Date('2026-11-26T00:00:00Z');
          let monotonicMs = 0;
          const provider =
            storeKind === 'repository'
              ? new ZLinkInMemoryProviderLocationStore(() => storeNow)
              : undefined;
          const store =
            provider === undefined
              ? new ZLinkInMemoryLocationStore(
                  () => storeNow,
                  () => monotonicMs,
                  marginMs
                )
              : new ZLinkLocationStoreRepository(
                  provider,
                  () => storeNow,
                  undefined,
                  () => monotonicMs,
                  marginMs
                );
          const predecessor = await claimOwner(store, 'predecessor');
          const successor = await claimOwner(store, 'successor');
          const before = aggregateDescriptor(
            {
              meshName,
              nodeRid,
              nodeLifecycleGeneration: 1n,
              owner: predecessor
            },
            8
          );
          const after = aggregateDescriptor(
            {
              meshName,
              nodeRid,
              nodeLifecycleGeneration: 2n,
              owner: successor
            },
            8
          );
          assert.equal(
            (await store.updateMeshNode(before, ZLinkLocationWriteIntent.NewClaim)).status,
            ZLinkLocationWriteStatus.Stored
          );
          const claim = () =>
            store.updateMeshNode(after, ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim);
          const writes = provider === undefined ? undefined : t.mock.method(provider, 'write').mock;

          const live = await store.readOwnerLease(predecessor.ownerId);
          assert.equal(live.kind, 'found');
          assert.deepEqual(live.token, predecessor);
          assert.ok(live.leaseExpiresAt > live.storeNow);
          assert.equal((await claim()).status, ZLinkLocationWriteStatus.RejectedConflict);
          if (writes !== undefined)
            assert.equal(writes.callCount(), 0, 'Conflict must not submit a descriptor CAS');

          assert.equal(await store.releaseOwnerLease(predecessor), 'released');
          const reissue = reissued ? await claimOwner(store, predecessor.ownerId) : undefined;
          const exact = await store.readOwnerLease(predecessor.ownerId);
          if (reissued) {
            assert.equal(exact.kind, 'found');
            assert.deepEqual(exact.token, reissue);
            assert.notEqual(exact.token.leaseGeneration, predecessor.leaseGeneration);
            assert.ok(exact.leaseExpiresAt > exact.storeNow);
          } else {
            assert.equal(exact.kind, 'missing');
          }
          writes?.resetCalls();
          await assert.rejects(claim, ZLinkOwnerLeaseReleaseDelayPendingError);
          monotonicMs = 3 * marginMs - 1;
          await assert.rejects(claim, ZLinkOwnerLeaseReleaseDelayPendingError);
          if (writes !== undefined)
            assert.equal(writes.callCount(), 0, 'Release wait must not submit a descriptor CAS');

          monotonicMs = 3 * marginMs;
          const result = await claim();
          t.diagnostic(
            `elapsed=${monotonicMs}ms previous=${predecessor.leaseGeneration} exact=${exact.token?.leaseGeneration ?? exact.kind} status=${result.status} cas=${writes?.callCount() ?? 'in-memory'}`
          );
          assert.equal(
            result.status,
            ZLinkLocationWriteStatus.Stored,
            'After 3 × margin, the invalid predecessor token must allow the fixed RID claim'
          );
          if (writes !== undefined) assert.equal(writes.callCount(), 1);
          const {
            items: [published]
          } = await store.listMeshNodes(meshName);
          assert.equal(String(published.rid), nodeRid);
          assert.equal(published.lifecycleGeneration, after.lifecycleGeneration);
          assert.equal(published.ownerId, successor.ownerId);
          assert.equal(published.leaseGeneration, successor.leaseGeneration);
        }
      );
    }
  });
}

test('normal host shutdown releases the exact fixed RID owner lease', async (t) => {
  const client = await connectTestRedis(t);
  if (client === undefined) return;
  const keyPrefix = `zlink:fixed-rid-normal:${randomUUID()}`;
  const openStore = () => new ZLinkRedisLocationStore({ url: testRedisUrl, keyPrefix });
  const provider = openStore();
  const repository = new ZLinkLocationStoreRepository(provider);
  const clock = fixedRidHostClock(t);
  const registration = createFrameworkRegistrationWithBuilder((builder) => {
    builder.addLocationStore(openStore());
    builder.configureLocations().ownerLeaseFencingMarginMs(1);
    builder.addRouteMesh(meshName).listen('tcp://127.0.0.1:0').routingId(nodeRid);
  });
  const host = new ZLinkFrameworkRuntimeHost({ registration });
  t.after(async () => {
    try {
      await host.stop();
      const keys = await client.keys(`${keyPrefix}:*`);
      if (keys.length > 0) await client.del(keys);
    } finally {
      await Promise.all([provider.dispose(), client.quit()]);
    }
  });

  await host.start();
  await finishFixedRidHostStartup(host, clock, ZLinkFrameworkRuntimeState.Preparing);
  assert.equal(host.status.state, ZLinkFrameworkRuntimeState.Serving);
  const {
    items: [published]
  } = await repository.listMeshNodes(meshName);
  assert.equal(String(published.rid), nodeRid);
  const before = await repository.readOwnerLease(published.ownerId);
  assert.equal(before.kind, 'found');
  assert.equal(before.token.leaseGeneration, published.leaseGeneration);
  assert.ok(before.leaseExpiresAt > before.storeNow);

  const result = await host.shutdown();
  assert.equal(result.outcome, ZLinkFrameworkTerminationOutcome.Stopped);
  assert.equal((await repository.readOwnerLease(published.ownerId)).kind, 'missing');
});

test(
  'Entry Join Node releases the predecessor lease when sent SIGTERM after publication',
  {
    timeout: 55_000,
    skip: process.platform !== 'linux'
  },
  async (t) => {
    const client = await connectTestRedis(t);
    if (client === undefined) return;
    const prefix = `zlink:fixed-rid-restart:${randomUUID()}`;
    const provider = new ZLinkRedisLocationStore({
      url: testRedisUrl,
      keyPrefix: `${prefix}:location`
    });
    const repository = new ZLinkLocationStoreRepository(provider);
    const dir = await mkdtemp(join(tmpdir(), 'fixed-rid-restart-'));
    const processes = [];
    t.after(async () => {
      try {
        for (const { child, exit } of processes) {
          if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
          await exit;
        }
        const keys = await client.keys(`${prefix}:*`);
        if (keys.length > 0) await client.del(keys);
      } finally {
        await Promise.all([
          provider.dispose(),
          client.quit(),
          rm(dir, { recursive: true, force: true })
        ]);
      }
    });

    function startHost(mode, label) {
      const eventFile = join(dir, `${label}.events`);
      const readyFile = join(dir, `${label}.ready`);
      const child = spawn(process.execPath, [
        join(__dirname, '../../cross-language/user_spot_join_host.js'),
        mode,
        '--mesh-name',
        meshName,
        '--redis-endpoint',
        new URL(testRedisUrl).host,
        '--redis-key-prefix',
        prefix,
        '--node-rid',
        nodeRid,
        '--bind-endpoint',
        'tcp://127.0.0.1:0',
        '--spot-id',
        'restart-spot',
        '--actor-id',
        'restart-actor',
        '--event-file',
        eventFile,
        '--ready-file',
        readyFile
      ]);
      let output = '';
      child.stdout.on('data', (chunk) => {
        output += chunk;
      });
      child.stderr.on('data', (chunk) => {
        output += chunk;
      });
      const exit = new Promise((resolve, reject) => {
        child.once('error', reject);
        child.once('close', (code, signal) => resolve({ code, signal }));
      });
      const host = { child, exit, eventFile, readyFile, output: () => output };
      processes.push(host);
      return host;
    }

    const host = startHost('user-spot-join-source', 'predecessor');
    const deadline = Date.now() + 40_000;
    let published;
    while (published === undefined && Date.now() < deadline) {
      assert.equal(host.child.exitCode, null, host.output());
      const ready = await readFile(host.readyFile, 'utf8').catch((error) => {
        if (error.code !== 'ENOENT') throw error;
      });
      if (ready !== undefined) {
        const { items } = await repository.listMeshNodes(meshName);
        published = items.find(
          (row) => String(row.rid) === nodeRid && row.state === ZLinkFrameworkRuntimeState.Serving
        );
      }
      if (published === undefined) await delay(25);
    }
    assert.ok(published, `Predecessor must publish its Serving descriptor: ${host.output()}`);
    const before = await repository.readOwnerLease(published.ownerId);
    assert.equal(before.kind, 'found');
    assert.equal(before.token.leaseGeneration, published.leaseGeneration);
    assert.ok(before.leaseExpiresAt > before.storeNow);

    assert.equal(host.child.kill('SIGTERM'), true);
    const status = await host.exit;
    assert.deepEqual(status, { code: null, signal: 'SIGTERM' }, host.output());
    const after = await repository.readOwnerLease(published.ownerId);
    t.diagnostic(
      `predecessor exit=${JSON.stringify(status)} exact=${showLease(after)} before=${showLease(before)}`
    );
    const flow = await readFile(`${host.eventFile}.flow`, 'utf8');
    t.diagnostic(
      `predecessor flow records=${flow.split('\n').filter(Boolean).length} output=${host.output().trim()}`
    );

    if (
      after.kind === 'found' &&
      after.token.leaseGeneration === published.leaseGeneration &&
      after.leaseExpiresAt > after.storeNow
    ) {
      const replacement = startHost('user-spot-target', 'successor');
      const result = await replacement.exit;
      t.diagnostic(
        `replacement exit=${JSON.stringify(result)} output=${replacement.output().trim()}`
      );
      assert.equal(result.code, 1);
      assert.match(replacement.output(), /descriptor publication failed: rejectedConflict/);
    }
    assert.equal(
      after.kind,
      'missing',
      'The process exited without releasing the exact predecessor lease'
    );
  }
);

function showLease(lease) {
  if (lease.kind === 'missing') return 'missing';
  const { ownerId, leaseGeneration } = lease.token;
  return `owner=${ownerId} generation=${leaseGeneration} StoreNow=${lease.storeNow.toISOString()} ExpiresAt=${lease.leaseExpiresAt.toISOString()}`;
}
