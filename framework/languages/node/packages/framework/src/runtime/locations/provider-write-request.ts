import {
  ZLINK_PROVIDER_MAX_KEY_BYTES,
  ZLINK_PROVIDER_MAX_VALUE_BYTES,
  ZLINK_PROVIDER_MAX_VERSION_BYTES,
  ZLINK_PROVIDER_MAX_WRITE_BYTES,
  ZLINK_PROVIDER_MAX_WRITE_KEYS,
  type ZLinkStoreCondition,
  type ZLinkStoreWriteRequest
} from '../../contracts/Locations/Stores';

export const STORE_COUNTER_MAX = (1n << 63n) - 1n;
export const STORE_COUNTER_MAX_BYTES = STORE_COUNTER_MAX.toString().length;

export function readStoreCounter(bytes: Uint8Array | undefined): bigint | undefined {
  if (bytes === undefined) return 0n;
  if (bytes.byteLength === 0 || bytes.byteLength > STORE_COUNTER_MAX_BYTES) return undefined;
  const decimal = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString('latin1');
  if (/[^0-9]/.test(decimal) || (decimal.length > 1 && decimal[0] === '0')) return undefined;
  const value = BigInt(decimal);
  return value <= STORE_COUNTER_MAX ? value : undefined;
}

export function sumStoreCounters(
  condition: Extract<ZLinkStoreCondition, { kind: 'sumAtMost' }>,
  read: (key: string) => bigint | undefined
): boolean | undefined {
  let total = condition.delta;
  for (const key of condition.keys) {
    const value = read(key.value);
    if (value === undefined) return undefined;
    total += value;
  }
  return total <= condition.limit;
}

export function addStoreCounter(current: bigint | undefined, delta: bigint): Buffer | undefined {
  if (current === undefined) return undefined;
  const next = current + delta;
  return next < 0n || next > STORE_COUNTER_MAX ? undefined : Buffer.from(next.toString());
}

function textSize(value: string, max: number): number {
  if (typeof value !== 'string') throw new TypeError('Store input must be a string.');
  const size = Buffer.byteLength(value);
  if (size < 1 || size > max) throw new RangeError('Store input exceeds its UTF-8 bound.');
  return size;
}

function valueSize(value: Uint8Array): number {
  if (!(value instanceof Uint8Array)) throw new TypeError('Store value must be bytes.');
  if (value.byteLength > ZLINK_PROVIDER_MAX_VALUE_BYTES)
    throw new RangeError(
      `Location Store value exceeds ${ZLINK_PROVIDER_MAX_VALUE_BYTES / (1024 * 1024)} MiB.`
    );
  return value.byteLength;
}

function operandSize(value: bigint): number {
  if (typeof value !== 'bigint') throw new TypeError('Store counter operands must be bigint.');
  return value.toString().length;
}

export function validateStoreWriteRequest(
  request: ZLinkStoreWriteRequest,
  maxRetentionMs = Number.MAX_SAFE_INTEGER
): Set<string> {
  const conditionKeys = new Set<string>();
  const mutationKeys = new Set<string>();
  let size = 0;
  function addKey(keys: Set<string>, value: string) {
    if (keys.has(value)) throw new RangeError('Duplicate Store write key.');
    keys.add(value);
    size += textSize(value, ZLINK_PROVIDER_MAX_KEY_BYTES);
  }
  for (const condition of request.conditions) {
    switch (condition.kind) {
      case 'sumAtMost':
        for (const key of condition.keys) addKey(conditionKeys, key.value);
        size += operandSize(condition.delta) + operandSize(condition.limit);
        break;
      case 'value':
        size += valueSize(condition.expected);
        addKey(conditionKeys, condition.key.value);
        break;
      case 'version':
        size += textSize(condition.expected.value, ZLINK_PROVIDER_MAX_VERSION_BYTES);
        addKey(conditionKeys, condition.key.value);
        break;
      case 'missing':
        addKey(conditionKeys, condition.key.value);
        break;
      default:
        throw new TypeError('Invalid Store condition.');
    }
  }
  for (const mutation of request.mutations) {
    addKey(mutationKeys, mutation.key.value);
    switch (mutation.kind) {
      case 'put': {
        size += valueSize(mutation.bytes);
        const retention = mutation.retentionMs;
        if (
          retention !== undefined &&
          (!Number.isFinite(retention) || retention <= 0 || Math.ceil(retention) > maxRetentionMs)
        ) {
          throw new RangeError('Invalid Store retention.');
        }
        break;
      }
      case 'add':
        size += operandSize(mutation.delta);
        break;
      case 'delete':
        break;
      default:
        throw new TypeError('Invalid Store mutation.');
    }
  }
  const keys = new Set([...conditionKeys, ...mutationKeys]);
  if (keys.size > ZLINK_PROVIDER_MAX_WRITE_KEYS || size > ZLINK_PROVIDER_MAX_WRITE_BYTES)
    throw new RangeError('Store batch exceeds its bound.');
  return keys;
}
