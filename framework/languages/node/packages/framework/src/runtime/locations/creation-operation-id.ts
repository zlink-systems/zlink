import { UINT64_BYTES } from '@zlink-systems/stream-wire';
import { OPERATION_IDENTITY_BYTES } from '../foundation/operation-identity';
import { randomBytes } from 'node:crypto';

export function randomOperationId(entropy: Uint8Array = randomBytes(OPERATION_IDENTITY_BYTES)): {
  readonly high: bigint;
  readonly low: bigint;
} {
  if (entropy.byteLength !== OPERATION_IDENTITY_BYTES)
    throw new RangeError(`Operation ID entropy must be ${OPERATION_IDENTITY_BYTES} bytes.`);
  const bytes = Buffer.from(entropy);
  const high = bytes.readBigUInt64BE(0);
  const low = bytes.readBigUInt64BE(UINT64_BYTES);
  return high === 0n && low === 0n ? { high, low: 1n } : { high, low };
}
