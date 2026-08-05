import assert from 'node:assert/strict';
import test from 'node:test';

import {
  ZLinkActorSessionBindingRegistry
} from '../../packages/framework/dist/runtime/streams/actor-session-binding-registry.js';

test('session route keeps its explicit authority fence across ActorRef copies', () => {
  const registry = new ZLinkActorSessionBindingRegistry();
  const context = {
    bindLocal() {},
    unbindLocal() {}
  };
  const copiedRef = {
    ...{
      actorId: 'actor-1',
      objectGeneration: 7n,
      bindingGeneration: 3n
    }
  };
  const actor = { actorId: 'actor-1', ref: copiedRef };

  registry.bind(context, actor, 'binding-token', {
    authorityOwnerGeneration: 11n,
    ownerLeaseGeneration: 13n
  });

  assert.equal(registry.seal('actor-1', 'seal-1', {
    objectGeneration: 7n,
    authorityOwnerGeneration: 11n,
    bindingGeneration: 3n,
    ownerLeaseGeneration: 13n
  }), 0n);
});
