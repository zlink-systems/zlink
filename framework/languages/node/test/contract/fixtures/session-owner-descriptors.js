'use strict';

/**
 * Session-Actor binding §4: a test host whose MeshNode descriptor publishes an
 * owner token that differs from its RID and lifecycle generation.
 */
function testSessionOwnerToken(nodeRid, nodeGeneration) {
  return {
    ownerId: `owner-of-${String(nodeRid)}`,
    leaseGeneration: BigInt(nodeGeneration) + 100n
  };
}

/** Gives a stateful runtime the descriptor source a live host provides. */
function withTestSessionOwners(runtime) {
  runtime.setSessionOwnerDescriptors({
    local: () => testSessionOwnerToken(runtime.nodeRid, runtime.nodeGeneration),
    peer: async (peerRid, peerGeneration) => testSessionOwnerToken(peerRid, peerGeneration)
  });
  return runtime;
}

module.exports = { testSessionOwnerToken, withTestSessionOwners };
