import { UINT64_MAX, ZlinkStreamContentType } from '@zlink-systems/stream-wire';
import {
  actorMessageFollowPayloadChecksum,
  messageFollowOwnerFenceKey,
  ownerFence
} from './actor-message-follow-context';
import type { ZLinkActorHandoffPacket } from './actor-handoff';
import { ZLinkBufferMessage } from '../backend/runtime-message';
import { decodeMultipartParts, encodeMultipartPayload } from '../foundation/service-wire-m6a-codec';
import {
  decodeServiceWireFrozenRecord,
  encodeServiceWireFrozenActorApplicationRecord,
  type ServiceWireFrozenRecord,
  type ServiceWireRequestSourceFence
} from '../foundation/service-stateful-wire-codec';
import {
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
  SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME
} from '../foundation/service-wire-constants.generated';
import type {
  ServiceRelocationParticipant,
  ServiceRelocationQueuedMessage
} from '../foundation/service-relocation-runtime';
import { decodeAuthorityKey } from '../locations/authority-key-codec';
import type { ZLinkAuthorityKey } from '../locations/internal-location-contracts';
import { decodeRoutingId, encodeRoutingIdStorageHex, routingIdsEqual } from '../routing-id';
import { decodeHandoffBacklog } from '../spots/spot-remote-codec';
import {
  decodeActorRequestDeadlineUnixMs,
  decodeStreamHeader,
  ZLinkStreamMessageKind
} from '../streams/protocol';

export function decodeQueuedHandoffPacket(
  message: ServiceRelocationQueuedMessage,
  targetOwnerId?: string,
  actorId?: string
): ZLinkActorHandoffPacket {
  const payload = Buffer.from(message.payload);
  // The in-memory capture is a JSON object. Durable records start with a
  // binary kind discriminator and must pass the frozen-record decoder.
  const frozen = payload[0] === 0x7b ? undefined : decodeServiceWireFrozenRecord(payload);
  if (frozen !== undefined && !isLegacyActorAcceptedRecord(frozen)) {
    return decodeFrozenActorHandoffPacket(frozen, message.sequence, targetOwnerId, actorId);
  }
  const value = JSON.parse(
    Buffer.from(frozen?.applicationPayload?.bytes ?? payload).toString('utf8')
  ) as Record<string, unknown>;
  const index = value.index;
  if (!Number.isSafeInteger(index) || (index as number) < 0) {
    throw new TypeError('Relocation Actor packet index is invalid.');
  }
  const messageFollowOrigin = decodeQueuedMessageFollowOrigin(value.messageFollowOrigin);
  const decoded = decodeHandoffBacklog([
    {
      ...value,
      index: 0,
      ...(messageFollowOrigin === undefined ? {} : { messageFollowOrigin })
    }
  ])[0]!;
  const requestSource =
    decoded.source === undefined ? undefined : handoffSourceFence(decoded.source);
  if (
    messageFollowOrigin !== undefined &&
    requestSource !== undefined &&
    routingIdsEqual(messageFollowOrigin.sourceNodeRid, requestSource.nodeRid)
  ) {
    return {
      ...decoded,
      index: index as number,
      messageFollowOrigin: {
        ...messageFollowOrigin,
        requestSource
      }
    };
  }
  return { ...decoded, index: index as number };
}

export function isLegacyActorAcceptedRecord(record: ServiceWireFrozenRecord): boolean {
  const application = record.applicationPayload;
  return (
    application?.packetName === '__zlink.actor.handoff.accepted' &&
    application.contentType === ZlinkStreamContentType.Json
  );
}

