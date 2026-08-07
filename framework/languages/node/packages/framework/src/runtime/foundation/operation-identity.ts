import { randomBytes } from 'node:crypto';

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
  return `${operationId.high.toString(16)}:${operationId.low.toString(16)}` as
    ZLinkOperationIdentityKey;
}

export function createRandomOperationIdentity(
  source: (size: number) => Buffer = randomBytes
): ZLinkOperationIdentity128 {
  for (;;) {
    const bytes = source(16);
    if (bytes.length !== 16) {
      throw new RangeError('Operation identity entropy source must return exactly 16 bytes.');
    }
    const operationId = {
      high: bytes.readBigUInt64BE(0),
      low: bytes.readBigUInt64BE(8)
    };
    if (operationId.high !== 0n || operationId.low !== 0n) {
      return operationId;
    }
  }
}
