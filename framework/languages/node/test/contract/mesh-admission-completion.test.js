'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkUnhandledDispatchAction } = require('../../packages/framework/dist');
const { Message, SubmitResult } = require('@zlink-systems/zlink');
const {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist');
const { ZLinkRuntimeAdmissionGate } = require('../../packages/framework/dist/runtime/admission');
const {
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');
const {
  ZLinkSpotNodeRuntimeManager
} = require('../../packages/framework/dist/runtime/spots/spot-node-runtime-manager');
const {
  ZLinkChannelRequestDispatcher,
  ZLinkRoutePacketDispatcher
} = require('../../packages/framework/dist/runtime/channels/channel-dispatchers');
const {
  encodeChannelEnvelopeParts,
  decodeChannelReply
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ReceiveKind,
  OperationKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const stream = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  channelErrorCodeName
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkRuntimeDispatchErrorAction
} = require('../../packages/framework/dist/contracts/Dispatch/ZLinkDispatchOptions');

for (const surface of ['instance', 'user', 'entry', 'actor', 'node', 'channel']) {
  for (const request of [true, false]) {
    test(`${surface} admission rejection ${request ? 'replies once' : 'does not reply to one-way send'}`, async () => {
      const mesh = 'admission-completion';
      const gate = new ZLinkRuntimeAdmissionGate();
      gate.register(mesh);
      gate.seal(mesh, ZLinkFrameworkInternalErrorKind.RuntimeShutdown);
      const admit = (work, onRejected) =>
        gate.run(mesh, 'sealed test ingress', work, undefined, onRejected);
      const errors = [];
      const reporter = {
        captureEnabled: () => true,
        flow: { flowCreationEnabled: () => false, begin: () => undefined },
        report: (error) => errors.push(error)
      };
      const handler = {
        handle() {
          assert.fail('sealed ingress ran an application handler');
        }
      };
      const kind =
        surface === 'instance'
          ? ReceiveKind.InstanceSpotActivation
          : surface === 'actor'
            ? request
              ? ReceiveKind.ActorRequest
              : ReceiveKind.ActorSend
            : surface === 'node'
              ? request
                ? ReceiveKind.NodeRequest
                : ReceiveKind.NodeSend
              : surface === 'channel'
                ? request
                  ? ReceiveKind.ChannelRequest
                  : ReceiveKind.ChannelSend
                : request
                  ? ReceiveKind.SpotRequest
                  : ReceiveKind.SpotSend;
      const parts = (
        surface === 'actor'
          ? [
              stream.encodeStreamHeader({
                kind: request
                  ? stream.ZLinkStreamMessageKind.Request
                  : stream.ZLinkStreamMessageKind.Send,
                codec: stream.ZLinkStreamCodec.Json,
                flags: stream.ZLinkStreamHeaderFlags.None,
                requestSeq: 7n,
                name: 'Ping',
                metadata: new Map()
              })
            ]
          : encodeChannelEnvelopeParts(
              request ? 1 : 3, // ZLinkChannelMessageKind is a TypeScript const enum.
              mesh,
              'Ping',
              { value: 1 }
            )
      ).map((part) => Message.from(part));
      let replies = 0;
      let replyParts;
      const record = {
        kind,
        operationKind: request ? OperationKind.InstanceSpotRequest : undefined,
        sourceNodeRid: 'source',
        parts,
        reply(parts) {
          replies++;
          replyParts = parts.map((part) => Message.from(part));
          return SubmitResult.Ok;
        }
      };
      const owner = {
        spotId: 'target',
        actor: { actorId: 'actor', nodeRid: 'node', generation: 1n }
      };
      let dispatch;
      if (surface === 'node' || surface === 'channel') {
        const dispatcher =
          surface === 'channel'
            ? new ZLinkChannelRequestDispatcher({
                meshName: mesh,
                channelName: mesh,
                dispatchErrors: reporter,
                handlers: new Map([['Ping', handler]]),
                sendHandlers: new Map([['Ping', handler]])
              })
            : new ZLinkRoutePacketDispatcher({
                routerChannelId: mesh,
                dispatchErrors: reporter,
                handlers: [{ kind: request ? 'request' : 'send', packetName: 'Ping', handler }]
              });
        dispatch = () => dispatcher.dispatchMesh(record, undefined, admit);
      } else if (surface === 'entry') {
        const manager = Object.create(ZLinkSpotNodeRuntimeManager.prototype);
        manager.entryActivations = new Map([
          [mesh, { spotId: 'target', dispatchPacketEncoded: handler.handle }]
        ]);
        manager.options = {
          registration: { messageSerializers: new Map() },
          dispatchErrors: reporter
        };
        dispatch = () => manager.dispatchEntrySpotRecord(mesh, owner, record, admit);
      } else {
        const manager = new DefaultZLinkSpotManager({
          detachedTaskRunner: {},
          dispatchErrors: reporter
        });
        dispatch = () =>
          surface === 'actor'
            ? manager.dispatchMeshActor('remote', mesh, owner, record, admit)
            : surface === 'instance'
              ? manager.dispatchMeshInstance(mesh, owner, record, admit)
              : manager.dispatchMeshSpot(mesh, owner, record, admit);
      }
      try {
        if (!request && (surface === 'user' || surface === 'entry' || surface === 'actor'))
          await assert.rejects(
            dispatch(),
            (error) =>
              error instanceof ZLinkFrameworkException &&
              error.kind === ZLinkFrameworkErrorKind.ShuttingDown
          );
        else await dispatch();
        assert.equal(replies, request ? 1 : 0);
        assert.equal(gate.pending(mesh), 0);
        if (request && surface === 'actor') {
          const frame = stream.tryDecodeStreamFrame(replyParts[0].data());
          assert.equal(
            stream.decodeStreamHeader(frame.header).kind,
            stream.ZLinkStreamMessageKind.Error
          );
          assert.equal(
            JSON.parse(Buffer.from(frame.payload).toString()).code,
            channelErrorCodeName(ZLinkFrameworkErrorKind.ShuttingDown)
          );
        } else if (request)
          assert.throws(
            () => decodeChannelReply(replyParts),
            (error) =>
              error instanceof ZLinkFrameworkException &&
              error.kind === ZLinkFrameworkErrorKind.ShuttingDown
          );
        if (!request && surface !== 'user' && surface !== 'entry' && surface !== 'actor') {
          assert.equal(errors.length, 1);
          assert.equal(errors[0].action, ZLinkRuntimeDispatchErrorAction.Drop);
        }
      } finally {
        for (const part of parts) part.close();
        for (const part of replyParts ?? []) part.close();
      }
    });
  }
}

for (const request of [true, false]) {
  test(`admitted ${request ? 'request' : 'send'} preserves the configured missing-handler Throw policy`, async () => {
    const gate = new ZLinkRuntimeAdmissionGate();
    gate.register('policy');
    const dispatcher = new ZLinkChannelRequestDispatcher({
      channelName: 'policy',
      handlers: new Map(),
      sendHandlers: new Map(),
      dispatchErrors: {
        flow: { flowCreationEnabled: () => false, begin: () => undefined },
        report() {}
      },
      unhandled: {
        request: ZLinkUnhandledDispatchAction.Throw,
        send: ZLinkUnhandledDispatchAction.Throw,
        publish: ZLinkUnhandledDispatchAction.Throw
      }
    });
    const parts = encodeChannelEnvelopeParts(request ? 1 : 3, 'policy', 'Missing', {}).map((part) =>
      Message.from(part)
    );
    let replies = 0;
    try {
      await assert.rejects(async () =>
        dispatcher.dispatchMesh(
          {
            kind: request ? ReceiveKind.ChannelRequest : ReceiveKind.ChannelSend,
            parts,
            sourceNodeRid: 'source',
            reply() {
              replies++;
              return SubmitResult.Ok;
            }
          },
          undefined,
          (work, onRejected) =>
            gate.run('policy', 'policy test ingress', work, undefined, onRejected)
        )
      );
      assert.equal(replies, 0, 'Throw policy must not be converted to a reply');
      assert.equal(gate.pending('policy'), 0);
    } finally {
      for (const part of parts) part.close();
    }
  });
}

test('Spot Actor relay preserves the typed host admission rejection', async () => {
  const { ZLinkFrameworkRuntimeHost } = require('../../packages/framework/dist/runtime/host');
  const {
    ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET
  } = require('../../packages/framework/dist/runtime/actors/actor-packet-relay-wire');
  const host = Object.create(ZLinkFrameworkRuntimeHost.prototype);
  host.options = { registration: { messageSerializers: new Map() } };
  host.flowCreationEnabled = () => false;
  host.boundSessionRelay = {
    actorPackets: {
      receiveRemoteActorPacketRelay() {
        assert.fail('sealed ingress ran the Actor relay');
      }
    }
  };
  const gate = new ZLinkRuntimeAdmissionGate();
  gate.register('relay');
  gate.seal('relay', ZLinkFrameworkInternalErrorKind.RuntimeShutdown);
  const parts = encodeChannelEnvelopeParts(
    1,
    'relay',
    ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
    {}
  ).map((part) => Message.from(part));
  let replies = 0;
  let replyParts;
  try {
    await host.dispatchMeshSpotRecord(
      'relay',
      { spotId: 'target' },
      {
        kind: ReceiveKind.SpotRequest,
        parts,
        sourceNodeRid: 'source',
        reply(parts) {
          replies++;
          replyParts = parts.map((part) => Message.from(part));
          return SubmitResult.Ok;
        }
      },
      (work) => gate.run('relay', 'sealed relay ingress', work)
    );
    assert.equal(replies, 1);
    assert.throws(
      () => decodeChannelReply(replyParts),
      (error) =>
        error instanceof ZLinkFrameworkException &&
        error.kind === ZLinkFrameworkErrorKind.ShuttingDown
    );
  } finally {
    for (const part of parts) part.close();
    for (const part of replyParts ?? []) part.close();
  }
});
