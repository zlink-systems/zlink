import { createHash } from 'node:crypto';
import type { RoutingId, ZLinkActorJoinOperationId } from '../../contracts';
import { encodeRoutingIdStorageHex } from '../routing-id';
import { decodeServiceWireFrozenRecord } from './service-stateful-wire-codec';

const RECOVERY_PACKET_NAME = '__zlink.actor.routed_join.recovery';
const RECOVERY_CONTENT_TYPE = 'application/x-zlink-actor-routed-join-recovery-v1';
const ZLJR_MAGIC = 0x5a4c4a52;
const ZLJR_VERSION = 1;
const MAXIMUM_METADATA_BYTES = 256 * 1024;
const MAXIMUM_MESSAGE_BYTES = 1024 * 1024;
const FRAMEWORK_METADATA_UPPER_BOUND_BYTES = 64 * 1024;
const ACCEPTED_JOURNAL_UPPER_BOUND_BYTES = 16 * 1024 * 1024;
const SNAPSHOT_APPLICATION_STATE_RESERVATION_BYTES = 64 * 1024 * 1024;
const RECREATE_RELOCATION_CONTENT_TYPE = 'application/vnd.zlink.actor-relocation.recreate';
const SNAPSHOT_RELOCATION_CONTENT_TYPE = 'application/vnd.zlink.actor-relocation.snapshot';

export interface CanonicalActorJoinRecoveryRequest {
  readonly actorId: string;
  readonly actorType: string;
  readonly handoffId: string;
  readonly sourceSpotId: string;
  readonly sourceNodeRid: RoutingId;
  readonly actorGeneration: bigint;
  readonly actorAuthorityOwnerGeneration: bigint;
  readonly actorNodeGeneration: bigint;
  readonly expectedOwnerLeaseGeneration: bigint;
  readonly relocationId: string;
  readonly relocationContentType: string;
  readonly requestContentType: string;
  readonly request: Uint8Array;
  readonly targetSpotId: string;
  readonly targetNodeRid: RoutingId;
  readonly targetNodeGeneration: bigint;
  readonly targetSpotGeneration: bigint;
  readonly targetAuthorityOwnerGeneration: bigint;
  readonly targetSpotAuthorityOwnerGeneration: bigint;
  readonly coordinator: {
    readonly ownerId: string;
    readonly leaseGeneration: bigint;
    readonly nodeRid: RoutingId;
    readonly nodeGeneration: bigint;
    readonly expectedAuthorityStoreVersion: string;
  };
  readonly operationId?: ZLinkActorJoinOperationId;
  readonly replyContentType?: string;
  readonly reply?: Uint8Array;
}

export interface CanonicalActorJoinRecovery {
  readonly source: {
    readonly nodeRid: string;
    readonly nodeGeneration: bigint;
    readonly ownerId: string;
    readonly ownerLeaseGeneration: bigint;
  };
  readonly request: {
    readonly actorId: string;
    readonly actorType: string;
    readonly handoffId: string;
    readonly sourceSpotId: string;
    readonly sourceNodeRid: Buffer;
    readonly actorGeneration: bigint;
    readonly actorAuthorityOwnerGeneration: bigint;
    readonly relocationAggregateId: string;
    readonly requestContentType: string;
    readonly request: Buffer;
    readonly reservationToken: string;
    readonly reservedPayloadBytes: bigint;
  };
  readonly targetSpotId: string;
  readonly targetNodeRid: Buffer;
  readonly targetNodeGeneration: bigint;
  readonly targetSpotGeneration: bigint;
  readonly targetAuthorityOwnerGeneration: bigint;
  readonly operationId?: ZLinkActorJoinOperationId;
  readonly replyContentType?: string;
  readonly reply: Buffer;
}

export function canonicalActorJoinHandoffId(input: {
  readonly sourceActorNodeRid: Uint8Array;
  readonly actorId: string;
  readonly actorGeneration: bigint;
  readonly sourceActorNodeGeneration: bigint;
  readonly correlation: bigint;
}): string {
  const actor = textBytes(input.actorId, 'actorId');
  if (actor.byteLength > 0xffff) throw new RangeError('Actor id exceeds u16 bytes.');
  const digest = createHash('sha256').update(Buffer.concat([
    Buffer.from(input.sourceActorNodeRid),
    u16(actor.byteLength),
    actor,
    u64(input.actorGeneration),
    u64(input.sourceActorNodeGeneration),
    u64(input.correlation)
  ])).digest().subarray(0, 16);
  return Buffer.from([
    digest[3]!, digest[2]!, digest[1]!, digest[0]!,
    digest[5]!, digest[4]!, digest[7]!, digest[6]!,
    ...digest.subarray(8)
  ]).toString('hex');
}

