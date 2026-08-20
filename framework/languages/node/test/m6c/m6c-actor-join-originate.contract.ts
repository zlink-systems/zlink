import assert from 'node:assert/strict';
import { test } from 'node:test';
import { RequestResult } from '@zlink-systems/zlink';
import type {
  RawServiceIngressRecord,
  RawServiceMeshRuntime
} from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import {
  ServiceStatefulRuntime,
  type ServiceStatefulMailboxData
} from '../../packages/framework/src/runtime/foundation/service-stateful-runtime';
import { decodeActorJoin28 } from '../../packages/framework/src/runtime/protocol/service_wire_pilot_codec.generated';
import {
  decodeFrameworkActorJoinPayload,
  encodeFrameworkActorJoinPayload,
  ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME
} from '../../packages/framework/src/runtime/messaging/actor-join-payload-codec';
import { ZLinkBufferMessage } from '../../packages/framework/src/runtime/backend/runtime-message';
import { ZLinkRemoteActorJoinReceiver } from '../../packages/framework/src/runtime/host/remote-actor-join-receiver';
import { encodeAuthorityKey } from '../../packages/framework/src/runtime/locations/authority-key-codec';
import { SERVICE_WIRE_REQUIRED_CAPABILITY } from '../../packages/framework/src/runtime/foundation/service-wire-constants.generated';
import {
  decodeStatefulHeader,
  encodeStatefulReply
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';

function applicationJobOwner() {
  const permit = {
    markApplicationQueued() {},
    detachForHandlerTurn() {},
    releaseBeforeHandler() {},
    releaseAfterInternalProcessing() {}
  };
  return {
    takeInitial: () => permit,
    close() {}
  };
}

function legacyJoinMultipart(transferId: string): Buffer {
  const json = Buffer.from(JSON.stringify({ phase: 'admission', transferId }));
  const payload = Buffer.alloc(8 + json.byteLength);
  payload.writeUInt32BE(1, 0);
  payload.writeUInt32BE(json.byteLength, 4);
  json.copy(payload, 8);
  return payload;
}

test('unbound node-to-node Actor Join originates canonical 28, admits Store state, and returns its reply', async () => {
  let targetIngress: ((record: RawServiceIngressRecord) => unknown) | undefined;
  let received: { readonly stateful?: ServiceStatefulMailboxData } | undefined;
  let completeReply: ((parts: readonly Buffer[]) => void) | undefined;
  const targetRaw = {
    topology: {
      peer: (rid: string) => rid === 'node-a'
        ? {
            descriptor: {
              lifecycleGeneration: 7n,
              protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY]
            }
          }
        : undefined
    },
    mailbox: {
      tryEnqueue(record: unknown) {
        received = record as { readonly stateful?: ServiceStatefulMailboxData };
        return true;
      }
    },
    setServiceIngress(handler: typeof targetIngress) {
      targetIngress = handler;
    },
    replyService(_ingress: unknown, parts: readonly Buffer[]) {
      completeReply?.(parts.map(part => Buffer.from(part)));
    }
  } as unknown as RawServiceMeshRuntime;
  const target = new ServiceStatefulRuntime(targetRaw, 'node-b', 11n);
  const targetSpot = target.createSpot('room-b', 'user');

  let activated = false;
  const receiver = new ZLinkRemoteActorJoinReceiver({
    authorityStore: () => ({
      async readAuthority(key: { readonly value: string }) {
        assert.equal(key.value, encodeAuthorityKey('actor', 'actor-a').value);
        return {
          kind: 'active',
          storeNow: new Date(),
          allocation: {
            state: 'active',
            objectKind: 'actor',
            stableType: 'Player',
            descriptor: { rid: 'node-a' },
            descriptorLifecycleGeneration: 7n
          },
          objectGeneration: 1n,
          authorityOwnerGeneration: 1n,
          ownerLeaseGeneration: 13n
        };
      }
    }) as never,
    actorManager: () => ({
      async getOrCreateActor() {
        activated = true;
        return {};
      },
      getState: () => ({ setNativeActorRef() {} })
    }) as never,
    spotManager: () => undefined
  });

  const sourceRaw = {
    topology: {
      peer: (rid: string) => rid === 'node-b'
        ? {
            descriptor: {
              lifecycleGeneration: 11n,
              protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY]
            }
          }
        : undefined
    },
    setServiceIngress() {},
    async requestService(_rid: string, parts: readonly Buffer[]) {
      assert.equal(
        Buffer.concat(parts).includes(Buffer.from('local-only-transfer')),
        false,
        'transfer bookkeeping must remain local to the sender'
      );
      const decoded = decodeActorJoin28(parts);
      assert.equal(decoded.actor.id, 'actor-a');
      assert.equal(decoded.actor.targetNodeGeneration, 7n);
      assert.equal(decoded.actor.expectedOwnerLeaseGeneration, 13n);
      assert.equal(decoded.targetSpot.id, 'room-b');
      assert.equal(decoded.targetSpot.targetNodeRid.toString(), 'node-b');
      assert.equal(decoded.payload?.packetName, ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME);
      assert.deepEqual(
        decodeFrameworkActorJoinPayload(decoded.payload!.payload).payload,
        Buffer.from('{"join":true}')
      );
      const reply = new Promise<readonly Buffer[]>(resolve => { completeReply = resolve; });
      await targetIngress!({
        command: 28,
        flags: 0,
        sourceRoutingId: 'node-a',
        parts,
        applicationJobOwner: applicationJobOwner() as never
      });
      const canonical = received?.stateful?.kindData;
      assert.equal(canonical?.kind, 'actorControl');
      if (canonical?.kind !== 'actorControl' || canonical.canonicalActorJoin === undefined) {
        assert.fail('target did not take the S4b canonical actorJoin path');
      }
      await receiver.prepareCanonicalActorJoin({
        actorId: 'actor-a',
        actorNodeRid: canonical.canonicalActorJoin.actorNodeRid,
        actorGeneration: canonical.canonicalActorJoin.actorGeneration,
        actorNodeGeneration: canonical.canonicalActorJoin.actorNodeGeneration,
        expectedAuthorityOwnerGeneration: canonical.canonicalActorJoin.authorityOwnerGeneration,
        expectedOwnerLeaseGeneration: canonical.canonicalActorJoin.ownerLeaseGeneration
      });
      assert.equal(activated, true, 'S4b Store admission must activate the Store-resolved type');
      assert.equal(received?.stateful?.reply?.(
        RequestResult.Ok,
        0,
        undefined,
        {
          kind: 'actorJoin',
          joinResult: 0,
          spot: targetSpot.ref,
          membershipEpoch: 1n
        }
      ), true);
      return await reply;
    }
  } as unknown as RawServiceMeshRuntime;
  const source = new ServiceStatefulRuntime(sourceRaw, 'node-a', 7n);
  const actor = source.createActor('actor-a');
  source.rememberSpotRoute({
    spot: targetSpot.ref,
    targetNodeRid: 'node-b',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: targetSpot.authorityOwnerGeneration,
    ownerLeaseGeneration: targetSpot.authorityOwnerGeneration,
    storeVersion: 'observed-room-b'
  });
  const request = ZLinkBufferMessage.from(Buffer.from('{"join":true}'));
  try {
    const pending = source.joinActorCanonical(
      actor.ref,
      'node-b',
      targetSpot.ref,
      targetSpot.ref.generation,
      {
        packetName: ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME,
        contentType: 'application/json',
        payload: encodeFrameworkActorJoinPayload(request)
      },
      {
        packetName: 'ZLinkFrameworkMultipart',
        contentType: 'application/vnd.zlink.framework-multipart',
        payload: Buffer.from('legacy-json-fallback')
      },
      {
        targetNodeGeneration: 7n,
        authorityOwnerGeneration: actor.authorityOwnerGeneration,
        ownerLeaseGeneration: 13n
      },
      { phase: 'admission', transferId: 'local-only-transfer' },
      5_000
    );
    const result = await pending.promise;
    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.equal(result.kindData?.kind, 'actorJoinCompletion');
    assert.equal(target.registry.actor('actor-a'), undefined,
      'canonical command 28 is admission-only; relocation owns target commit');
  } finally {
    request.close();
    source.close();
    target.close();
  }
});

