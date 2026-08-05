export function asArray(value: unknown): readonly unknown[] {
  if (!Array.isArray(value)) {
    throw new TypeError('Redis command returned a non-array value.');
  }
  return value;
}

export function asString(value: unknown): string {
  if (typeof value === 'string') {
    return value;
  }
  if (Buffer.isBuffer(value)) {
    return value.toString();
  }
  if (typeof value === 'number' || typeof value === 'bigint') {
    return String(value);
  }
  throw new TypeError('Redis command returned a non-string value.');
}

export function toNumber(value: unknown): number {
  return Number(asString(value));
}
