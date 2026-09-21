import { randomBytes } from 'node:crypto';

export function randomOperationId(entropy: Uint8Array = randomBytes(16)): {
  readonly high: bigint;
  readonly low: bigint;
} {
  if (entropy.byteLength !== 16) throw new RangeError('Operation ID entropy must be 16 bytes.');
  const bytes = Buffer.from(entropy);
  const high = bytes.readBigUInt64BE(0);
  const low = bytes.readBigUInt64BE(8);
  return high === 0n && low === 0n ? { high, low: 1n } : { high, low };
}
