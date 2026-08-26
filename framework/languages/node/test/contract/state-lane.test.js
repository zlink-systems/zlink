const assert = require('node:assert/strict');
const test = require('node:test');

const { ZLinkStateLane } = require('../../packages/framework/dist/internal');

/*
 * The .NET golden tests ConcurrentCallers_MutateUnsynchronizedStateWithoutLosingUpdates and
 * WorkItems_NeverOverlap are intentionally not ported. They establish multi-threaded mutation
 * and atomic overlap facts that do not exist on Node's single event loop. The remaining tests
 * cover the Node-relevant guarantee: a mailbox turn holds ownership across await boundaries, so
 * later work cannot interleave with it and recursive entry is diagnosed at its call site.
 */

test('state lane returns the result of work', async () => {
  const lane = new ZLinkStateLane();

  assert.equal(await lane.run(() => 42), 42);
});

test('state lane surfaces a work failure to its caller', async () => {
  const lane = new ZLinkStateLane();

  await assert.rejects(
    lane.run(() => { throw new Error('boom'); }),
    /boom/u
  );
});

test('state lane keeps serving after a work item throws', async () => {
  const lane = new ZLinkStateLane();

  await assert.rejects(lane.run(() => { throw new Error('boom'); }), /boom/u);
  assert.equal(await lane.run(() => 7), 7);
});

test('posts from one caller run in post order', async () => {
  const lane = new ZLinkStateLane();
  const order = [];

  for (let index = 0; index < 100; index++) {
    const value = index;
    assert.equal(lane.tryPost(async () => {
      await Promise.resolve();
      order.push(value);
    }), true);
  }

  assert.deepEqual(await lane.run(() => [...order]), Array.from({ length: 100 }, (_, index) => index));
});

test('draining more than one golden batch still runs every item', async () => {
  const lane = new ZLinkStateLane();
  let count = 0;

  for (let index = 0; index < 250; index++) {
    assert.equal(lane.tryPost(() => { count++; }), true);
  }

  assert.equal(await lane.run(() => count), 250);
});

test('reentering the same lane after an await fails instead of hanging', async () => {
  const lane = new ZLinkStateLane();

  await lane.run(async () => {
    await Promise.resolve();
    assert.throws(() => lane.run(() => 1), /already runs on the state lane/u);
  });
});

test('isOnLane is true only inside a turn', async () => {
  const lane = new ZLinkStateLane();

  assert.equal(lane.isOnLane, false);
  assert.equal(await lane.run(async () => {
    await Promise.resolve();
    return lane.isOnLane;
  }), true);
  assert.equal(lane.isOnLane, false);
});

test('a different lane is enterable from inside a turn', async () => {
  const outer = new ZLinkStateLane();
  const inner = new ZLinkStateLane();

  assert.equal(await outer.run(async () => await inner.run(() => 5)), 5);
});

test('dispose waits for queued work', async () => {
  const lane = new ZLinkStateLane();
  let completed = 0;

  for (let index = 0; index < 200; index++) {
    lane.tryPost(() => { completed++; });
  }

  await lane.dispose();
  assert.equal(completed, 200);
});

test('run after dispose throws', async () => {
  const lane = new ZLinkStateLane();
  await lane.dispose();

  assert.throws(() => lane.run(() => 1), /closed/u);
});

test('tryPost after dispose reports refusal instead of throwing', async () => {
  const lane = new ZLinkStateLane();
  await lane.dispose();

  assert.equal(lane.tryPost(() => undefined), false);
});

test('dispose is idempotent', async () => {
  const lane = new ZLinkStateLane();

  await lane.dispose();
  await lane.dispose();
});
