export const ZLINK_DECORATOR_METADATA = Symbol.for('@zlink-systems/framework:decorator');

export interface ZLinkDecoratorMetadata {
  readonly kind: string;
  readonly packetName?: string;
  readonly groupName?: string;
  readonly methodName?: string;
  readonly meshName?: string;
  readonly channelName?: string;
  readonly topic?: string;
}

export function ZLinkHandlerGroup(groupName: string): ClassDecorator {
  return classDecorator({ kind: 'handlerGroup', groupName });
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

export function ZLinkPacket(packetName: string): ClassDecorator {
  return classDecorator({ kind: 'packet', packetName });
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
  return (target, propertyKey) => appendMetadata(target.constructor, {
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

export function readZLinkDecoratorMetadata(target: object): readonly ZLinkDecoratorMetadata[] {
  if (!Object.prototype.hasOwnProperty.call(target, ZLINK_DECORATOR_METADATA)) {
    return [];
  }
  return (target as Record<symbol, unknown>)[ZLINK_DECORATOR_METADATA] as readonly ZLinkDecoratorMetadata[];
}
