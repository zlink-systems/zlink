import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ZLinkFrameworkRuntimeHost } from '../../packages/framework/src/runtime/host';
import { ZLinkRuntimeAdmissionGate } from '../../packages/framework/src/runtime/admission';
import { ZLinkBufferMessage } from '../../packages/framework/src/runtime/backend/runtime-message';
import { SubmitResult } from '../../packages/framework/src/runtime/backend/runtime-values';
import {
  ReadyDomain,
  ReceiveKind,
  type ReadyRecord,
  type ReceiveRecord
} from '../../packages/framework/src/runtime/foundation/service-runtime-contracts';
import {
  encodeChannelEnvelopeParts,
  ZLinkChannelMessageKind
} from '../../packages/framework/src/runtime/channels/channel-envelope';
import {
  encodeRemoteActorPacketRelayPayload,
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
  ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET
} from '../../packages/framework/src/runtime/actors/actor-packet-relay-wire';
import {
  encodeStreamHeader,
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind
} from '../../packages/framework/src/runtime/streams/protocol';
import { ZLinkFrameworkErrorKind } from '../../packages/framework/src/contracts';

function relayRecord(name: string, kind: number = ReceiveKind.NodeRequest): ReceiveRecord {
  const payload = encodeRemoteActorPacketRelayPayload({
    actorId: 'actor-1',
    header: encodeStreamHeader({
      kind: ZLinkStreamMessageKind.Send,
      codec: ZLinkStreamCodec.Raw,
      flags: ZLinkStreamHeaderFlags.None,
      name,
      metadata: new Map()
    }),
    payload: Buffer.alloc(0)
  });
  const parts = encodeChannelEnvelopeParts(
    ZLinkChannelMessageKind.Request,
    'mesh',
    ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
    payload,
    undefined,
    undefined,
    undefined,
    undefined,
    false
  ).map((part) => ZLinkBufferMessage.from(part));
  return {
    kind,
    domain: ReadyDomain.Application,
    sourceNodeRid: null,
    sourceSpotId: null,
    sourceBindingGeneration: 0n,
    sourceActor: null,
    operationId: { high: 1n, low: 1n },
    operationKind: 1,
    channelName: null,
    topic: null,
    applicationMetadata: null,
    kindData: null,
    terminalResult: 0,
    failureErrno: 0,
    parts,
    reply: () => SubmitResult.Ok,
    replyActorJoin: () => SubmitResult.Ok
  };
}

function hostFixture() {
  const admission = new ZLinkRuntimeAdmissionGate();
  admission.register('mesh');
  let delivered = 0;
  const fixture = Object.assign(Object.create(ZLinkFrameworkRuntimeHost.prototype), {
    admission,
    options: { registration: { messageSerializers: new Map() } },
    flowCreationEnabled: () => false,
    serviceRelocation: { tryHandleControl: () => false },
    channelRuntime: {
      dispatchMeshRoute: () => {
        delivered += 1;
      }
    },
    boundSessionRelay: {
      actorPackets: {
        receiveRemoteActorPacketRelay: async () => {
          delivered += 1;
          return { ok: true };
        }
      }
    }
  }) as {
    dispatchMeshRecord(
      origin: () => 'remote',
      mesh: string,
      owner: ReadyRecord,
      record: ReceiveRecord
    ): void | Promise<void>;
  };
  return {
    admission,
    delivered: () => delivered,
    dispatch: (record: ReceiveRecord) =>
      fixture.dispatchMeshRecord(
        () => 'remote',
        'mesh',
        { ownerKind: 1, domain: record.domain, spotId: null, actor: null },
        record
      )
  };
}

for (const kind of [ReceiveKind.NodeRequest, ReceiveKind.SpotRequest]) {
  test(`shutdown keeps disconnect relay progressing for receive kind ${kind}`, async () => {
    const host = hostFixture();
    host.admission.close();
    const record = relayRecord(ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET, kind);
    try {
      await host.dispatch(record);
      assert.equal(host.delivered(), 1);
      assert.equal(host.admission.pending('mesh'), 0);
    } finally {
      for (const part of record.parts) part.close();
    }
  });

  test(`shutdown still refuses ordinary Actor relay for receive kind ${kind}`, async () => {
    const host = hostFixture();
    const record = relayRecord('application-message', kind);
    try {
      await host.dispatch(record);
      assert.equal(host.delivered(), 1);
      host.admission.close();
      await assert.rejects(async () => host.dispatch(record), {
        kind: ZLinkFrameworkErrorKind.ShuttingDown
      });
      assert.equal(host.delivered(), 1);
      assert.equal(host.admission.pending('mesh'), 0);
    } finally {
      for (const part of record.parts) part.close();
    }
  });
}

test('shutdown keeps an infrastructure Node record progressing', async () => {
  const host = hostFixture();
  host.admission.close();
  const record = { ...relayRecord('unused'), domain: ReadyDomain.Infrastructure };
  try {
    await host.dispatch(record);
    assert.equal(host.delivered(), 1);
    assert.equal(host.admission.pending('mesh'), 0);
  } finally {
    for (const part of record.parts) part.close();
  }
});
