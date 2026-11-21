import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts/Errors/ZLinkFrameworkException';

export type RelocationCompletionDelivery =
  'pending' | 'terminalReceived' | 'alreadyTerminal' | 'sourceLeaseExpired';
export interface RelocationCompletionEntry {
  readonly objectKey: string;
  readonly source: {
    readonly ownerId: string;
    readonly leaseGeneration: string;
    readonly nodeRid: string;
    readonly nodeGeneration: string;
  };
  readonly operationId: string;
  readonly replyRouteId: string;
  readonly reply: string;
  readonly delivery: RelocationCompletionDelivery;
}
export interface RelocationCompletionRecord {
  readonly formatVersion: '1';
  readonly relocationId: string;
  readonly entries: readonly RelocationCompletionEntry[];
}

export function completionDataLost(message: string): never {
  throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.DataLost, `DataLost: ${message}`);
}

/** All scalar values in §9.3 are strings, so recursive key sorting implements JCS. */
export function encodeRelocationCompletionRecord(record: RelocationCompletionRecord): Buffer {
  validate(record);
  return Buffer.from(JSON.stringify(canonical(record)), 'utf8');
}
export function decodeRelocationCompletionRecord(bytes: Uint8Array): RelocationCompletionRecord {
  try {
    const value = JSON.parse(
      new TextDecoder('utf-8', { fatal: true }).decode(bytes)
    ) as RelocationCompletionRecord;
    const encoded = encodeRelocationCompletionRecord(value);
    if (!encoded.equals(Buffer.from(bytes)))
      completionDataLost('Completion record is not canonical JSON.');
    return value;
  } catch (error) {
    if (error instanceof ZLinkFrameworkException) throw error;
    if (error instanceof SyntaxError || error instanceof TypeError || error instanceof RangeError)
      return completionDataLost('Invalid completion record.');
    throw error;
  }
}
function canonical(value: unknown): unknown {
  if (typeof value === 'string' && Buffer.from(value, 'utf8').toString('utf8') !== value)
    completionDataLost('Completion text is not valid Unicode.');
  if (Array.isArray(value)) return value.map(canonical);
  if (value !== null && typeof value === 'object') {
    const object = value as Record<string, unknown>;
    return Object.fromEntries(
      Object.keys(object)
        .sort()
        .map((key) => [key, canonical(object[key])])
    );
  }
  return value;
}
function fields(value: unknown, names: string[]): asserts value is Record<string, unknown> {
  if (
    value === null ||
    typeof value !== 'object' ||
    Array.isArray(value) ||
    Object.keys(value).sort().join(',') !== names.sort().join(',')
  )
    completionDataLost('Invalid completion fields.');
}
function decimal(value: unknown): void {
  if (
    typeof value !== 'string' ||
    !/^[1-9][0-9]*$/.test(value) ||
    BigInt(value) > 0xffffffffffffffffn
  )
    completionDataLost('Invalid completion uint64.');
}
function validate(record: RelocationCompletionRecord): void {
  fields(record, ['formatVersion', 'relocationId', 'entries']);
  if (
    String(record.formatVersion) !== '1' ||
    typeof record.relocationId !== 'string' ||
    !/^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/.test(record.relocationId) ||
    !Array.isArray(record.entries)
  )
    completionDataLost('Invalid completion version or relocation identity.');
  const seen = new Set<string>();
  let previous = Buffer.alloc(0);
  for (const entry of record.entries) {
    fields(entry, ['objectKey', 'source', 'operationId', 'replyRouteId', 'reply', 'delivery']);
    fields(entry.source, ['ownerId', 'leaseGeneration', 'nodeRid', 'nodeGeneration']);
    if (typeof entry.objectKey !== 'string' || !/^zla1:[aus]:.+$/.test(entry.objectKey))
      completionDataLost('Invalid authority key.');
    const key = Buffer.from(entry.objectKey, 'utf8');
    if (Buffer.compare(previous, key) > 0)
      completionDataLost('Completion object order differs from authority order.');
    previous = key;
    if (
      typeof entry.operationId !== 'string' ||
      !/^[0-9a-f]{32}$/.test(entry.operationId) ||
      /^0+$/.test(entry.operationId)
    )
      completionDataLost('Invalid completion operation identity.');
    decimal(entry.replyRouteId);
    decimal(entry.source.leaseGeneration);
    decimal(entry.source.nodeGeneration);
    if (
      typeof entry.source.ownerId !== 'string' ||
      entry.source.ownerId.length === 0 ||
      typeof entry.source.nodeRid !== 'string' ||
      !/^(?:[0-9a-f]{2})+$/.test(entry.source.nodeRid)
    )
      completionDataLost('Invalid requester fence.');
    if (
      typeof entry.reply !== 'string' ||
      Buffer.from(entry.reply, 'base64').toString('base64') !== entry.reply
    )
      completionDataLost('Invalid reply base64.');
    if (
      typeof entry.delivery !== 'string' ||
      !['pending', 'terminalReceived', 'alreadyTerminal', 'sourceLeaseExpired'].includes(
        entry.delivery
      )
    )
      completionDataLost('Invalid delivery.');
    const identity = JSON.stringify([
      record.relocationId,
      entry.source.ownerId,
      entry.source.leaseGeneration,
      entry.source.nodeRid,
      entry.source.nodeGeneration,
      entry.operationId
    ]);
    if (seen.has(identity)) completionDataLost('Duplicate completed operation.');
    seen.add(identity);
  }
}