export function encodeCanonicalActorJoinRecoverySavedWork(
  input: CanonicalActorJoinRecoveryRequest
): Buffer {
  const sourceNodeRid = routingIdBytes(input.sourceNodeRid);
  const targetNodeRid = routingIdBytes(input.targetNodeRid);
  const coordinatorNodeRid = routingIdBytes(input.coordinator.nodeRid);
  const operationId = input.operationId;
  const reply = operationId === undefined ? Buffer.alloc(0) : Buffer.from(input.reply ?? []);
  const request = Buffer.from(input.request);
  if (request.byteLength > MAXIMUM_MESSAGE_BYTES || reply.byteLength > MAXIMUM_MESSAGE_BYTES) {
    throw new RangeError('Actor Join recovery message exceeds 1 MiB.');
  }
  const replyContentType = operationId === undefined ? null : requireText(
    input.replyContentType,
    'Actor Join reply content type'
  );
  const metadata = encodeMetadataJson({
    Request: {
      ActorId: requireText(input.actorId, 'Actor id'),
      ActorType: requireText(input.actorType, 'Actor type'),
      HandoffId: requireText(input.handoffId, 'Actor Join handoff id'),
      BoundSessionNodeRid: null,
      BoundSessionRid: null,
      RelocationContentType: requireText(input.relocationContentType, 'relocation content type'),
      RelocationReference: 'pending',
      RelocationChecksumCrc32c: 0,
      RelocationAggregateId: canonicalUuid(input.relocationId),
      RelocationAggregateGeneration: 1n,
      RelocationInventoryDigest: Buffer.alloc(32).toString('base64'),
      RequestContentType: requireText(input.requestContentType, 'Actor Join request content type'),
      Request: '',
      HandoffFrames: [],
      SourceSpotId: requireText(input.sourceSpotId, 'source Spot id'),
      SourceNodeRid: sourceNodeRid.toString('base64'),
      ActorGeneration: nonZeroU64(input.actorGeneration, 'Actor generation'),
      ActorAuthorityOwnerGeneration: nonZeroU64(
        input.actorAuthorityOwnerGeneration,
        'Actor authority owner generation'
      ),
      BoundSessionBindingToken: null,
      BoundSessionBindingGeneration: 0,
      BoundSessionObjectGeneration: 0,
      BoundSessionAuthorityOwnerGeneration: 0,
      BoundSessionMeshName: null,
      BoundSessionTargetNodeGeneration: 0,
      BoundSessionOwnerLeaseGeneration: 0,
      BoundSessionOwnerNodeGeneration: 0,
      BoundSessionAcceptedHighWater: 0,
      BoundSessionSessionOwnerId: null,
      BoundSessionSessionOwnerLeaseGeneration: 0,
      ReservationToken: requireText(input.handoffId, 'Actor Join reservation token'),
      ReservedPayloadBytes: predictedRelocationPayloadBytes(
        request.byteLength,
        input.relocationContentType
      ),
      TargetNodeRid: targetNodeRid.toString('base64'),
      TargetNodeGeneration: nonZeroU64(input.targetNodeGeneration, 'target node generation'),
      TargetSpotGeneration: nonZeroU64(input.targetSpotGeneration, 'target Spot generation'),
      TargetAuthorityOwnerGeneration: nonZeroU64(
        input.targetAuthorityOwnerGeneration,
        'target authority owner generation'
      ),
      TargetSpotAuthorityOwnerGeneration: nonZeroU64(
        input.targetSpotAuthorityOwnerGeneration,
        'target Spot authority owner generation'
      ),
      RelocationCoordinatorOwnerId: requireText(
        input.coordinator.ownerId,
        'relocation coordinator owner id'
      ),
      RelocationCoordinatorLeaseGeneration: nonZeroU64(
        input.coordinator.leaseGeneration,
        'relocation coordinator lease generation'
      ),
      RelocationCoordinatorNodeRid: coordinatorNodeRid.toString('base64'),
      RelocationCoordinatorNodeGeneration: nonZeroU64(
        input.coordinator.nodeGeneration,
        'relocation coordinator node generation'
      ),
      RelocationCoordinatorExpectedAuthorityStoreVersion: requireText(
        input.coordinator.expectedAuthorityStoreVersion,
        'relocation coordinator authority Store version'
      ),
      ActorNodeGeneration: nonZeroU64(input.actorNodeGeneration, 'Actor node generation'),
      ExpectedOwnerLeaseGeneration: nonZeroU64(
        input.expectedOwnerLeaseGeneration,
        'Actor owner lease generation'
      )
    },
    TargetSpotId: requireText(input.targetSpotId, 'target Spot id'),
    TargetNodeRid: targetNodeRid.toString('base64'),
    TargetNodeGeneration: nonZeroU64(input.targetNodeGeneration, 'target node generation'),
    TargetSpotGeneration: nonZeroU64(input.targetSpotGeneration, 'target Spot generation'),
    TargetAuthorityOwnerGeneration: nonZeroU64(
      input.targetAuthorityOwnerGeneration,
      'target authority owner generation'
    ),
    OperationIdHigh: operationId?.high ?? 0n,
    OperationIdLow: operationId?.low ?? 0n,
    ReplyContentType: replyContentType,
    Reply: ''
  });
  if (metadata.byteLength > MAXIMUM_METADATA_BYTES) {
    throw new RangeError('Actor Join recovery metadata exceeds 256 KiB.');
  }
  const zlir = Buffer.concat([
    u32(ZLJR_MAGIC),
    Buffer.of(ZLJR_VERSION),
    u32(metadata.byteLength),
    u32(request.byteLength),
    u32(reply.byteLength),
    metadata,
    request,
    reply
  ]);
  const sourceBody = Buffer.concat([
    text8(encodeRoutingIdStorageHex(input.sourceNodeRid), 'source node routing id'),
    u64(nonZeroU64(input.actorNodeGeneration, 'source node generation')),
    text8(input.coordinator.ownerId, 'source owner id'),
    u64(nonZeroU64(input.coordinator.leaseGeneration, 'source owner lease generation'))
  ]);
  const payloadBody = Buffer.concat([
    text8(RECOVERY_PACKET_NAME, 'recovery packet name'),
    text8(RECOVERY_CONTENT_TYPE, 'recovery content type'),
    u32(zlir.byteLength),
    zlir
  ]);
  return Buffer.concat([
    Buffer.of(1, 1),
    u16(sourceBody.byteLength),
    sourceBody,
    Buffer.of(0),
    u64(0n), u64(0n), u32(0), u16(0),
    Buffer.of(1), u32(payloadBody.byteLength), payloadBody
  ]);
}

