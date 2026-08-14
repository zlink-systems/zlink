// SPDX-License-Identifier: MPL-2.0

export function validateUInt64(value: bigint, name: string): bigint {
  if (typeof value !== 'bigint') {
    throw new TypeError(`${name} must be a bigint`);
  }
  const max = (1n << 64n) - 1n;
  if (value < 0n || value > max) {
    throw new RangeError(`${name} must fit in uint64`);
  }
  return value;
}

export function uint64Buffer(value: bigint, name: string): Buffer {
  validateUInt64(value, name);
  const buf = Buffer.allocUnsafe(8);
  buf.writeBigUInt64LE(value, 0);
  return buf;
}

export function readUInt64Option(buffer: Buffer, name: string): bigint {
  if (buffer.length !== 8) throw new Error(`${name} option returned an invalid payload`);
  return buffer.readBigUInt64LE(0);
}
