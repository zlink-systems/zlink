'use strict';

// Cross-language source request owner contract (fixture
// framework/runtime/protocol/fixtures/source-request-owner.json): the first accepting owner reads
// OwnerId and LeaseGeneration from the admitted peer's descriptor, from the polled descriptors
// first and the Store once otherwise, and ends the request Unavailable without a live descriptor.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {
  ZLinkRequestSourceResolver
} = require('../../packages/framework/dist/runtime/locations/request-source-resolver');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} = require('../../packages/framework/dist');
const {
  internalFrameworkWireReply
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

const LEASE_TTL_MS = 1000;
const schema = JSON.parse(
  fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/protocol/service-wire-v1.schema.json'),
    'utf8'
  )
);
const fixture = JSON.parse(
  fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/protocol/fixtures/source-request-owner.json'),
    'utf8'
  )
);

for (const scenario of fixture.cases) {
  test(`SourceRequestOwner ${scenario.name}`, async () => {
    const meshName = fixture.meshName;
    const nodeRid = fixture.peer.nodeRid;
    const nodeGeneration = BigInt(fixture.peer.nodeGeneration);
    const ownerId = fixture.descriptor.ownerId;
    const leaseGeneration = BigInt(fixture.descriptor.leaseGeneration);
    const descriptor = {
      meshName,
      rid: nodeRid,
      lifecycleGeneration: nodeGeneration,
      ownerId,
      leaseGeneration
    };
    let nowMs = 1;
    let fetchedAtMs = nowMs;
    let descriptorReads = 0;
    const resolver = new ZLinkRequestSourceResolver({
      monotonicNowMs: () => nowMs,
      listLiveMeshNodes: async () => {
        descriptorReads += 1;
        fetchedAtMs = nowMs;
        return scenario.stored ? [descriptor] : [];
      },
      remainingOwnerTokenLeaseMs: async (owner) =>
        owner.ownerId === ownerId && owner.leaseGeneration === leaseGeneration
          ? Math.max(0, LEASE_TTL_MS - (nowMs - fetchedAtMs))
          : 0
    });
    if (scenario.polled) {
      // The auto-connect poll observes the live descriptors.
      await resolver.observeLiveDescriptors(meshName, [descriptor]);
    }
    if (!scenario.leaseLive) nowMs += LEASE_TTL_MS * 2;

    const recorded = await resolver.accept(meshName, nodeRid, nodeGeneration);

    assert.equal(descriptorReads, scenario.storeDescriptorReads);
    if (scenario.result === 'accepted') {
      const expected = { ownerId, leaseGeneration, nodeRid, nodeGeneration };
      assert.deepEqual(recorded, expected);
      // The Store answer joins the polled descriptors; the next request does not read the Store.
      assert.deepEqual(resolver.accept(meshName, nodeRid, nodeGeneration), expected);
      assert.equal(descriptorReads, scenario.storeDescriptorReads);
    } else {
      assert.equal(recorded, undefined);
      // The accepting owner ends the request with the fixture's public result and wire code.
      assert.equal(
        ZLinkFrameworkErrorKind[fixture.unavailable.errorKind],
        ZLinkFrameworkErrorKind.Unavailable
      );
      assert.deepEqual(
        internalFrameworkWireReply(
          new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, 'source')
        ),
        {
          terminalResult: wireValue(fixture.unavailable.terminalResult),
          failureCode: wireValue(fixture.unavailable.failureCode)
        }
      );
    }
  });
}

function wireValue(name) {
  const found = [];
  (function walk(node) {
    if (Array.isArray(node)) node.forEach(walk);
    else if (node !== null && typeof node === 'object') {
      if (node.name === name && typeof node.value === 'number') found.push(node.value);
      Object.values(node).forEach(walk);
    }
  })(schema);
  assert.equal(new Set(found).size, 1, `wire name ${name}`);
  return found[0];
}