test('canonical actorJoin candidate retains private JSON when the admitted peer lacks capability', async () => {
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 11n, protocolCapabilities: [] } })
    },
    setServiceIngress() {},
    async requestService(_rid: string, parts: readonly Buffer[]) {
      assert.equal(decodeActorJoin28(parts).payload?.packetName, 'ZLinkFrameworkMultipart');
      assert.equal(Buffer.concat(parts).includes(Buffer.from('local-only-transfer')), true);
      const header = decodeStatefulHeader(parts[0]!);
      assert.equal(header.kind, 'actorJoin');
      if (header.kind !== 'actorJoin') throw new Error('expected legacy actorJoin header');
      return [encodeStatefulReply(header.correlation, RequestResult.Ok, 0, {
        kind: 'actorJoin',
        joinResult: 1,
        spot: { spotId: 'room-b', generation: 1n }
      })];
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'node-a', 7n);
  const actor = runtime.createActor('actor-a');
  runtime.rememberSpotRoute({
    spot: { spotId: 'room-b', generation: 1n },
    targetNodeRid: 'node-b',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 1n,
    ownerLeaseGeneration: 1n,
    storeVersion: 'observed-room-b'
  });
  const pending = runtime.joinActorCanonical(
    actor.ref,
    'node-b',
    { spotId: 'room-b', generation: 1n },
    1n,
    {
      packetName: ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME,
      contentType: 'application/json',
      payload: Buffer.from('canonical-request')
    },
    {
      packetName: 'ZLinkFrameworkMultipart',
      contentType: 'application/vnd.zlink.framework-multipart',
      payload: legacyJoinMultipart('local-only-transfer')
    },
    {
      targetNodeGeneration: 7n,
      authorityOwnerGeneration: actor.authorityOwnerGeneration,
      ownerLeaseGeneration: 13n
    },
    { phase: 'admission', transferId: 'local-only-transfer' },
    5_000
  );
  try {
    const result = await pending.promise;
    assert.equal(result.terminalResult, RequestResult.Ok);
  } finally {
    runtime.close();
  }
});
