import {
  type ProtobufEncodeContext,
  type ProtobufEnvelopeCodecOptions
} from '@zlink-systems/framework-codec-protobuf';
import type { ZlinkStreamEncodedPayload } from '@zlink-systems/stream-connector';
import { ZlinkStreamCodec } from '@zlink-systems/stream-wire';
import {
  BingoGeneratedMessageConstructors,
  BingoGeneratedProtobufCodec,
  type BingoPayloadEnvelope
} from './bingo-messages.generated';
import { PacketNames } from './messages';

const messageTypesByPacketName: Readonly<Record<string, string>> = {
  [PacketNames.authenticateReq]: 'AuthenticateReq',
  [PacketNames.authenticatePlayerReq]: 'AuthenticatePlayerReq',
  [PacketNames.ensurePlayerActorReq]: 'EnsurePlayerActorReq',
  [PacketNames.matchBingoReq]: 'MatchBingoReq',
  [PacketNames.matchBingoApiReq]: 'MatchBingoApiReq',
  [PacketNames.bingoRoomJoinReq]: 'BingoRoomJoinReq',
  [PacketNames.submitBingoCardReq]: 'SubmitBingoCardReq',
  [PacketNames.observeBingoEventsReq]: 'ObserveBingoEventsReq',
  [PacketNames.stopObservingBingoEventsReq]: 'StopObservingBingoEventsReq',
  [PacketNames.bingoRewardAcquiredEvent]: 'BingoRewardAcquiredEvent',
  [PacketNames.playerJoinedNotify]: 'PlayerJoinedNotify',
  [PacketNames.gameStartedNotify]: 'BingoGameStartedNotify',
  [PacketNames.numberDrawnNotify]: 'BingoNumberDrawnNotify',
  [PacketNames.gameEndedNotify]: 'BingoGameEndedNotify',
  [PacketNames.rewardAnnouncedNotify]: 'BingoRewardAnnouncedNotify'
};

const responseTypesByRequestPacketName: Readonly<Record<string, string>> = {
  [PacketNames.authenticateReq]: 'AuthenticateRes',
  [PacketNames.authenticatePlayerReq]: 'AuthenticatePlayerRes',
  [PacketNames.ensurePlayerActorReq]: 'EnsurePlayerActorRes',
  [PacketNames.matchBingoReq]: 'MatchBingoRes',
  [PacketNames.matchBingoApiReq]: 'MatchBingoApiRes',
  [PacketNames.bingoRoomJoinReq]: 'BingoRoomJoinRes',
  [PacketNames.submitBingoCardReq]: 'SubmitBingoCardRes',
  [PacketNames.observeBingoEventsReq]: 'ObserveBingoEventsRes',
  [PacketNames.stopObservingBingoEventsReq]: 'StopObservingBingoEventsRes'
};

const bingoProtobufOptions: ProtobufEnvelopeCodecOptions = {
  encode(payload: unknown, rawContext?: ProtobufEncodeContext | Function): ZlinkStreamEncodedPayload {
    const context = normalizeContext(rawContext);
    const type = resolveType(payload, context);
    const message = BingoGeneratedProtobufCodec.encode(type, payload);
    return {
      codec: ZlinkStreamCodec.Protobuf,
      payload: BingoGeneratedProtobufCodec.encode('BingoPayloadEnvelope', { type, payload: message }),
      messageType: context.messageType ?? inferMessageType(payload)
    };
  },
  decode<TPayload = unknown>(payload: ZlinkStreamEncodedPayload): TPayload {
    if (payload.codec !== ZlinkStreamCodec.Protobuf) {
      throw new Error(`Stream payload codec is ${payload.codec}, not Protobuf.`);
    }
    const envelope = BingoGeneratedProtobufCodec.decode(
      'BingoPayloadEnvelope',
      payload.payload
    ) as BingoPayloadEnvelope;
    return BingoGeneratedProtobufCodec.decode(envelope.type, envelope.payload) as TPayload;
  }
};

function normalizeContext(value: ProtobufEncodeContext | Function | undefined): ProtobufEncodeContext {
  return typeof value === 'function'
    ? { direction: 'Send', messageType: value }
    : value ?? { direction: 'Send' };
}

function resolveType(payload: unknown, context: ProtobufEncodeContext): string {
  const responseType = context.packetName === undefined
    ? undefined
    : responseTypesByRequestPacketName[context.packetName];
  if (context.direction === 'Response' && responseType !== undefined) return responseType;
  const packetType = context.packetName === undefined
    ? undefined
    : messageTypesByPacketName[context.packetName];
  if (packetType !== undefined) return packetType;
  const constructor = context.messageType ?? inferMessageType(payload);
  if (
    constructor?.name !== undefined
    && BingoGeneratedMessageConstructors[constructor.name as keyof typeof BingoGeneratedMessageConstructors] === constructor
  ) {
    return constructor.name;
  }
  throw new Error('Cannot infer generated Bingo Protobuf message type.');
}

function inferMessageType(value: unknown): Function | undefined {
  if (value === null || value === undefined) return undefined;
  const constructor = (value as { constructor?: Function }).constructor;
  return constructor === Object ? undefined : constructor;
}

export { bingoProtobufOptions };