export function decodeCanonicalActorJoinRecoverySavedWork(
  encoded: Uint8Array
): CanonicalActorJoinRecovery | undefined {
  let frozen;
  try {
    frozen = decodeServiceWireFrozenRecord(encoded);
  } catch {
    return undefined;
  }
  const payload = frozen.applicationPayload;
  if (
    frozen.recordKind !== 1
    || frozen.sourceKind !== 1
    || payload?.packetName !== RECOVERY_PACKET_NAME
    || payload.contentType !== RECOVERY_CONTENT_TYPE
  ) {
    return undefined;
  }
  const bytes = Buffer.from(payload.bytes);
  if (bytes.byteLength < 17 || bytes.readUInt32BE(0) !== ZLJR_MAGIC || bytes[4] !== ZLJR_VERSION) {
    throw new TypeError('Canonical Actor Join recovery payload header is invalid.');
  }
  const metadataLength = bytes.readUInt32BE(5);
  const requestLength = bytes.readUInt32BE(9);
  const replyLength = bytes.readUInt32BE(13);
  if (
    metadataLength > MAXIMUM_METADATA_BYTES
    || requestLength > MAXIMUM_MESSAGE_BYTES
    || replyLength > MAXIMUM_MESSAGE_BYTES
    || 17 + metadataLength + requestLength + replyLength !== bytes.byteLength
  ) {
    throw new TypeError('Canonical Actor Join recovery payload length is invalid.');
  }
  const metadataBytes = bytes.subarray(17, 17 + metadataLength);
  const metadata = JSON.parse(quoteJsonIntegers(metadataBytes.toString('utf8'))) as Record<string, unknown>;
  const requestValue = object(metadata.Request, 'Request');
  const handoffId = text(requestValue.HandoffId, 'Request.HandoffId');
  const relocationAggregateId = canonicalUuid(
    text(requestValue.RelocationAggregateId, 'Request.RelocationAggregateId')
  );
  const reservationToken = text(requestValue.ReservationToken, 'Request.ReservationToken');
  const reservedPayloadBytes = positiveInteger(
    requestValue.ReservedPayloadBytes,
    'Request.ReservedPayloadBytes'
  );
  if (
    canonicalUuid(handoffId) !== relocationAggregateId
    || reservationToken !== handoffId
  ) {
    throw new TypeError('Canonical Actor Join recovery reservation identity changed.');
  }
  const request = Buffer.from(bytes.subarray(
    17 + metadataLength,
    17 + metadataLength + requestLength
  ));
  const reply = Buffer.from(bytes.subarray(17 + metadataLength + requestLength));
  const operationHigh = integer(metadata.OperationIdHigh, 'OperationIdHigh');
  const operationLow = integer(metadata.OperationIdLow, 'OperationIdLow');
  const hasOperation = operationHigh !== 0n || operationLow !== 0n;
  const replyContentType = nullableText(metadata.ReplyContentType, 'ReplyContentType');
  if (hasOperation !== (replyContentType !== undefined)) {
    throw new TypeError('Canonical Actor Join recovery reply identity is invalid.');
  }
  const targetNodeRid = base64(metadata.TargetNodeRid, 'TargetNodeRid');
  const requestTargetNodeRid = base64(requestValue.TargetNodeRid, 'Request.TargetNodeRid');
  if (!targetNodeRid.equals(requestTargetNodeRid)) {
    throw new TypeError('Canonical Actor Join recovery target routing id changed.');
  }
  return {
    source: {
      nodeRid: frozen.source.nodeRid,
      nodeGeneration: frozen.source.nodeGeneration,
      ownerId: frozen.source.ownerId,
      ownerLeaseGeneration: frozen.source.leaseGeneration
    },
    request: {
      actorId: text(requestValue.ActorId, 'Request.ActorId'),
      actorType: text(requestValue.ActorType, 'Request.ActorType'),
      handoffId,
      sourceSpotId: text(requestValue.SourceSpotId, 'Request.SourceSpotId'),
      sourceNodeRid: base64(requestValue.SourceNodeRid, 'Request.SourceNodeRid'),
      actorGeneration: positiveInteger(requestValue.ActorGeneration, 'Request.ActorGeneration'),
      actorAuthorityOwnerGeneration: positiveInteger(
        requestValue.ActorAuthorityOwnerGeneration,
        'Request.ActorAuthorityOwnerGeneration'
      ),
      relocationAggregateId,
      requestContentType: text(requestValue.RequestContentType, 'Request.RequestContentType'),
      request,
      reservationToken,
      reservedPayloadBytes
    },
    targetSpotId: text(metadata.TargetSpotId, 'TargetSpotId'),
    targetNodeRid,
    targetNodeGeneration: positiveInteger(metadata.TargetNodeGeneration, 'TargetNodeGeneration'),
    targetSpotGeneration: positiveInteger(metadata.TargetSpotGeneration, 'TargetSpotGeneration'),
    targetAuthorityOwnerGeneration: positiveInteger(
      metadata.TargetAuthorityOwnerGeneration,
      'TargetAuthorityOwnerGeneration'
    ),
    ...(hasOperation ? { operationId: { high: operationHigh, low: operationLow } } : {}),
    ...(replyContentType === undefined ? {} : { replyContentType }),
    reply
  };
}

