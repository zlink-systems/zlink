import { ZLINK_MAX_ROUTING_ID_BYTES } from '../../contracts/Common/CoreTypes';
import type { ZLinkAuthorityKey } from '../../contracts/Locations/Authority';
import { decodeAuthorityKey } from './authority-key-codec';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts/Errors/ZLinkFrameworkException';

export interface RelocationReplyIdentity {
  readonly relocationId: string;
  readonly objectKey: string;
  readonly source: {
    readonly ownerId: string;
    readonly leaseGeneration: string;
    readonly nodeRid: string;
    readonly nodeGeneration: string;
  };
  readonly operationId: string;
  readonly replyRouteId: string;
}

export interface RelocationReplyWriter {
  readonly ownerId: string;
  readonly leaseGeneration: string;
}

export type RelocationReplyDelivery = 'terminalReceived' | 'alreadyTerminal' | 'sourceLeaseExpired';

export type RelocationReplyRecord = RelocationReplyIdentity & {
  readonly formatVersion: 1;
  readonly writer: RelocationReplyWriter;
} & (
    | { readonly state: 'accepted' }
    | { readonly state: 'pending' | RelocationReplyDelivery; readonly reply: string }
  );

export const RELOCATION_REPLY_PREFIX = '$zlink/relocation-reply/';

export function relocationReplyPrefix(relocationId: string): string {
  return `${RELOCATION_REPLY_PREFIX}${relocationId}/`;
}

export function relocationReplyKey(identity: RelocationReplyIdentity): string {
  const { relocationId, source, operationId } = identity;
  const { ownerId, leaseGeneration, nodeRid, nodeGeneration } = source;
  return `${relocationReplyPrefix(relocationId)}${ownerId}.${leaseGeneration}.${nodeRid}.${nodeGeneration}/${operationId}`;
}

export function relocationReplyDataLost(message: string): never {
  throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.DataLost, `DataLost: ${message}`);
}

/** The fields below are emitted in RFC 8785 property order. */
export function encodeRelocationReplyRecord(record: RelocationReplyRecord): Buffer {
  const { source, writer } = record;
  const canonical = {
    formatVersion: record.formatVersion,
    objectKey: record.objectKey,
    operationId: record.operationId,
    relocationId: record.relocationId,
    ...(record.state === 'accepted' ? {} : { reply: record.reply }),
    replyRouteId: record.replyRouteId,
    source: {
      leaseGeneration: source.leaseGeneration,
      nodeGeneration: source.nodeGeneration,
      nodeRid: source.nodeRid,
      ownerId: source.ownerId
    },
    state: record.state,
    writer: { leaseGeneration: writer.leaseGeneration, ownerId: writer.ownerId }
  };
  return Buffer.from(JSON.stringify(canonical));
}

export function decodeRelocationReplyRecord(
  bytes: Uint8Array,
  logicalKey: string
): RelocationReplyRecord {
  try {
    const value: unknown = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
    if (
      value === null ||
      typeof value !== 'object' ||
      !('formatVersion' in value) ||
      value.formatVersion !== 1
    ) {
      return relocationReplyDataLost('Invalid relocation reply row version.');
    }
    const row = value as RelocationReplyRecord;
    if (
      !/^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/.test(row.relocationId) ||
      typeof row.operationId !== 'string' ||
      !/^[0-9a-f]{32}$/.test(row.operationId) ||
      /^0+$/.test(row.operationId) ||
      !positiveUint64(row.replyRouteId) ||
      !positiveUint64(row.source.leaseGeneration) ||
      !positiveUint64(row.source.nodeGeneration) ||
      typeof row.source.nodeRid !== 'string' ||
      row.source.nodeRid.length > 2 * ZLINK_MAX_ROUTING_ID_BYTES ||
      !/^(?:[0-9a-f]{2})+$/.test(row.source.nodeRid) ||
      typeof row.objectKey !== 'string' ||
      !validText(row.source.ownerId) ||
      !validText(row.writer.ownerId) ||
      !positiveUint64(row.writer.leaseGeneration) ||
      ![
        'accepted',
        'pending',
        'terminalReceived',
        'alreadyTerminal',
        'sourceLeaseExpired'
      ].includes(row.state) ||
      (row.state !== 'accepted' &&
        (typeof row.reply !== 'string' ||
          row.reply.length === 0 ||
          Buffer.from(row.reply, 'base64').toString('base64') !== row.reply)) ||
      relocationReplyKey(row) !== logicalKey ||
      !encodeRelocationReplyRecord(row).equals(bytes)
    ) {
      return relocationReplyDataLost('Invalid relocation reply row.');
    }
    decodeAuthorityKey({ value: row.objectKey } as ZLinkAuthorityKey);
    return row;
  } catch (error) {
    if (error instanceof SyntaxError || error instanceof TypeError || error instanceof RangeError)
      return relocationReplyDataLost('Invalid relocation reply row.');
    throw error;
  }
}

function positiveUint64(value: unknown): boolean {
  return (
    typeof value === 'string' &&
    /^[1-9][0-9]{0,19}$/.test(value) &&
    BigInt(value) <= 0xffffffffffffffffn
  );
}

function validText(value: unknown): boolean {
  return (
    typeof value === 'string' &&
    value.length > 0 &&
    Buffer.from(value, 'utf8').toString('utf8') === value
  );
}
