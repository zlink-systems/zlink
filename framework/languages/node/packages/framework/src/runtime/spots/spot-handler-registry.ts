import type { ZLinkHandlerMessageType } from '../../contracts/Common/HandlerMessageType';
import type {
  Type,
  ZLinkActor,
  ZLinkEntrySpot,
  ZLinkInstanceSpot,
  ZLinkInstanceSpotHandlerRegistry,
  ZLinkSpot,
  ZLinkSpotHandlerRegistry,
  ZLinkSpotTimerHandler
} from '../../contracts';
import type {
  ZLinkEntrySpotActorRequestHandlerRegistration,
  ZLinkEntrySpotActorSendHandlerRegistration,
  ZLinkEntrySpotPacketHandlerRegistration,
  ZLinkEntrySpotSubscriptionHandlerRegistration,
  ZLinkSpotActorRequestHandlerRegistration,
  ZLinkSpotActorSendHandlerRegistration,
  ZLinkSpotPacketHandlerRegistration,
  ZLinkSpotSubscriptionHandlerRegistration
} from '../../contracts/Configuration/RegistrationTypes';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { resolveHandlerMessageType } from '../../contracts/Common/HandlerMessageType';
import { prepareHandlerInstance } from '../handlers/handler-instance-scope';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { ZLinkActorPacketKind, ZLinkSpotActorHandlerRegistryRuntime } from '../actors';
import { readZLinkDecoratorMetadata } from '../../contracts/Handlers/Attributes';

export interface ZLinkSpotHandlerRegistration {
  readonly kind: 'packet' | 'subscribe' | 'actorSend' | 'actorRequest' | 'timer';
  readonly handlerType: Type;
  readonly messageType?: ZLinkHandlerMessageType;
  readonly packetName?: string;
  readonly channelName?: string;
  readonly topic?: string;
  readonly actorType?: Type<ZLinkActor>;
}

export class DefaultZLinkSpotHandlerRegistry<
  TActor extends ZLinkActor = ZLinkActor
