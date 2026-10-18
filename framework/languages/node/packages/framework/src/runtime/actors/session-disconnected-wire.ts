import type { RoutingId } from '../../contracts';
import { ZLINK_MAX_ROUTING_ID_BYTES } from '../../contracts/Common/CoreTypes';
import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '../../contracts';
import { decodeRoutingId, encodeRoutingIdStorageHex } from '../routing-id';

const RID_LENGTH_BYTES = 1;
const GENERATION_BYTES = 8;
const GENERATIONS_BYTES = GENERATION_BYTES * 2;

export interface SessionDisconnectedIdentity {
  readonly sessionOwnerNodeRid: RoutingId;
  readonly sessionOwnerNodeGeneration: bigint;
  readonly bindingGeneration: bigint;
}

export function encodeSessionDisconnected(
  sessionOwnerNodeRid: RoutingId | undefined,
  sessionOwnerNodeGeneration: bigint | undefined,
  bindingGeneration: bigint | undefined
): Buffer {
  if (
    sessionOwnerNodeRid === undefined ||
    sessionOwnerNodeGeneration === undefined ||
    sessionOwnerNodeGeneration === 0n ||
    bindingGeneration === undefined ||
    bindingGeneration === 0n
  ) {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'Session disconnect requires an exact binding identity.'
    );
  }
  const rid = Buffer.from(encodeRoutingIdStorageHex(sessionOwnerNodeRid), 'hex');
  if (rid.length === 0 || rid.length > ZLINK_MAX_ROUTING_ID_BYTES) {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'Invalid Session owner Node RID length.'
    );
  }
  const payload = Buffer.allocUnsafe(RID_LENGTH_BYTES + rid.length + GENERATIONS_BYTES);
  payload[0] = rid.length;
  rid.copy(payload, RID_LENGTH_BYTES);
  payload.writeBigUInt64BE(sessionOwnerNodeGeneration, RID_LENGTH_BYTES + rid.length);
  payload.writeBigUInt64BE(bindingGeneration, RID_LENGTH_BYTES + rid.length + GENERATION_BYTES);
  return payload;
}

export function decodeSessionDisconnected(
  bytes: Uint8Array
): SessionDisconnectedIdentity | undefined {
  const payload = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (payload.length < RID_LENGTH_BYTES + 1 + GENERATIONS_BYTES) return undefined;
  const ridLength = payload[0];
  if (ridLength === 0 || payload.length !== RID_LENGTH_BYTES + ridLength + GENERATIONS_BYTES)
    return undefined;
  const offset = RID_LENGTH_BYTES + ridLength;
  const sessionOwnerNodeGeneration = payload.readBigUInt64BE(offset);
  const bindingGeneration = payload.readBigUInt64BE(offset + GENERATION_BYTES);
  if (sessionOwnerNodeGeneration === 0n || bindingGeneration === 0n) return undefined;
  const hex = payload.subarray(RID_LENGTH_BYTES, offset).toString('hex');
  return {
    sessionOwnerNodeRid: decodeRoutingId(hex, hex),
    sessionOwnerNodeGeneration,
    bindingGeneration
  };
}