export function routingIdBytes(value: RoutingId): Buffer {
  return Buffer.from(encodeRoutingIdStorageHex(value), 'hex');
}

function encodeMetadataJson(value: unknown): Buffer {
  const marker = '__zlink_u64__:';
  const encoded = JSON.stringify(value, (_key, item) => typeof item === 'bigint'
    ? `${marker}${item.toString()}`
    : item);
  return Buffer.from(encoded.replace(new RegExp(`"${marker}(\\d+)"`, 'gu'), '$1'), 'utf8');
}

function quoteJsonIntegers(value: string): string {
  let result = '';
  let quoted = false;
  let escaped = false;
  for (let index = 0; index < value.length;) {
    const character = value[index]!;
    if (quoted) {
      result += character;
      index += 1;
      if (escaped) escaped = false;
      else if (character === '\\') escaped = true;
      else if (character === '"') quoted = false;
      continue;
    }
    if (character === '"') {
      quoted = true;
      result += character;
      index += 1;
      continue;
    }
    if (character === '-' || (character >= '0' && character <= '9')) {
      let end = index + 1;
      while (end < value.length && value[end]! >= '0' && value[end]! <= '9') end += 1;
      result += `"${value.slice(index, end)}"`;
      index = end;
      continue;
    }
    result += character;
    index += 1;
  }
  return result;
}

