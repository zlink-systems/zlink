const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function read(relativePath) {
  return fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');
}

test('DeliveryDispatch uses public Actor APIs without internal Spot handle resolvers', () => {
  for (const source of [
    'samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-worker.ts',
    'samples/DeliveryDispatch.Ts/Server/CourierSession/courier-session.ts'
  ]) {
    const content = read(source);
    assert.doesNotMatch(content, /ZLINK_SPOT_HANDLE_RESOLVER|resolveSpotHandle/);
    assert.match(content, /ZLINK_ACTOR_CLIENT|ZLINK_ACTOR_MANAGER/);
    assert.doesNotMatch(content, /requestToNode|sendToNode/);
  }

  const names = read('samples/DeliveryDispatch.Ts/Shared/Configuration/sample-names.ts');
  assert.doesNotMatch(names, /courierActorNodeRouteChannel/);
  const handlers = read('samples/DeliveryDispatch.Ts/Server/Courier/offer-delivery-handler.ts');
  assert.doesNotMatch(handlers, /ensureCourierActor|nodeRid|ActorRef/);
  assert.doesNotMatch(handlers, /joinEntrySpot/);

  for (const source of [
    'samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-center-module.ts',
    'samples/DeliveryDispatch.Ts/Server/CourierSession/courier-session-module.ts'
  ]) {
    const content = read(source);
    assert.match(content, /addRouteMesh\(SampleNames\.courierMeshName\)/);
    assert.match(content, /\.listen\(/);
  }
});
