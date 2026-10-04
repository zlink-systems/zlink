const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

// Only the Store response and restored object are controlled. The attempt lane,
// cutover validation, authority retry, finalize and shutdown paths are production.
function targetFixture(runtime = new ZLinkHostServiceRelocationRuntime({}), meshName = 'mesh') {
  const coordinator = {
    ownerId: 'source-owner',
    leaseGeneration: 3n,
    nodeRid: 'source',
    nodeGeneration: 2n,
    expectedAuthorityStoreVersion: 'source-v1'
  };
  const object = {
    kind: 'actor',
    actorId: 'actor',
    objectGeneration: 5n,
    expectedAuthorityOwnerGeneration: 11n
  };
  const prepare = {
    relocation: { high: 0n, low: 17n },
    targetAttemptGeneration: 1n,
    coordinator,
    object,
    target: {
      nodeRid: 'target',
      nodeGeneration: 6n,
      ownerId: 'target-owner',
      ownerLeaseGeneration: 14n
    }
  };
  const events = [];
  const authority = { kind: 'snapshot', ownerId: 'target-owner' };
  const prepared = {
    fence: { aggregateId: { value: 'aggregate' }, aggregateGeneration: 1n },
    plan: { targetDescriptor: { meshName } }
  };
  const stage = {
    offer: {
      prepare,
      authenticatedSourceNodeRid: 'source',
      prepareFingerprint: 'exact-prepare',
      reservation: { prepared }
    },
    staging: {
      primaryAuthorityKey: { value: 'actor-key' },
      hidden: new Map(),
      envelope: { participants: [] }
    },
    owner: {
      async abort() {
        events.push('abort');
      },
      async normalize() {
        events.push('normalize');
      },
      async publish() {
        events.push('publish');
      },
      async openAdmission() {
        events.push('open');
      }
    },
    phase: 'ready',
    lane: Promise.resolve(),
    cutoverReceived: false,
    boundaryRelay: [],
    releaseActivation() {
      events.push('release');
    }
  };
  const store = {
    async commitAggregate() {
      events.push('cas');
      return { kind: 'committed' };
    },
    async abortAggregate() {
      events.push('reservation-abort');
      return { kind: 'aborted' };
    },
    async readOwnerLease(ownerId) {
      const storeNow = new Date();
      return {
        kind: 'found',
        token: { ownerId, leaseGeneration: 14n },
        storeNow,
        leaseExpiresAt: new Date(storeNow.getTime() + 60_000)
      };
    }
  };
  runtime.options.locationStore = () => store;
  runtime.options.spotManager = () => undefined;
  // Read reconciliation results, including unknown, are delivered independently
  // of CAS. This isolates the shutdown decision from provider serialization.
  runtime.readAggregateForCommitRetry = async () => ({
    kind: 'committed',
    authorities: new Map([['actor-key', authority]])
  });
  runtime.clearTargetRelocationPublication = async () => {};
  runtime.targetReplyRelayCoordinator = () => coordinator;
  runtime.relayTerminalReplies = async () => {};
  runtime.targetStages.set('0:17:1', stage);
  const cutover = {
    ...prepare,
    kind: 'cutover',
    senderRole: 'source',
    boundaryRecordCount: 0n,
    boundaryChecksumCrc32c: 0
  };
  return {
    runtime,
    stage,
    events,
    store,
    authority,
    finalize: () => runtime.beginTargetFinalize(meshName, '0:17:1', stage),
    cutover: () => runtime.handleOneWayControl(meshName, cutover, 'source')
  };
}

