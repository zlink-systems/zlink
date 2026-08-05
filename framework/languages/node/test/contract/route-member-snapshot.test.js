const assert = require('node:assert/strict');
const test = require('node:test');

const { ZLinkRouteMemberSnapshot } = require('../../packages/framework/dist/runtime/channels/route-member-snapshot');

test('route member snapshot distinguishes unknown, missing, connected, and disconnected targets', () => {
  const snapshot = new ZLinkRouteMemberSnapshot();

  assert.equal(snapshot.status('profile.route', 'api-a'), 'unknown');
  snapshot.observeReady('profile.route', 'api-a', 'tcp://127.0.0.1:4001');
  assert.equal(snapshot.status('profile.route', 'api-a'), 'connected');
  assert.equal(snapshot.status('profile.route', 'api-missing'), 'missing');

  snapshot.observeTermination('profile.route', undefined, 'tcp://127.0.0.1:4001');
  assert.equal(snapshot.status('profile.route', 'api-a'), 'disconnected');
  assert.equal(snapshot.status('profile.route', 'api-missing'), 'missing');
});

test('route member snapshot does not guess a target when multiple members terminate without identity', () => {
  const snapshot = new ZLinkRouteMemberSnapshot();
  snapshot.observeReady('profile.route', 'api-a', 'tcp://127.0.0.1:4001');
  snapshot.observeReady('profile.route', 'api-b', 'tcp://127.0.0.1:4002');

  assert.equal(snapshot.observeTermination('profile.route', undefined, ''), undefined);
  assert.equal(snapshot.status('profile.route', 'api-a'), 'connected');
  assert.equal(snapshot.status('profile.route', 'api-b'), 'connected');
});