function canonicalUuid(value: string): string {
  const hex = value.replaceAll('-', '').toLowerCase();
  if (!/^[0-9a-f]{32}$/u.test(hex) || /^0+$/u.test(hex)) {
    throw new TypeError('Actor Join relocation id is invalid.');
  }
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}`
    + `-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function predictedRelocationPayloadBytes(
  requestBytes: number,
  relocationContentType: string
): number {
  if (!Number.isSafeInteger(requestBytes) || requestBytes < 0) {
    throw new RangeError('Actor Join request size is invalid.');
  }
  if (
    relocationContentType !== RECREATE_RELOCATION_CONTENT_TYPE
    && relocationContentType !== SNAPSHOT_RELOCATION_CONTENT_TYPE
  ) {
    throw new TypeError('Actor Join relocation content type is invalid.');
  }
  return FRAMEWORK_METADATA_UPPER_BOUND_BYTES
    + ACCEPTED_JOURNAL_UPPER_BOUND_BYTES
    + requestBytes
    + (relocationContentType === SNAPSHOT_RELOCATION_CONTENT_TYPE
      ? SNAPSHOT_APPLICATION_STATE_RESERVATION_BYTES
      : 0);
}

function textBytes(value: string, name: string): Buffer {
  const result = Buffer.from(requireText(value, name), 'utf8');
  if (result.includes(0)) throw new TypeError(`${name} contains NUL.`);
  return result;
}

function text8(value: string, name: string): Buffer {
  const bytes = textBytes(value, name);
  if (bytes.byteLength > 0xff) throw new RangeError(`${name} exceeds text8.`);
  return Buffer.concat([Buffer.of(bytes.byteLength), bytes]);
}

function requireText(value: string | undefined, name: string): string {
  if (value === undefined || value.length === 0) throw new TypeError(`${name} must not be empty.`);
  return value;
}

function nonZeroU64(value: bigint, name: string): bigint {
  if (value < 1n || value > 0xffff_ffff_ffff_ffffn) throw new RangeError(`${name} is invalid.`);
  return value;
}

function u16(value: number): Buffer {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff) throw new RangeError('u16');
  const result = Buffer.alloc(2);
  result.writeUInt16BE(value);
  return result;
}

function u32(value: number): Buffer {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) throw new RangeError('u32');
  const result = Buffer.alloc(4);
  result.writeUInt32BE(value);
  return result;
}

function u64(value: bigint): Buffer {
  if (value < 0n || value > 0xffff_ffff_ffff_ffffn) throw new RangeError('u64');
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function object(value: unknown, name: string): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new TypeError(`${name} must be an object.`);
  }
  return value as Record<string, unknown>;
}

function text(value: unknown, name: string): string {
  if (typeof value !== 'string' || value.length === 0) throw new TypeError(`${name} is invalid.`);
  return value;
}

function nullableText(value: unknown, name: string): string | undefined {
  return value === null ? undefined : text(value, name);
}

function integer(value: unknown, name: string): bigint {
  if (typeof value !== 'string' || !/^\d+$/u.test(value)) throw new TypeError(`${name} is invalid.`);
  const parsed = BigInt(value);
  if (parsed > 0xffff_ffff_ffff_ffffn) throw new TypeError(`${name} exceeds u64.`);
  return parsed;
}

function positiveInteger(value: unknown, name: string): bigint {
  const parsed = integer(value, name);
  if (parsed === 0n) throw new TypeError(`${name} must not be zero.`);
  return parsed;
}

function base64(value: unknown, name: string): Buffer {
  if (typeof value !== 'string') throw new TypeError(`${name} is invalid.`);
  const decoded = Buffer.from(value, 'base64');
  if (decoded.toString('base64') !== value) throw new TypeError(`${name} is not canonical base64.`);
  return decoded;
}