for (const warningStarted of [false, true]) {
  test(
    `target shutdown seals READY before cutover (Warning settlement=${warningStarted})`,
    { timeout: 1500 },
    async () => {
      const meshName = `reloc-stop-${warningStarted}-${process.pid}`;
      const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
        builder.addRouteMesh(meshName).listen(`inproc://${meshName}`).routingId('target');
      });
      const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
      let fixture;
      let finalize;
      const entered = deferred();
      const read = deferred();
      try {
        await host.start();
        fixture = targetFixture(host.serviceRelocation, meshName);
        if (warningStarted) {
          fixture.runtime.readAggregateForCommitRetry = async () => {
            entered.resolve();
            return read.promise;
          };
          finalize = fixture.finalize();
          finalize.catch(() => {});
          await entered.promise;
        }
        const shutdown = host.shutdown({ deadlineMs: 1000 });
        // A cutover delivered after the seal, while a prior Store read is still
        // pending, must not claim publication for this attempt.
        await fixture.cutover();
        const result = await shutdown;
        assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
        assert.equal(fixture.stage.cutoverReceived, false);
        assert.equal(fixture.events.filter((event) => event === 'abort').length, 1);
        assert.equal(fixture.events.includes('cas'), false);
        assert.equal(fixture.runtime.targetStages.size, 0);
        await fixture.cutover();
        assert.equal(fixture.events.includes('cas'), false);
        read.resolve({ kind: 'source' });
        if (finalize !== undefined) await finalize;
      } finally {
        read.resolve({ kind: 'stale' });
        await host.stop();
      }
    }
  );
}

test(
  'a queued cutover cannot verify after shutdown seals its attempt lane',
  { timeout: 1500 },
  async () => {
    const fixture = targetFixture();
    const lane = deferred();
    fixture.stage.lane = lane.promise;
    const cutover = fixture.cutover();
    const stopped = fixture.runtime.dispose();
    lane.resolve();
    await Promise.all([cutover, stopped]);
    assert.equal(fixture.stage.cutoverReceived, false);
    assert.equal(fixture.events.includes('cas'), false);
    assert.equal(fixture.events.filter((event) => event === 'abort').length, 1);
  }
);

test(
  'shutdown cleans staging after cutover boundary validation rejects its lane',
  { timeout: 1500 },
  async () => {
    const fixture = targetFixture();
    const prepare = fixture.stage.offer.prepare;
    await assert.rejects(
      fixture.runtime.handleOneWayControl(
        'mesh',
        {
          ...prepare,
          kind: 'cutover',
          senderRole: 'source',
          boundaryRecordCount: 0n,
          boundaryChecksumCrc32c: 1
        },
        'source'
      ),
      /boundary confirmation mismatch/
    );
    await assert.rejects(fixture.runtime.dispose(), /boundary confirmation mismatch/);
    assert.equal(fixture.events.filter((event) => event === 'abort').length, 1);
    assert.equal(fixture.events.includes('cas'), false);
  }
);

for (const responseKind of ['pending', 'committed', 'unknown']) {
  const uncertain = responseKind !== 'pending';
  test(
    `verified cutover survives shutdown and completes CAS settlement (response=${responseKind})`,
    { timeout: 1500 },
    async (t) => {
      const fixture = targetFixture();
      const entered = deferred();
      const response = deferred();
      let reads = 0;
      fixture.store.commitAggregate = async () => {
        fixture.events.push('cas');
        if (!uncertain) {
          entered.resolve();
          await response.promise;
        }
        if (
          responseKind === 'unknown' &&
          fixture.events.filter((event) => event === 'cas').length === 1
        ) {
          const error = new Error('CAS response lost after commit');
          error.name = 'TimeoutError';
          throw error;
        }
        return { kind: 'committed' };
      };
      fixture.runtime.readAggregateForCommitRetry = async () => {
        reads += 1;
        if (uncertain && reads === 1) {
          entered.resolve();
          await response.promise;
        }
        return uncertain && reads === 1
          ? { kind: 'unknown' }
          : {
              kind: 'committed',
              authorities: new Map([['actor-key', fixture.authority]])
            };
      };
      const keepAlive = setInterval(() => {}, 1000);
      try {
        const cutover = fixture.cutover();
        cutover.catch(() => {});
        await entered.promise;
        assert.equal(fixture.stage.cutoverReceived, true);
        const stopped = fixture.runtime.dispose();
        stopped.catch(() => {});
        assert.equal(fixture.events.includes('abort'), false);
        response.resolve();
        await Promise.all([cutover, stopped]);
        assert.equal(fixture.events.includes('abort'), false);
        assert.equal(fixture.events.includes('open'), true);
        assert.equal(fixture.stage.phase, 'open');
        assert.equal(reads, uncertain ? 2 : 1);
      } finally {
        t.diagnostic(`attempt events: ${fixture.events.join(',')}`);
        response.resolve();
        clearInterval(keepAlive);
      }
    }
  );
}
