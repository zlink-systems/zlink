// SPDX-License-Identifier: MPL-2.0

export function uint64Buffer(value: bigint, name: string): Buffer {
  if (typeof value !== 'bigint') {
    throw new TypeError(`${name} must be a bigint`);
  }
  const max = (1n << 64n) - 1n;
  if (value < 0n || value > max) {
    throw new RangeError(`${name} must fit in uint64`);
  }
  const buf = Buffer.allocUnsafe(8);
  buf.writeBigUInt64LE(value, 0);
  return buf;
}

export function readUInt64Option(buffer: Buffer, name: string): bigint {
  if (buffer.length !== 8) throw new Error(`${name} option returned an invalid payload`);
  return buffer.readBigUInt64LE(0);
}
