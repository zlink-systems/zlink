import { readZLinkDecoratorMetadata } from '../../contracts/Handlers/Attributes';
import type { ZLinkPacketJsonContract } from '../../contracts/Handlers/JsonContract';
import { ZLinkConfigurationException } from '../configuration';

const STRUCTURAL_PAYLOAD_NAMES = new Set([
  'Object',
  'Array',
  'Buffer',
  'Uint8Array',
  'String',
  'Number',
  'Boolean',
  'BigInt',
  'Symbol',
  'Date'
]);

export function resolveFrameworkPacketName(
  payload: unknown,
  explicitPacketName: string | undefined,
  surface: string
): string {
  const packetName = normalizePacketName(explicitPacketName)
    ?? tryDecoratorPacketName(payload)
    ?? tryConstructorPacketName(payload);
  if (packetName === undefined) {
    throw new ZLinkConfigurationException(
      `${surface} packetName is required when the payload type cannot provide one.`
    );
  }
  return packetName;
}

export function resolveFrameworkPacketJsonContract(
  payload: unknown,
  explicitPacketName?: string
): ZLinkPacketJsonContract | undefined {
  if (typeof payload !== 'object' || payload === null) return undefined;
  const constructor = (payload as { constructor?: object }).constructor;
  if (constructor === undefined || constructor === Object) return undefined;
  return readFrameworkPacketJsonContract(constructor, explicitPacketName);
}

export function readFrameworkPacketJsonContract(
  type: object,
  explicitPacketName?: string
): ZLinkPacketJsonContract | undefined {
  const expectedPacketName = normalizePacketName(explicitPacketName);
  for (const metadata of readZLinkDecoratorMetadata(type)) {
    if (
      metadata.kind === 'packet'
      && metadata.jsonContract !== undefined
      && (expectedPacketName === undefined || normalizePacketName(metadata.packetName) === expectedPacketName)
    ) {
      return metadata.jsonContract;
    }
  }
  return undefined;
}

function tryDecoratorPacketName(payload: unknown): string | undefined {
  if (typeof payload !== 'object' || payload === null) {
    return undefined;
  }
  const constructor = (payload as { constructor?: object }).constructor;
  if (constructor === undefined || constructor === Object) {
    return undefined;
  }
  for (const metadata of readZLinkDecoratorMetadata(constructor)) {
    if (metadata.kind === 'packet') {
      return normalizePacketName(metadata.packetName);
    }
  }
  return undefined;
}

function tryConstructorPacketName(payload: unknown): string | undefined {
  if (typeof payload !== 'object' || payload === null) {
    return undefined;
  }
  const name = (payload as { constructor?: { name?: unknown } }).constructor?.name;
  if (typeof name !== 'string' || STRUCTURAL_PAYLOAD_NAMES.has(name)) {
    return undefined;
  }
  return normalizePacketName(name);
}

function normalizePacketName(value: unknown): string | undefined {
  if (typeof value !== 'string') {
    return undefined;
  }
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}