function decodeFrozenActorHandoffPacket(
  record: ServiceWireFrozenRecord,
  sequence: bigint,
  targetOwnerId?: string,
  actorId?: string
): ZLinkActorHandoffPacket {
  const { applicationPayload, target, replyRouteId } = record;
  if (
    target?.kind !== 'actor' ||
    (actorId !== undefined && actorId !== target.actorId) ||
    applicationPayload?.packetName !== SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME ||
    applicationPayload.contentType !== SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE ||
    sequence < 1n ||
    sequence > BigInt(Number.MAX_SAFE_INTEGER)
  ) {
    throw new TypeError('Relocation Actor frozen record is invalid.');
  }
  const parts = decodeMultipartParts(applicationPayload.bytes, (part) => part);
  const [header, body] = parts;
  const request = replyRouteId !== undefined;
  if (
    parts.length !== 2 ||
    decodeStreamHeader(header).kind !==
      (request ? ZLinkStreamMessageKind.Request : ZLinkStreamMessageKind.Send)
  ) {
    throw new TypeError('Relocation Actor frozen record has invalid STREAM parts.');
  }
  const operationId =
    record.operationId.high.toString(16).padStart(16, '0') +
    record.operationId.low.toString(16).padStart(16, '0');
  const owner = ownerFence({
    ownerId: targetOwnerId ?? record.source.ownerId,
    ownerLeaseGeneration: target.ownerLeaseGeneration,
    nodeRid: String(target.targetNodeRid),
    nodeRidHex: encodeRoutingIdStorageHex(target.targetNodeRid),
    nodeGeneration: target.targetNodeGeneration,
    authorityOwnerGeneration: target.authorityOwnerGeneration
  });
  const source = request
    ? {
        ownerId: record.source.ownerId,
        ownerLeaseGeneration: record.source.leaseGeneration.toString(),
        nodeRid: String(record.source.nodeRid),
        nodeRidHex: encodeRoutingIdStorageHex(record.source.nodeRid),
        nodeGeneration: record.source.nodeGeneration.toString(),
        replyRouteId: replyRouteId.toString()
      }
    : undefined;
  return {
    index: Number(sequence - 1n),
    header: header.toString('base64'),
    payload: body.toString('base64'),
    returnResponse: request,
    ...(source === undefined
      ? {}
      : {
          source,
          messageFollowOrigin: {
            sourceNodeRid: record.source.nodeRid,
            originalOperation: record.operationId,
            originalReplyRouteId: replyRouteId!,
            requestSource: record.source
          }
        }),
    messageFollowContext: {
      operationId,
      objectGeneration: target.generation.toString(),
      sourceOwner: owner,
      targetOwner: owner,
      deadlineUnixMs: request ? decodeActorRequestDeadlineUnixMs(header) : undefined,
      correlationId: request ? operationId : undefined,
      replyRouteId: replyRouteId?.toString(16).padStart(32, '0'),
      request,
      hopCount: 0,
      visitedOwners: [messageFollowOwnerFenceKey(owner)],
      payloadChecksumSha256: actorMessageFollowPayloadChecksum(
        parts.map((part) => ZLinkBufferMessage.fromOwned(part))
      )
    }
  };
}

export function handoffSourceFence(
  value: NonNullable<ZLinkActorHandoffPacket['source']>
): ServiceWireRequestSourceFence {
  return {
    ownerId: value.ownerId,
    leaseGeneration: BigInt(value.ownerLeaseGeneration),
    nodeRid: decodeRoutingId(value.nodeRid, value.nodeRidHex),
    nodeGeneration: BigInt(value.nodeGeneration)
  };
}

export function encodeActorAcceptedRecord(
  participant: Pick<ServiceRelocationParticipant, 'key' | 'objectKind' | 'objectGeneration'>,
  packet: ZLinkActorHandoffPacket,
  fallbackSource: () => ServiceWireRequestSourceFence
) {
  if (participant.objectKind !== 'actor') {
    throw new Error('Only Actor object-mailbox participants can contain accepted packets.');
  }
  const identity = decodeAuthorityKey({ value: participant.key } as ZLinkAuthorityKey).globalId;
  const source =
    packet.source === undefined
      ? (packet.messageFollowOrigin?.requestSource ?? fallbackSource())
      : handoffSourceFence(packet.source);
  const target = packet.messageFollowContext.targetOwner;
  const operationId = parseHandoffOperationId(packet.messageFollowContext.operationId);
  if (packet.returnResponse && packet.source === undefined) {
    throw new Error('Captured Actor request has no exact request-source fence.');
  }
  if (
    !packet.returnResponse &&
    packet.source === undefined &&
    packet.remoteBoundSessionTarget !== undefined
  ) {
    // A durable frozen record is permitted only for a lease-backed source.
    // Work tied to a Session connection's lifetime drains before Captured,
    // so freezing it under the coordinator's fence is a protocol error.
    throw new Error('Captured connection-bound Actor send cannot enter a relocation envelope.');
  }
  return encodeServiceWireFrozenActorApplicationRecord({
    source,
    target: {
      actorId: identity,
      objectGeneration: participant.objectGeneration,
      nodeRid: decodeRoutingId(target.nodeRid, target.nodeRidHex),
      nodeGeneration: BigInt(target.nodeGeneration),
      authorityOwnerGeneration: BigInt(target.authorityOwnerGeneration),
      ownerLeaseGeneration: BigInt(target.ownerLeaseGeneration)
    },
    operationId,
    ...(packet.returnResponse ? { replyRouteId: BigInt(packet.source!.replyRouteId) } : {}),
    payload: {
      packetName: SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
      contentType: SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
      bytes: encodeMultipartPayload([
        Buffer.from(packet.header, 'base64'),
        Buffer.from(packet.payload, 'base64')
      ])
    }
  });
}

