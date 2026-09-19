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

// #662: contract README §7.2 discards a decision that no longer matches a live
// offer ("이전 A의 늦은 결정은 현재 Attempt=2와 일치하지 않으므로 버린다.") and the .NET
// reference (CourierDecisionActorHandler.cs) logs a warning and returns instead of
// throwing. Node's courier actor must do the same for a decision on an offer it has
// no record of, rather than throwing.
test('DeliveryDispatch courier decide() logs and ignores a decision for an unknown offer instead of throwing', () => {
  const content = read('samples/DeliveryDispatch.Ts/Server/Courier/courier-actor.ts');
  const decideMatch = content.match(/async decide\(decision: CourierDecisionMsg\): Promise<void> \{[\s\S]*?\n  \}/);
  assert.ok(decideMatch, 'decide() method not found in courier-actor.ts');
  const decideBody = decideMatch[0];
  assert.match(decideBody, /attempt === undefined/);
  assert.doesNotMatch(
    decideBody,
    /throw new Error/,
    'decide() must not throw for an unknown offer; the contract discards a stale/unrecognized decision (README §7.2) the way the .NET reference does (log + return).'
  );
  assert.match(
    decideBody,
    /console\.(log|error|warn)\(\s*`deliverydispatch-courier[^`]*unknown-offer[^`]*`\s*\)/,
    'decide() must log the discarded decision with the deliverydispatch-courier convention.'
  );
});
