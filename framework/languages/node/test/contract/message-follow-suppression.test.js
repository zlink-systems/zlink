'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const suppression = require(
  '../../packages/framework/dist/runtime/foundation/message-follow-suppression-registry'
);

const fixture = JSON.parse(fs.readFileSync(path.resolve(
  __dirname,
  '../../../../runtime/conformance/message-follow-suppression-v1.json'
), 'utf8'));

function fence(key) {
  const source = key.sourceRoute;
  const target = key.targetRoute;
  assert.equal(source.objectKind, target.objectKind);
  assert.equal(source.logicalObjectId, target.logicalObjectId);
  assert.equal(source.objectGeneration, target.objectGeneration);
  return {
    objectKind: source.objectKind,
    logicalObjectId: source.logicalObjectId,
    objectGeneration: source.objectGeneration,
    sourceNodeRid: source.targetNodeRid,
    sourceNodeGeneration: source.targetNodeGeneration,
    sourceAuthorityOwnerGeneration: source.authorityOwnerGeneration,
    sourceOwnerLeaseGeneration: source.ownerLeaseGeneration,
    targetNodeRid: target.targetNodeRid,
    targetNodeGeneration: target.targetNodeGeneration,
    targetAuthorityOwnerGeneration: target.authorityOwnerGeneration,
    targetOwnerLeaseGeneration: target.ownerLeaseGeneration
  };
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function setPath(target, dottedPath, value) {
  const segments = dottedPath.split('.');
  const field = segments.pop();
  let current = target;
  for (const segment of segments) current = current[segment];
  current[field] = value;
}

test('Message Follow suppression consumes the shared exact-fence fixture', () => {
  assert.equal(fixture.fixture, 'zlink.framework.message-follow-suppression');
  assert.equal(fixture.version, 1);
  assert.deepEqual(fixture.registryInvariants.states, [
    'idle',
    'inFlight',
    'sentUntilExpiry'
  ]);

  for (const mutation of fixture.independentKeyMutations) {
    const changed = clone(fixture.keys.base);
    for (const fieldPath of mutation.paths) {
      setPath(changed, fieldPath, mutation.replacement);
    }
    assert.notEqual(
      suppression.messageFollowSuppressionKey(fence(fixture.keys.base)),
      suppression.messageFollowSuppressionKey(fence(changed)),
      mutation.name
    );
  }

  for (const scenario of fixture.scenarios) {
    const registry = new suppression.MessageFollowSuppressionRegistry();
    const claims = new Map();
    for (const key of Object.values(fixture.keys)) registry.retainRoute(fence(key));

    for (const operation of scenario.operations) {
      const route = fence(fixture.keys[operation.key]);
      let result;
      switch (operation.kind) {
        case 'begin': {
          const claim = registry.begin(route);
          result = claim === undefined ? 'suppressed' : 'granted';
          if (claim !== undefined) claims.set(operation.claim, claim);
          break;
        }
        case 'markSent':
          result = registry.markSent(claims.get(operation.claim))
            ? 'applied'
            : 'ignoredStaleClaim';
          break;
        case 'abort':
          result = registry.abort(claims.get(operation.claim))
            ? 'applied'
            : 'ignoredStaleClaim';
          break;
        case 'expireRoute':
          result = registry.expireRoute(route) ? 'removed' : 'notFound';
          break;
        case 'retainRoute':
          registry.retainRoute(route);
          result = 'retained';
          break;
        case 'replaceRoute':
          registry.replaceRoute(route, fence(fixture.keys[operation.replacement]));
          result = 'replaced';
          break;
        default:
          assert.fail(`Unknown Message Follow fixture operation '${operation.kind}'.`);
      }
      assert.equal(result, operation.result, `${scenario.name}:${operation.kind}`);
      const stateKey = operation.kind === 'replaceRoute'
        ? fixture.keys[operation.replacement]
        : fixture.keys[operation.key];
      assert.equal(
        registry.state(fence(stateKey)),
        operation.state,
        `${scenario.name}:${operation.kind}:state`
      );
    }
  }
});
