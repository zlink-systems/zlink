import { UINT64_BYTES, UINT64_MAX } from '@zlink-systems/stream-wire';
export const OPERATION_IDENTITY_BYTES = 16;

import { randomBytes } from 'node:crypto';

export const ZLINK_NATIVE_CORRELATION_OPERATION_NAMESPACE = 2n;

export interface ZLinkOperationIdentity128 {
  readonly high: bigint;
  readonly low: bigint;
}

declare const operationKeyBrand: unique symbol;

/** Internal map key for the high/low 128-bit operation identity domain. */
export type ZLinkOperationIdentityKey = string & {
  readonly [operationKeyBrand]: true;
};

export function operationIdentityKey(
  operationId: ZLinkOperationIdentity128
): ZLinkOperationIdentityKey {
  requireUnsigned64(operationId.high, 'operationId.high');
  requireUnsigned64(operationId.low, 'operationId.low');
  if (operationId.high === 0n && operationId.low === 0n) {
    throw new RangeError('Operation identity must not be all zero.');
  }
  return `${operationId.high.toString(16)}:${operationId.low.toString(16)}` as ZLinkOperationIdentityKey;
}

export function createRandomOperationIdentity(
  source: (size: number) => Buffer = randomBytes
): ZLinkOperationIdentity128 {
  for (;;) {
    const bytes = source(OPERATION_IDENTITY_BYTES);
    if (bytes.length !== OPERATION_IDENTITY_BYTES) {
      throw new RangeError(
        `Operation identity entropy source must return exactly ${OPERATION_IDENTITY_BYTES} bytes.`
      );
    }
    const operationId = {
      high: bytes.readBigUInt64BE(0),
      low: bytes.readBigUInt64BE(UINT64_BYTES)
    };
    if (operationId.high !== 0n || operationId.low !== 0n) {
      return operationId;
    }
  }
}

function requireUnsigned64(value: bigint, name: string): void {
  if (typeof value !== 'bigint' || value < 0n || value > UINT64_MAX) {
    throw new RangeError(`${name} must be an unsigned 64-bit integer.`);
  }
}
