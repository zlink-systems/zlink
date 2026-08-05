import type { ZLinkSerialWorkOptions } from './serial-scheduler';

/**
 * Returns the application metadata byte count used by the execution budget.
 * Metadata is counted as the UTF-8 bytes of its key and value strings; the
 * scheduler adds the fixed envelope and queue-node cost separately.
 */
export function zlinkMetadataByteLength(
  metadata: ReadonlyMap<string, string> | Readonly<Record<string, string>> | undefined
): number {
  if (metadata === undefined) return 0;
  let bytes = 0;
  const entries = metadata instanceof Map
    ? metadata.entries()
    : Object.entries(metadata);
  for (const [key, value] of entries) {
    bytes += Buffer.byteLength(key, 'utf8') + Buffer.byteLength(value, 'utf8');
  }
  return bytes;
}

export function zlinkSerialWorkOptions(
  payloadBytes: number,
  metadataBytes: number
): ZLinkSerialWorkOptions {
  return { payloadBytes, metadataBytes };
}