> implements ZLinkSpotHandlerRegistry {
  private readonly entries: ZLinkSpotHandlerRegistration[] = [];

  constructor(
    private readonly actorHandlers?: ZLinkSpotActorHandlerRegistryRuntime,
    private readonly providerResolver?: ZLinkProviderResolver
  ) {}

  addHandler<THandler>(handlerType: Type<THandler>, packetName?: string): this {
    const metadata = readZLinkDecoratorMetadata(handlerType).find(
      (entry) => entry.kind === 'spotActorSend' || entry.kind === 'spotActorRequest'
    );
    const resolvedPacketName = packetName ?? metadata?.packetName;
    if (metadata === undefined || resolvedPacketName === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `Actor packet handler '${handlerType.name}' must declare a packet name.`
      );
    }
    return this.addActorPacketRegistration(
      metadata.kind === 'spotActorSend' ? ZLinkActorPacketKind.Send : ZLinkActorPacketKind.Request,
      handlerType,
      Object as unknown as Type<TActor>,
      resolvedPacketName,
      resolveHandlerMessageType(handlerType)
    );
  }

  addActorPacket<THandler, TRegisteredActor extends ZLinkActor>(
    handlerType: Type<THandler>,
    actorType: Type<TRegisteredActor>
  ): this {
    const metadata = readZLinkDecoratorMetadata(handlerType).find(
      (entry) => entry.kind === 'spotActorSend' || entry.kind === 'spotActorRequest'
    );
    if (metadata?.packetName === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `Actor packet handler '${handlerType.name}' must declare a packet name.`
      );
    }
    return this.addActorPacketRegistration(
      metadata.kind === 'spotActorSend' ? ZLinkActorPacketKind.Send : ZLinkActorPacketKind.Request,
      handlerType,
      actorType as unknown as Type<TActor>,
      metadata.packetName,
      resolveHandlerMessageType(handlerType)
    );
  }

  addPacket(handlerType: Type, packetName?: string): this {
    return this.addPacketRegistration(
      handlerType,
      packetName,
      resolveHandlerMessageType(handlerType)
    );
  }

  addPacketRegistration(
    handlerType: Type,
    packetName: string | undefined,
    messageType: ZLinkHandlerMessageType | undefined
  ): this {
    const declared = readZLinkDecoratorMetadata(handlerType).find(
      (entry) => entry.kind === 'packet'
    )?.packetName;
    prepareHandlerInstance(this.providerResolver, handlerType);
    const resolvedPacketName = packetName ?? declared ?? handlerType.name;
    if (
      this.entries.some(
        (entry) => entry.kind === 'packet' && entry.packetName === resolvedPacketName
      )
    )
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `SPOT packet '${resolvedPacketName}' is already registered.`
      );
    this.entries.push({
      kind: 'packet',
      handlerType,
      ...(messageType === undefined ? {} : { messageType: messageType }),
      packetName: resolvedPacketName
    });
    return this;
  }

  packet(packetName: string, handlerType: Type): this {
    return this.addPacket(handlerType, packetName);
  }

  addActorPacketRegistration(
    kind: ZLinkActorPacketKind,
    handlerType: Type,
    actorType: Type<TActor>,
    packetName: string,
    messageType: ZLinkHandlerMessageType | undefined
  ): this {
    prepareHandlerInstance(this.providerResolver, handlerType);
    this.entries.push({
      kind: kind === ZLinkActorPacketKind.Send ? 'actorSend' : 'actorRequest',
      handlerType,
      ...(messageType === undefined ? {} : { messageType: messageType }),
      actorType,
      packetName
    });
    this.actorHandlers?.addPacket({
      kind,
      packetName,
      actorType,
      handlerType,
      messageType: messageType
    });
    return this;
  }

  actorSend(
    packetName: string,
    handlerType: Type,
    actorType: Type<TActor> = Object as unknown as Type<TActor>
  ): this {
    return this.addActorPacketRegistration(
      ZLinkActorPacketKind.Send,
      handlerType,
      actorType,
      packetName,
      resolveHandlerMessageType(handlerType)
    );
  }

  actorRequest(
    packetName: string,
    handlerType: Type,
    actorType: Type<TActor> = Object as unknown as Type<TActor>
  ): this {
    return this.addActorPacketRegistration(
      ZLinkActorPacketKind.Request,
      handlerType,
      actorType,
      packetName,
      resolveHandlerMessageType(handlerType)
    );
  }

  addSubscribe(handlerType: Type, channelName: string, topic: string): this {
    return this.addSubscriptionRegistration(
      handlerType,
      channelName,
      topic,
      resolveHandlerMessageType(handlerType)
    );
  }

  addSubscriptionRegistration(
    handlerType: Type,
    channelName: string,
    topic: string,
    messageType: ZLinkHandlerMessageType | undefined
  ): this {
    if (channelName.trim().length === 0) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        'SPOT subscribe channel name must not be empty.'
      );
    }
    if (topic.trim().length === 0) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        'SPOT subscribe topic must not be empty.'
      );
    }
    if (
      this.entries.some(
        (entry) =>
          entry.kind === 'subscribe' &&
          entry.handlerType === handlerType &&
          entry.channelName === channelName &&
          entry.topic === topic
      )
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `SPOT subscription '${channelName}/${topic}' is already registered.`
      );
    }
    prepareHandlerInstance(this.providerResolver, handlerType);
    this.entries.push({
      kind: 'subscribe',
      handlerType,
      ...(messageType === undefined ? {} : { messageType: messageType }),
      channelName,
      topic
    });
    return this;
  }

  subscribe(channelName: string, topic: string, handlerType: Type): this {
    return this.addSubscribe(handlerType, channelName, topic);
  }

  addTimerHandler<THandler extends ZLinkSpotTimerHandler<ZLinkSpot>>(
    handlerType: Type<THandler>
  ): this {
    prepareHandlerInstance(this.providerResolver, handlerType);
    this.entries.push({ kind: 'timer', handlerType });
    return this;
  }

  snapshot(): readonly ZLinkSpotHandlerRegistration[] {
    return [...this.entries];
  }
}

/**
 * Exposes only the actor-free handler surface permitted for Instance Spots while
 * retaining the common immutable registration snapshot used by dispatch.
 */
export class DefaultZLinkInstanceSpotHandlerRegistry implements ZLinkInstanceSpotHandlerRegistry {
  constructor(private readonly registrations: DefaultZLinkSpotHandlerRegistry) {}

  addPacket<THandler>(handlerType: Type<THandler>): this {
    this.registrations.addPacket(handlerType);
    return this;
  }

