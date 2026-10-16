import type { ZLinkPacketJsonContract } from './json-contract';

export const ZLINK_DECORATOR_METADATA = Symbol.for('@zlink-systems/framework:decorator');

export interface ZLinkDecoratorMetadata {
  readonly kind: string;
  readonly packetName?: string;
  readonly groupName?: string;
  readonly methodName?: string;
  readonly meshName?: string;
  readonly channelName?: string;
  readonly topic?: string;
  readonly jsonContract?: ZLinkPacketJsonContract;
}

export function readZLinkDecoratorMetadata(target: object): readonly ZLinkDecoratorMetadata[] {
  if (!Object.prototype.hasOwnProperty.call(target, ZLINK_DECORATOR_METADATA)) {
    return [];
  }
  return (target as Record<symbol, unknown>)[
    ZLINK_DECORATOR_METADATA
  ] as readonly ZLinkDecoratorMetadata[];
}

export function readFrameworkPacketJsonContract(
  type: object,
  explicitPacketName?: string
): ZLinkPacketJsonContract | undefined {
  const expectedPacketName = normalizePacketName(explicitPacketName);
  for (const metadata of readZLinkDecoratorMetadata(type)) {
    if (
      metadata.kind === 'packet' &&
      metadata.jsonContract !== undefined &&
      (expectedPacketName === undefined ||
        normalizePacketName(metadata.packetName) === expectedPacketName)
    ) {
      return metadata.jsonContract;
    }
  }
  return undefined;
}

export function normalizePacketName(value: unknown): string | undefined {
  if (typeof value !== 'string') {
    return undefined;
  }
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}
