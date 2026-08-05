export const defaultMaxDecompressedPayloadSize = 64 * 1024;

export function lz4PickleUncompressed(payload: Uint8Array): Uint8Array {
  if (payload.length === 0) {
    return new Uint8Array();
  }
  const pickled = new Uint8Array(payload.length + 1);
  pickled[0] = 0;
  pickled.set(payload, 1);
  return pickled;
}

export function lz4UnpicklePayload(
  payload: Uint8Array,
  maxDecompressedSize = defaultMaxDecompressedPayloadSize
): Uint8Array {
  if (payload.length === 0) {
    return new Uint8Array();
  }
  const header = payload[0];
  if ((header & 0x07) !== 0) {
    throw new Error('Unexpected LZ4 pickle version.');
  }
  const sizeOfDiff = decodeDiffSize((header >>> 6) & 0x03);
  const dataOffset = 1 + sizeOfDiff;
  if (payload.length < dataOffset) {
    throw new Error('LZ4 pickle header is incomplete.');
  }
  const data = payload.subarray(dataOffset);
  const resultDiff = sizeOfDiff === 0 ? 0 : readLittleEndian(payload, 1, sizeOfDiff);
  const resultLength = data.length + resultDiff;
  if (resultLength > maxDecompressedSize) {
    throw new Error('LZ4 decoded payload exceeds maximum stream payload size.');
  }
  if (resultDiff === 0) {
    return data.slice();
  }
  return decodeLz4Block(data, resultLength);
}

function decodeDiffSize(encoded: number): number {
  return encoded === 3 ? 4 : encoded;
}

function readLittleEndian(source: Uint8Array, offset: number, size: number): number {
  if (size === 1) {
    return source[offset];
  }
  if (size === 2) {
    return source[offset] | (source[offset + 1] << 8);
  }
  if (size === 4) {
    return (
      source[offset]
      | (source[offset + 1] << 8)
      | (source[offset + 2] << 16)
      | (source[offset + 3] * 0x1000000)
    );
  }
  throw new Error(`Unexpected LZ4 pickle field size: ${size}`);
}

function decodeLz4Block(source: Uint8Array, resultLength: number): Uint8Array {
  const target = new Uint8Array(resultLength);
  let sourceOffset = 0;
  let targetOffset = 0;

  while (sourceOffset < source.length) {
    const token = source[sourceOffset++];
    const literalLength = readLz4Length(source, token >>> 4, () => sourceOffset++);
    if (source.length - sourceOffset < literalLength) {
      throw new Error('LZ4 literal run is incomplete.');
    }
    if (target.length - targetOffset < literalLength) {
      throw new Error('LZ4 literal run exceeds output size.');
    }
    target.set(source.subarray(sourceOffset, sourceOffset + literalLength), targetOffset);
    sourceOffset += literalLength;
    targetOffset += literalLength;

    if (sourceOffset >= source.length) {
      break;
    }
    if (source.length - sourceOffset < 2) {
      throw new Error('LZ4 match offset is incomplete.');
    }
    const matchOffset = source[sourceOffset] | (source[sourceOffset + 1] << 8);
    sourceOffset += 2;
    if (matchOffset === 0 || matchOffset > targetOffset) {
      throw new Error('LZ4 match offset is invalid.');
    }
    const matchLength = readLz4Length(source, token & 0x0f, () => sourceOffset++) + 4;
    if (target.length - targetOffset < matchLength) {
      throw new Error('LZ4 match run exceeds output size.');
    }
    for (let index = 0; index < matchLength; index += 1) {
      target[targetOffset + index] = target[targetOffset - matchOffset + index];
    }
    targetOffset += matchLength;
  }

  if (targetOffset !== resultLength) {
    throw new Error('LZ4 decoded length does not match pickle header.');
  }
  return target;
}

function readLz4Length(source: Uint8Array, nibble: number, nextOffset: () => number): number {
  let length = nibble;
  if (length !== 15) {
    return length;
  }
  for (;;) {
    const offset = nextOffset();
    if (offset >= source.length) {
      throw new Error('LZ4 extended length is incomplete.');
    }
    const value = source[offset];
    length += value;
    if (value !== 255) {
      return length;
    }
  }
}