  addTimerHandler<THandler extends ZLinkSpotTimerHandler<ZLinkInstanceSpot>>(
    handlerType: Type<THandler>
  ): this {
    this.registrations.addTimerHandler(handlerType as never);
    return this;
  }

  snapshot(): readonly ZLinkSpotHandlerRegistration[] {
    return this.registrations.snapshot();
  }
}

interface ZLinkEntrySpotHandlerRegistrationSet {
  readonly actorSendHandlers?: readonly ZLinkEntrySpotActorSendHandlerRegistration[];
  readonly actorRequestHandlers?: readonly ZLinkEntrySpotActorRequestHandlerRegistration[];
  readonly packetHandlers?: readonly ZLinkEntrySpotPacketHandlerRegistration[];
  readonly subscriptionHandlers?: readonly ZLinkEntrySpotSubscriptionHandlerRegistration[];
}

interface ZLinkUserSpotHandlerRegistrationSet {
  readonly actorSendHandlers?: readonly ZLinkSpotActorSendHandlerRegistration[];
  readonly actorRequestHandlers?: readonly ZLinkSpotActorRequestHandlerRegistration[];
  readonly packetHandlers?: readonly ZLinkSpotPacketHandlerRegistration[];
  readonly subscriptionHandlers?: readonly ZLinkSpotSubscriptionHandlerRegistration[];
}

export function applyEntrySpotHandlerRegistrations(
  registry: DefaultZLinkSpotHandlerRegistry,
  entrySpotType: Type<ZLinkEntrySpot>,
  registrations: ZLinkEntrySpotHandlerRegistrationSet
): void {
  for (const handler of registrations.actorSendHandlers ?? []) {
    if (handler.entrySpotType === entrySpotType) {
      registry.addActorPacketRegistration(
        ZLinkActorPacketKind.Send,
        handler.handlerType,
        handler.actorType,
        handler.packetName,
        handler.messageType
      );
    }
  }
  for (const handler of registrations.actorRequestHandlers ?? []) {
    if (handler.entrySpotType === entrySpotType) {
      registry.addActorPacketRegistration(
        ZLinkActorPacketKind.Request,
        handler.handlerType,
        handler.actorType,
        handler.packetName,
        handler.messageType
      );
    }
  }
  for (const handler of registrations.packetHandlers ?? []) {
    if (handler.entrySpotType === entrySpotType) {
      registry.addPacketRegistration(handler.handlerType, handler.packetName, handler.messageType);
    }
  }
  for (const handler of registrations.subscriptionHandlers ?? []) {
    if (handler.entrySpotType === entrySpotType) {
      registry.addSubscriptionRegistration(
        handler.handlerType,
        handler.channelName,
        handler.topic,
        handler.messageType
      );
    }
  }
}

export function applySpotHandlerRegistrations(
  registry: DefaultZLinkSpotHandlerRegistry,
  spotType: Type<ZLinkSpot>,
  registrations: ZLinkUserSpotHandlerRegistrationSet
): void {
  for (const handler of registrations.actorSendHandlers ?? []) {
    if (isSpotImplementation(handler.spotType, spotType)) {
      registry.addActorPacketRegistration(
        ZLinkActorPacketKind.Send,
        handler.handlerType,
        handler.actorType,
        handler.packetName,
        handler.messageType
      );
    }
  }
  for (const handler of registrations.actorRequestHandlers ?? []) {
    if (isSpotImplementation(handler.spotType, spotType)) {
      registry.addActorPacketRegistration(
        ZLinkActorPacketKind.Request,
        handler.handlerType,
        handler.actorType,
        handler.packetName,
        handler.messageType
      );
    }
  }
  for (const handler of registrations.packetHandlers ?? []) {
    if (isSpotImplementation(handler.spotType, spotType)) {
      registry.addPacketRegistration(handler.handlerType, handler.packetName, handler.messageType);
    }
  }
  for (const handler of registrations.subscriptionHandlers ?? []) {
    if (isSpotImplementation(handler.spotType, spotType)) {
      registry.addSubscriptionRegistration(
        handler.handlerType,
        handler.channelName,
        handler.topic,
        handler.messageType
      );
    }
  }
}

function isSpotImplementation(
  registeredType: Type<ZLinkSpot>,
  implementation: Type<ZLinkSpot>
): boolean {
  return registeredType === implementation || implementation.prototype instanceof registeredType;
}
