import type { RoutingId } from '../contracts';

class ZLinkOpaqueRoutingId {
  constructor(
    private readonly text: string,
    private readonly hex: string
  ) {}

  toHex(): string {
    return this.hex;
  }

  toBytes(): Uint8Array {
    return Buffer.from(this.hex, 'hex');
  }

  toString(): string {
    return this.text;
  }
}

export function normalizeRoutingId(value: RoutingId | string): RoutingId {
  return String(value);
}

export function normalizeOpaqueRoutingId(value: unknown): RoutingId {
  if (typeof value === 'string') return value;
  if (typeof value === 'object' && value !== null) {
    const candidate = value as { toHex?: () => unknown; toString?: () => unknown };
    if (typeof candidate.toHex === 'function') {
      const hex = candidate.toHex.call(value);
      if (typeof hex === 'string') {
        return normalizeRoutingIdHex(hex);
      }
    }
    if (typeof candidate.toString === 'function') {
      const text = candidate.toString.call(value);
      if (typeof text === 'string' && text !== '[object Object]') return text;
    }
  }
  throw new TypeError('RoutingId must be a string or expose a string conversion.');
}

export function decodeRoutingId(text: string, hex: unknown): RoutingId {
  if (typeof hex !== 'string') {
    return normalizeRoutingId(text);
  }
  const normalizedHex = normalizeRoutingIdHex(hex);
  const bytes = Buffer.from(normalizedHex, 'hex');
  const decoded = bytes.toString('utf8');
  if (
    Buffer.from(decoded, 'utf8').toString('hex') === normalizedHex
    && !/[\u0000-\u001f\u007f-\u009f]/u.test(decoded)
  ) {
    return normalizeRoutingId(decoded);
  }
  return new ZLinkOpaqueRoutingId(text, normalizedHex) as unknown as RoutingId;
}

export function routingIdWireHex(routingId: RoutingId): string | undefined {
  const value = routingId as unknown as { toHex?: () => string };
  return typeof value.toHex === 'function'
    ? value.toHex.call(routingId).toLowerCase()
    : undefined;
}

export function encodeRoutingIdStorageHex(routingId: RoutingId): string {
  const wireHex = routingIdWireHex(routingId);
  if (wireHex !== undefined) {
    return wireHex;
  }

  if (typeof routingId !== 'string') {
    throw new TypeError('RoutingId must be a string or expose toHex().');
  }

  return Buffer.from(routingId, 'utf8').toString('hex');
}

export function toBackendRoutingId(routingId: unknown): RoutingId {
  if (typeof routingId === 'string') {
    return routingId;
  }

  const value = routingId as unknown as {
    toBytes?: () => Uint8Array;
    toHex?: () => string;
  };
  if (typeof value.toBytes === 'function') {
    const bytes = value.toBytes.call(routingId);
    return new ZLinkOpaqueRoutingId(String(routingId), Buffer.from(bytes).toString('hex')) as unknown as RoutingId;
  }
  if (typeof value.toHex === 'function') {
    return new ZLinkOpaqueRoutingId(
      String(routingId),
      normalizeRoutingIdHex(value.toHex.call(routingId))
    ) as unknown as RoutingId;
  }

  throw new TypeError('RoutingId must be a string or expose toBytes() or toHex().');
}

function normalizeRoutingIdHex(value: string): string {
  const hex = value.toLowerCase();
  if (hex.length === 0 || hex.length % 2 !== 0 || !/^[0-9a-f]+$/.test(hex)) {
    throw new TypeError('RoutingId hex must contain a non-empty, even number of hexadecimal digits.');
  }
  return hex;
}

export function routingIdsEqual(
  left: RoutingId | undefined,
  right: RoutingId | undefined
): boolean {
  if (left === undefined || right === undefined) {
    return left === right;
  }
  return encodeRoutingIdStorageHex(left) === encodeRoutingIdStorageHex(right);
}
