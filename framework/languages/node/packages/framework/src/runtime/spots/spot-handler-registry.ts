import type {
  Type,
  ZLinkActor,
  ZLinkEntrySpot,
  ZLinkInstanceSpotHandlerRegistry,
  ZLinkSpot,
  ZLinkSpotHandlerRegistry,
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
import { ZLinkConfigurationException } from '../configuration';
import {
  ZLinkActorPacketKind,
  ZLinkSpotActorHandlerRegistryRuntime
} from '../actors';
import { readZLinkDecoratorMetadata } from '../../contracts/Handlers/Attributes';

export interface ZLinkSpotHandlerRegistration {
  readonly kind:
    | 'handler'
    | 'packet'
    | 'subscribe'
    | 'actorSend'
    | 'actorRequest'
    | 'spotHandler';
  readonly handlerType: Type;
  readonly packetName?: string;
  readonly channelName?: string;
  readonly topic?: string;
  readonly actorType?: Type<ZLinkActor>;
}

export class DefaultZLinkSpotHandlerRegistry<TActor extends ZLinkActor = ZLinkActor> implements ZLinkSpotHandlerRegistry {
  private readonly entries: ZLinkSpotHandlerRegistration[] = [];

  constructor(private readonly actorHandlers?: ZLinkSpotActorHandlerRegistryRuntime) {}

  addHandler(handlerType: Type): this {
    this.entries.push({ kind: 'handler', handlerType });
    return this;
  }

  addActorPacket<THandler, TRegisteredActor extends ZLinkActor>(
    handlerType: Type<THandler>,
    actorType: Type<TRegisteredActor>
  ): this {
    const metadata = readZLinkDecoratorMetadata(handlerType).find((entry) =>
      entry.kind === 'spotActorSend' || entry.kind === 'spotActorRequest');
    if (metadata?.packetName === undefined) {
      throw new ZLinkConfigurationException(`Actor packet handler '${handlerType.name}' must declare a packet name.`);
    }
    return this.addActorPacketRegistration(
      metadata.kind === 'spotActorSend' ? ZLinkActorPacketKind.Send : ZLinkActorPacketKind.Request,
      handlerType,
      actorType as unknown as Type<TActor>,
      metadata.packetName
    );
  }

  addPacket(handlerType: Type, packetName?: string): this {
    const declared = readZLinkDecoratorMetadata(handlerType)
      .find((entry) => entry.kind === 'packet')?.packetName;
    this.entries.push({ kind: 'packet', handlerType, packetName: packetName ?? declared ?? handlerType.name });
    return this;
  }

  packet(packetName: string, handlerType: Type): this {
    return this.addPacket(handlerType, packetName);
  }

  addActorPacketRegistration(
    kind: ZLinkActorPacketKind,
    handlerType: Type,
    actorType: Type<TActor>,
    packetName: string
  ): this {
    this.entries.push({
      kind: kind === ZLinkActorPacketKind.Send ? 'actorSend' : 'actorRequest',
      handlerType,
      actorType,
      packetName
    });
    this.actorHandlers?.addPacket({
      kind,
      packetName,
      actorType,
      handlerType
    });
    return this;
  }

  actorSend(packetName: string, handlerType: Type, actorType: Type<TActor> = Object as unknown as Type<TActor>): this {
    return this.addActorPacketRegistration(
      ZLinkActorPacketKind.Send,
      handlerType,
      actorType,
      packetName
    );
  }

  actorRequest(packetName: string, handlerType: Type, actorType: Type<TActor> = Object as unknown as Type<TActor>): this {
    return this.addActorPacketRegistration(
      ZLinkActorPacketKind.Request,
      handlerType,
      actorType,
      packetName
    );
  }

  addSubscribe(handlerType: Type, channelName: string, topic: string): this {
    if (channelName.trim().length === 0) {
      throw new ZLinkConfigurationException('SPOT subscribe channel name must not be empty.');
    }
    if (topic.trim().length === 0) {
      throw new ZLinkConfigurationException('SPOT subscribe topic must not be empty.');
    }
    if (this.entries.some((entry) => entry.kind === 'subscribe'
      && entry.handlerType === handlerType
      && entry.channelName === channelName
      && entry.topic === topic)) {
      return this;
    }
    this.entries.push({ kind: 'subscribe', handlerType, channelName, topic });
    return this;
  }

  subscribe(channelName: string, topic: string, handlerType: Type): this {
    return this.addSubscribe(handlerType, channelName, topic);
  }

  addSpotHandler(handlerType: Type): this {
    this.entries.push({ kind: 'spotHandler', handlerType });
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
        handler.packetName
      );
    }
  }
  for (const handler of registrations.actorRequestHandlers ?? []) {
    if (handler.entrySpotType === entrySpotType) {
      registry.addActorPacketRegistration(
        ZLinkActorPacketKind.Request,
        handler.handlerType,
        handler.actorType,
        handler.packetName
      );
    }
  }
  for (const handler of registrations.packetHandlers ?? []) {
    if (handler.entrySpotType === entrySpotType) {
      registry.addPacket(handler.handlerType, handler.packetName);
    }
  }
  for (const handler of registrations.subscriptionHandlers ?? []) {
    if (handler.entrySpotType === entrySpotType) {
      registry.addSubscribe(handler.handlerType, handler.channelName, handler.topic);
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
        handler.packetName
      );
    }
  }
  for (const handler of registrations.actorRequestHandlers ?? []) {
    if (isSpotImplementation(handler.spotType, spotType)) {
      registry.addActorPacketRegistration(
        ZLinkActorPacketKind.Request,
        handler.handlerType,
        handler.actorType,
        handler.packetName
      );
    }
  }
  for (const handler of registrations.packetHandlers ?? []) {
    if (isSpotImplementation(handler.spotType, spotType)) {
      registry.addPacket(handler.handlerType, handler.packetName);
    }
  }
  for (const handler of registrations.subscriptionHandlers ?? []) {
    if (isSpotImplementation(handler.spotType, spotType)) {
      registry.addSubscribe(handler.handlerType, handler.channelName, handler.topic);
    }
  }
}

function isSpotImplementation(
  registeredType: Type<ZLinkSpot>,
  implementation: Type<ZLinkSpot>
): boolean {
  return registeredType === implementation
    || implementation.prototype instanceof registeredType;
}
