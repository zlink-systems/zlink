import {
  defineZLinkPacketJsonContract,
  readZLinkPacketJsonContract as readDefinedZLinkPacketJsonContract,
  type ZLinkPacketJsonContract
} from './JsonContract';

import {
  ZLINK_DECORATOR_METADATA,
  readZLinkDecoratorMetadata,
  type ZLinkDecoratorMetadata
} from '@zlink-systems/stream-wire/internal/json';
export {
  ZLINK_DECORATOR_METADATA,
  readZLinkDecoratorMetadata
} from '@zlink-systems/stream-wire/internal/json';
export type { ZLinkDecoratorMetadata } from '@zlink-systems/stream-wire/internal/json';

export function ZLinkHandlerGroup(groupName: string): ClassDecorator {
  return classDecorator({ kind: 'handlerGroup', groupName });
}

export interface ZLinkRequest<TReply> extends Promise<TReply> {
  readonly admitted: Promise<void>;
}

export function ZLinkRequest(packetName?: string): MethodDecorator {
  return methodDecorator({ kind: 'request', packetName });
}

export function ZLinkSend(packetName?: string): MethodDecorator {
  return methodDecorator({ kind: 'send', packetName });
}

export function ZLinkPublish(packetName?: string): MethodDecorator {
  return methodDecorator({ kind: 'publish', packetName });
}

/** @internal Generated schema registration overload. */
export function ZLinkPacket(
  packetName: string,
  jsonContract?: ZLinkPacketJsonContract
): ClassDecorator;

export function ZLinkPacket(packetName: string): ClassDecorator;

export function ZLinkPacket(
  packetName: string,
  jsonContract?: ZLinkPacketJsonContract
): ClassDecorator {
  return (target) => {
    const normalized =
      jsonContract === undefined
        ? undefined
        : defineZLinkPacketJsonContract(packetName, jsonContract);
    appendMetadata(target, { kind: 'packet', packetName, jsonContract: normalized });
  };
}

/** @internal */
export function readZLinkPacketJsonContract(
  packetName: string
): ZLinkPacketJsonContract | undefined {
  return readDefinedZLinkPacketJsonContract(packetName);
}

export function ZLinkSpotRequest(packetName?: string): MethodDecorator {
  return methodDecorator({ kind: 'spotRequest', packetName });
}

export function ZLinkSpotSubscription(channelName: string, topic: string): MethodDecorator {
  return methodDecorator({ kind: 'spotSubscription', channelName, topic });
}

export function ZLinkSpotActorSend(packetName?: string): MethodDecorator {
  return methodDecorator({ kind: 'spotActorSend', packetName });
}

export function ZLinkSpotActorRequest(packetName?: string): MethodDecorator {
  return methodDecorator({ kind: 'spotActorRequest', packetName });
}

export function ZLinkStreamPacket(): MethodDecorator {
  return methodDecorator({ kind: 'streamPacket' });
}

export function ZLinkStreamRaw(): MethodDecorator {
  return methodDecorator({ kind: 'streamRaw' });
}

function classDecorator(metadata: ZLinkDecoratorMetadata): ClassDecorator {
  return (target) => appendMetadata(target, metadata);
}

function methodDecorator(metadata: ZLinkDecoratorMetadata): MethodDecorator {
  return (target, propertyKey) =>
    appendMetadata(target.constructor, {
      ...metadata,
      methodName: String(propertyKey)
    });
}

function appendMetadata(target: object, metadata: ZLinkDecoratorMetadata): void {
  const current = readZLinkDecoratorMetadata(target);
  Object.defineProperty(target, ZLINK_DECORATOR_METADATA, {
    configurable: true,
    enumerable: false,
    value: [...current, metadata],
    writable: false
  });
}
