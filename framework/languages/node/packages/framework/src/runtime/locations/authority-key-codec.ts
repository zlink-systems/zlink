import type {
  ZLinkAuthorityKey,
  ZLinkPlacementObjectKind
} from '../../contracts/Locations/Authority';

export function encodeAuthorityKey(
  kind: ZLinkPlacementObjectKind,
  globalId: string
): ZLinkAuthorityKey {
  const bytes = Buffer.from(globalId);
  if (bytes.byteLength < 1 || bytes.byteLength > 255) {
    throw new RangeError('Authority identity must contain 1..255 UTF-8 bytes.');
  }
  const discriminator = kind === 'actor' ? 'a' : 's';
  return {
    value: `zla1:${discriminator}:${bytes.byteLength}:${percentEncode(bytes)}`
  } as ZLinkAuthorityKey;
}

export function decodeAuthorityKey(
  key: ZLinkAuthorityKey
): { readonly kind: ZLinkPlacementObjectKind; readonly globalId: string } {
  const match = /^zla1:([as]):([0-9]+):(.*)$/.exec(key.value);
  if (match === null) throw new TypeError('Authority key is not canonical.');
  const encoded = match[3]!;
  const bytes: number[] = [];
  for (let index = 0; index < encoded.length;) {
    if (encoded[index] === '%') {
      const hex = encoded.slice(index + 1, index + 3);
      if (!/^[0-9A-F]{2}$/.test(hex)) throw new TypeError('Authority key escape is invalid.');
      bytes.push(Number.parseInt(hex, 16));
      index += 3;
    } else {
      bytes.push(encoded.charCodeAt(index));
      index++;
    }
  }
  if (bytes.length !== Number(match[2])) {
    throw new TypeError('Authority key length does not match its identity.');
  }
  return {
    kind: match[1] === 'a' ? 'actor' : 'user_spot',
    globalId: Buffer.from(bytes).toString('utf8')
  };
}

function percentEncode(bytes: Uint8Array): string {
  let result = '';
  for (const byte of bytes) {
    result += isUnreserved(byte)
      ? String.fromCharCode(byte)
      : `%${byte.toString(16).toUpperCase().padStart(2, '0')}`;
  }
  return result;
}

function isUnreserved(byte: number): boolean {
  return byte >= 0x41 && byte <= 0x5a
    || byte >= 0x61 && byte <= 0x7a
    || byte >= 0x30 && byte <= 0x39
    || byte === 0x2d
    || byte === 0x2e
    || byte === 0x5f
    || byte === 0x7e;
}
