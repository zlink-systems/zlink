import type { ZLinkSerialWorkOptions } from './serial-scheduler';

/** Returns UTF-8 metadata bytes for the owning serial reservation. */
export function zlinkMetadataByteLength(
  metadata: ReadonlyMap<string, string> | Readonly<Record<string, string>> | undefined
): number {
  if (metadata === undefined) return 0;
  let bytes = 0;
  if (metadata instanceof Map) {
    for (const [key, value] of metadata) {
      bytes += Buffer.byteLength(key, 'utf8') + Buffer.byteLength(value, 'utf8');
    }
    return bytes;
  }
  //  Iterate keys directly; Object.entries would allocate a pairs array per
  //  message on the dispatch hot path.
  const record = metadata as Readonly<Record<string, string>>;
  for (const key in record) {
    if (Object.prototype.hasOwnProperty.call(record, key)) {
      bytes += Buffer.byteLength(key, 'utf8') + Buffer.byteLength(record[key], 'utf8');
    }
  }
  return bytes;
}

export function zlinkSerialWorkOptions(
  payloadBytes: number,
  metadataBytes: number
): ZLinkSerialWorkOptions {
  return { payloadBytes, metadataBytes };
}
