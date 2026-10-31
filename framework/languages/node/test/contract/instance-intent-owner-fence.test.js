const assert = require('node:assert/strict');
const test = require('node:test');
const { RequestResult } = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  RequestRuntime
} = require('../../packages/framework/dist/runtime/foundation/request-runtime');
const {
  ZLinkRuntimeRouteTransport
} = require('../../packages/framework/dist/runtime/channels/channel-transports');
const protocol = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

const fixture = require('../../../../runtime/conformance/spot-close-v1.json');
for (const scenario of fixture.authorityFollowCases) {
  test(`Instance authority follow keeps one entry and deadline: ${scenario.name}`, async () => {
    const requests = new RequestRuntime();
    let reads = 0,
      invalidations = 0,
      activations = 0,
      attempts = 0;
    let originalEntry, originalDeadline, replyParts;
    const route = {
      routerChannelId: 'mesh',
      targetNodeRid: 'old-owner',
      spotId: 'room',
      spotKind: framework.ZLinkSpotKind.Instance,
      stableType: 'Room',
      targetSpotGeneration: 3n,
      targetNodeGeneration: 2n,
      authorityOwnerGeneration: 4n,
      targetOwnerId: 'owner',
      ownerLeaseGeneration: 5n,
      authorityStoreVersion: 'v1'
    };
    const node = {
      requestInstanceSpot(_route, parts) {
        assert.equal(_route.targetNodeRid, reads === 1 ? 'old-owner' : 'new-owner');
        replyParts = protocol
          .encodeChannelReplyParts(
            protocol.decodeChannelEnvelope(parts.map(ZLinkBufferMessage.from)).header,
            'new-incarnation'
          )
          .map(ZLinkBufferMessage.from);
        return 2n;
      },
      instanceSpotPlacementTypes: () => ['Room'],
      selectObjectPlacement: () => ({
        kind: 'selected',
        target: {
          targetNodeRid: 'new-owner',
          targetNodeGeneration: 3n,
          descriptorVersion: '1'
        }
      }),
      requestToMissingInstanceSpot(_target, parts) {
        activations++;
        replyParts = protocol
          .encodeChannelReplyParts(
            protocol.decodeChannelEnvelope(parts.map(ZLinkBufferMessage.from)).header,
            'new-incarnation'
          )
          .map(ZLinkBufferMessage.from);
        return { high: 9n, low: 1n };
      }
    };
    const ownerCompletions = {
      submit(submit, _signal, entry, receiver) {
        originalEntry ??= entry;
        originalDeadline ??= entry.deadlineMs;
        assert.equal(entry, originalEntry);
        assert.equal(entry.deadlineMs, originalDeadline);
        assert.equal(entry.isPending(), true);
        assert.equal(requests.registry.size, 1);
        if (attempts > 0) assert.equal(reads, attempts + 1);
        submit();
        attempts++;
        const refused = scenario.refusals < 0 || attempts <= scenario.refusals;
        receiver.operationCompleted(entry.id, {
          terminalResult: refused
            ? scenario.failureCode === 18
              ? RequestResult.Rejected
              : RequestResult.Conflict
            : RequestResult.Ok,
          failureErrno: refused ? (scenario.failureCode ?? 34) : 0,
          parts: refused ? [] : replyParts
        });
      }
    };
    const coldCompletions = {
      async submit(submit, _signal, entry) {
        assert.equal(entry, originalEntry);
        assert.equal(entry.deadlineMs, originalDeadline);
        assert.equal(entry.isPending(), true);
        assert.equal(requests.registry.size, 1);
        submit();
        return {
          terminalResult: RequestResult.Ok,
          failureErrno: 0,
          parts: replyParts
        };
      }
    };
    const routed = new ZLinkRuntimeRouteTransport(
      () => undefined,
      undefined,
      () => ({ meshNode: () => node, meshCompletionTable: () => ownerCompletions })
    );
    const creatingResolver = new framework.ZLinkAuthoritySpotRouteResolver(
      {
        async readAuthority() {
          return {
            kind: 'snapshot',
            storeVersion: { value: 'pending-attempt' },
            payload: framework.encodeServiceInstanceAuthorityPayload({
              state: 'coldActivating',
              stableType: 'Room',
              spotId: 'room',
              ownerId: 'owner',
              ownerLeaseGeneration: 5n,
              ownerMeshName: 'mesh',
              ownerNodeRid: 'new-owner',
              ownerNodeGeneration: 3n
            }),
            objectGeneration: 3n,
            authorityOwnerGeneration: 4n,
            ownerId: 'owner',
            ownerLeaseGeneration: 5n,
            allocation: { state: 'reserved', objectKind: 'instance_spot', stableType: 'Room' },
            pendingCreation: { requestContentReference: 'same-attempt' },
            storeNow: new Date()
          };
        }
      },
      (mesh) => mesh
    );
    const address = new framework.ZLinkHostSpotAddressTransport({
      requests,
      resolver: () => ({
        async resolve() {
          reads++;
          if (reads === 1) return route;
          if (scenario.authority === 'Ready') return { ...route, targetNodeRid: 'new-owner' };
          if (scenario.authority === 'Creating')
            return creatingResolver.resolve('room', undefined, true);
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
            'released'
          );
        },
        invalidate() {
          invalidations++;
        }
      }),
      routed,
      meshNames: () => ['mesh'],
      meshNode: () => node,
      completions: () => coldCompletions,
      defaultRequestTimeoutMs: scenario.refusals < 0 ? 20 : 1000
    });
    class Ping {}
    try {
      const reply = address.requestToSpotAddress('room', new Ping(), {
        instanceSpot: scenario.intent,
        instanceSpotType: 'Room'
      });
      if (scenario.terminal === 'reply') assert.equal(await reply, 'new-incarnation');
      else
        await assert.rejects(
          reply,
          (error) => error.kind === framework.ZLinkFrameworkErrorKind[scenario.terminal]
        );
      if (scenario.refusals >= 0) {
        assert.equal(reads, scenario.terminal === 'reply' ? scenario.refusals + 1 : 1);
        assert.equal(
          invalidations,
          scenario.failureCode === 18 ? 0 : scenario.intent ? scenario.refusals : 1
        );
      } else assert.ok(reads > 2);
      assert.equal(
        activations,
        scenario.authority === 'Missing' || scenario.authority === 'Creating' ? 1 : 0
      );
      assert.equal(requests.registry.size, 0);
    } finally {
      requests.close();
    }
  });
}
