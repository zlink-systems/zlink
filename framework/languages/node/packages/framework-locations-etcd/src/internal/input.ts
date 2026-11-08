import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import type {
  ZLinkStoreKey,
  ZLinkStoreWriteRequest,
  ZLinkStoreScanRequest
} from '@zlink-systems/framework';
import { MAX_TIME_MS } from './encoding';

const requireFramework = createRequire(__filename);
export const bounds = requireFramework(
  join(dirname(requireFramework.resolve('@zlink-systems/framework')), 'internal')
) as Record<
  | 'ZLINK_PROVIDER_MAX_KEY_BYTES'
  | 'ZLINK_PROVIDER_MAX_VALUE_BYTES'
  | 'ZLINK_PROVIDER_MAX_VERSION_BYTES'
  | 'ZLINK_PROVIDER_MAX_WRITE_KEYS'
  | 'ZLINK_PROVIDER_MAX_WRITE_BYTES'
  | 'ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES'
  | 'ZLINK_PROVIDER_MAX_PAGE_SIZE'
  | 'ZLINK_PROVIDER_MAX_ENCODED_PAGE_BYTES',
  number
>;

export function text(value: string, minimum: number, maximum: number): string {
  if (typeof value !== 'string') throw new TypeError('Store input must be a string');
  const size = Buffer.byteLength(value);
  if (size < minimum || size > maximum) throw new RangeError('Store input exceeds its UTF-8 bound');
  return value;
}
export function key(value: ZLinkStoreKey): Buffer {
  return Buffer.from(text(value.value, 1, bounds.ZLINK_PROVIDER_MAX_KEY_BYTES));
}
function bytes(value: Uint8Array): void {
  if (!(value instanceof Uint8Array)) throw new TypeError('Store value must be bytes');
  if (value.byteLength > bounds.ZLINK_PROVIDER_MAX_VALUE_BYTES)
    throw new RangeError('Store value exceeds its bound');
}
export function validateWrite(request: ZLinkStoreWriteRequest): void {
  const conditionKeys = new Set<string>();
  const mutationKeys = new Set<string>();
  let size = 0;
  for (const condition of request.conditions) {
    if (!['missing', 'version', 'value'].includes(condition.kind))
      throw new TypeError('Invalid Store condition');
    size += key(condition.key).length;
    if (conditionKeys.has(condition.key.value)) throw new RangeError('Duplicate condition key');
    conditionKeys.add(condition.key.value);
    if (condition.kind === 'value') {
      bytes(condition.expected);
      size += condition.expected.byteLength;
    } else if (condition.kind === 'version')
      size += Buffer.byteLength(
        text(condition.expected.value, 1, bounds.ZLINK_PROVIDER_MAX_VERSION_BYTES)
      );
  }
  for (const mutation of request.mutations) {
    if (!['put', 'delete'].includes(mutation.kind)) throw new TypeError('Invalid Store mutation');
    size += key(mutation.key).length;
    if (mutationKeys.has(mutation.key.value)) throw new RangeError('Duplicate mutation key');
    mutationKeys.add(mutation.key.value);
    if (mutation.kind === 'put') {
      bytes(mutation.bytes);
      size += mutation.bytes.byteLength;
      const retention = mutation.retentionMs;
      if (
        retention !== undefined &&
        (!Number.isFinite(retention) ||
          retention <= 0 ||
          !Number.isSafeInteger(Math.ceil(retention)) ||
          Math.ceil(retention) > Number(MAX_TIME_MS))
      ) {
        throw new RangeError('Invalid Store retention');
      }
    }
  }
  if (
    new Set([...conditionKeys, ...mutationKeys]).size > bounds.ZLINK_PROVIDER_MAX_WRITE_KEYS ||
    size > bounds.ZLINK_PROVIDER_MAX_WRITE_BYTES
  )
    throw new RangeError('Store batch exceeds its bound');
}
export function validateScan(request: ZLinkStoreScanRequest): void {
  text(request.prefix, 0, bounds.ZLINK_PROVIDER_MAX_KEY_BYTES);
  if (
    !Number.isSafeInteger(request.limit) ||
    request.limit < 1 ||
    request.limit > bounds.ZLINK_PROVIDER_MAX_PAGE_SIZE
  ) {
    throw new RangeError('Invalid Store page limit');
  }
  if (request.cursor !== undefined)
    text(request.cursor.value, 1, bounds.ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES);
}