export function parseHandoffOperationId(value: string): {
  readonly high: bigint;
  readonly low: bigint;
} {
  if (!/^[0-9a-f]{32}$/.test(value)) {
    throw new Error('Captured Actor operation ID is invalid.');
  }
  const operation = {
    high: BigInt(`0x${value.slice(0, 16)}`),
    low: BigInt(`0x${value.slice(16)}`)
  };
  if (operation.high === 0n && operation.low === 0n) {
    throw new Error('Captured Actor operation ID must not be zero.');
  }
  return operation;
}

export function encodeHandoffQueuedMessages(
  backlog: readonly ZLinkActorHandoffPacket[]
): readonly ServiceRelocationQueuedMessage[] {
  return backlog.map((packet) => ({
    sequence: BigInt(packet.index) + 1n,
    payload: Buffer.from(
      JSON.stringify({
        ...packet,
        ...(packet.messageFollowOrigin === undefined
          ? {}
          : {
              messageFollowOrigin: {
                sourceNodeRid: String(packet.messageFollowOrigin.sourceNodeRid),
                sourceNodeRidHex: encodeRoutingIdStorageHex(
                  packet.messageFollowOrigin.sourceNodeRid
                ),
                originalOperation: {
                  high: packet.messageFollowOrigin.originalOperation.high.toString(),
                  low: packet.messageFollowOrigin.originalOperation.low.toString()
                },
                originalReplyRouteId: packet.messageFollowOrigin.originalReplyRouteId.toString()
              }
            })
      }),
      'utf8'
    )
  }));
}

function decodeQueuedMessageFollowOrigin(value: unknown) {
  if (value === undefined) return undefined;
  if (typeof value !== 'object' || value === null) {
    throw new TypeError('Relocation Actor Message Follow origin is invalid.');
  }
  const origin = value as Record<string, unknown>;
  const operation = origin.originalOperation;
  if (
    typeof origin.sourceNodeRid !== 'string' ||
    origin.sourceNodeRid.length === 0 ||
    typeof operation !== 'object' ||
    operation === null
  ) {
    throw new TypeError('Relocation Actor Message Follow origin is invalid.');
  }
  const operationRecord = operation as Record<string, unknown>;
  const high = queuedMessageU64(operationRecord.high, 'originalOperation.high');
  const low = queuedMessageU64(operationRecord.low, 'originalOperation.low');
  if (high === 0n && low === 0n) {
    throw new TypeError('Relocation Actor Message Follow operation must not be zero.');
  }
  return {
    sourceNodeRid: decodeRoutingId(origin.sourceNodeRid, origin.sourceNodeRidHex),
    originalOperation: { high, low },
    originalReplyRouteId: queuedMessageU64(origin.originalReplyRouteId, 'originalReplyRouteId')
  };
}

function queuedMessageU64(value: unknown, field: string): bigint {
  if (typeof value !== 'string' || !/^(?:0|[1-9][0-9]*)$/.test(value)) {
    throw new TypeError(`Relocation Actor Message Follow ${field} is invalid.`);
  }
  const result = BigInt(value);
  if (result > UINT64_MAX) {
    throw new TypeError(`Relocation Actor Message Follow ${field} is outside u64.`);
  }
  return result;
}
