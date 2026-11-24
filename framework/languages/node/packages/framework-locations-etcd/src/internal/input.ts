import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import type { ZLinkStoreKey, ZLinkStoreScanRequest } from '@zlink-systems/framework';

const requireFramework = createRequire(__filename);
export const bounds = requireFramework(
  join(dirname(requireFramework.resolve('@zlink-systems/framework')), 'internal')
) as typeof import('../../../framework/src/internal');

export function text(value: string, minimum: number, maximum: number): string {
  if (typeof value !== 'string') throw new TypeError('Store input must be a string');
  const size = Buffer.byteLength(value);
  if (size < minimum || size > maximum) throw new RangeError('Store input exceeds its UTF-8 bound');
  return value;
}
export function key(value: ZLinkStoreKey): Buffer {
  return Buffer.from(text(value.value, 1, bounds.ZLINK_PROVIDER_MAX_KEY_BYTES));
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
