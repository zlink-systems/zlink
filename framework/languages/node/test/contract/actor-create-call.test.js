const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} = require('../../packages/framework/dist');
const {
  DefaultZLinkActorManager,
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/internal');

function isInvalidOperation(error) {
  return error instanceof ZLinkFrameworkException
    && error.kind === ZLinkFrameworkErrorKind.InvalidOperation;
}

for (const operation of ['create', 'getOrCreate']) {
  for (const [option, duplicate] of [
    ['inMesh', 'other-mesh'],
    ['request', { value: 'replacement' }],
    ['timeout', 2000]
  ]) {
    test(`actor ${operation} rejects duplicate ${option} before effects and preserves options`, async () => {
      const submissions = [];
      let meshLookups = 0;
      const result = { status: 'rejected' };
      const manager = new DefaultZLinkActorManager({
        actorMeshNameProvider: () => {
          meshLookups += 1;
          return 'play';
        },
        placementCreate: async (...args) => {
          submissions.push(args);
          return result;
        }
      });
      const call = manager[operation]('alice', 'player')
        .inMesh('play').request(undefined).timeout(1000);
      assert.throws(() => call[option](duplicate), isInvalidOperation);
      assert.equal(meshLookups, 0);
      assert.deepEqual(submissions, []);

      const { signal } = new AbortController();
      assert.equal(await call.submit(signal), result);
      assert.deepEqual(submissions, [[
        'alice', 'player', operation === 'create',
        { meshName: 'play', request: undefined, timeoutMs: 1000 }, signal
      ]]);
      assert.equal(meshLookups, 1);
    });
  }

  for (const first of ['submit', 'yield']) {
    for (const second of ['submit', 'yield']) {
      test(`actor ${operation} rejects ${first}/${second} resubmission before effects`, async () => {
        let submissions = 0;
        let complete;
        const pendingResult = new Promise(resolve => { complete = resolve; });
        const manager = new DefaultZLinkActorManager({
          placementCreate: () => {
            submissions += 1;
            return pendingResult;
          }
        });
        const serial = new ZLinkSpotSerialTurnExecutor();
        await serial.execute(async () => {
          const call = manager[operation]('alice', 'player').inMesh('play');
          const pending = call[first]();
          try {
            assert.equal(submissions, 1);
            assert.throws(() => call[second](), isInvalidOperation);
            assert.equal(submissions, 1);
          } finally {
            complete({ status: 'rejected' });
          }
          assert.deepEqual(await pending, { status: 'rejected' });
          assert.throws(() => call[second](), isInvalidOperation);
          assert.equal(submissions, 1);
        });
      });
    }
  }
}
