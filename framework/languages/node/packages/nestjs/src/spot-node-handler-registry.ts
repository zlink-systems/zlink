import type {
  ZLinkEntrySpotActorRequestHandlerRegistration,
  ZLinkEntrySpotActorSendHandlerRegistration,
  ZLinkEntrySpotPacketHandlerRegistration,
  ZLinkEntrySpotSubscriptionHandlerRegistration,
  ZLinkEntrySpotTimerHandlerRegistration,
  ZLinkFrameworkRegistrationOptions,
  ZLinkSpotActorRequestHandlerRegistration,
  ZLinkSpotActorSendHandlerRegistration,
  ZLinkSpotNodeOptions,
  ZLinkSpotPacketHandlerRegistration,
  ZLinkSpotSubscriptionHandlerRegistration,
  ZLinkSpotTimerHandlerRegistration
} from './framework-integration-contracts';
import type { Mutable } from './contracts';
import type { Type, ZLinkInstanceSpot, ZLinkSpot } from '@zlink-systems/framework';
import { framework } from './framework-loader';

type MutableSpotNode = Mutable<ZLinkSpotNodeOptions>;

export class SpotNodeHandlerRegistry {
  private constructor(private readonly nodes: Record<string, MutableSpotNode>) {}

  static from(value: ZLinkFrameworkRegistrationOptions['spotNodes']): SpotNodeHandlerRegistry {
    if (value === undefined) {
      return new SpotNodeHandlerRegistry({});
    }
    if (!Array.isArray(value)) {
      return new SpotNodeHandlerRegistry(
        Object.fromEntries(Object.entries(value).map(([name, node]) => [name, { ...node }]))
      );
    }
    return new SpotNodeHandlerRegistry(Object.fromEntries(value.map((node) => {
      if (typeof node === 'string') {
        return [node, {}];
      }
      const { name, ...options } = node;
      return [name, { ...options }];
    })));
  }

  get isEmpty(): boolean {
    return Object.keys(this.nodes).length === 0;
  }

  toOptions(): ZLinkFrameworkRegistrationOptions['spotNodes'] {
    return this.nodes;
  }

  entrySpotTargets(
    entrySpotType: NonNullable<ZLinkSpotNodeOptions['entrySpotType']>,
    missingMessage: string
  ): readonly MutableSpotNode[] {
    return this.requireTargets(
      (node) => node.entrySpotType === entrySpotType,
      missingMessage
    );
  }

  spotTargets(
    spotType: Type<ZLinkSpot | ZLinkInstanceSpot>,
    missingMessage: string
  ): readonly MutableSpotNode[] {
    return this.requireTargets(
      (node) => (node.spotFactories ?? []).includes(spotType as Type<ZLinkSpot>)
        || Object.values(node.instanceSpotFactories ?? {}).includes(spotType as Type<ZLinkInstanceSpot>),
      missingMessage
    );
  }

  addEntrySpotTimer(node: MutableSpotNode, registration: ZLinkEntrySpotTimerHandlerRegistration): void {
    node.entrySpotTimerHandlers = [...(node.entrySpotTimerHandlers ?? []), registration];
  }

  addSpotTimer(node: MutableSpotNode, registration: ZLinkSpotTimerHandlerRegistration): void {
    node.spotTimerHandlers = [...(node.spotTimerHandlers ?? []), registration];
  }

  addEntrySpotPacket(node: MutableSpotNode, registration: ZLinkEntrySpotPacketHandlerRegistration): void {
    const packetName = registration.packetName ?? registration.handlerType.name;
    this.assertUnique(
      node.entrySpotPacketHandlers,
      (existing) => existing.entrySpotType === registration.entrySpotType &&
        (existing.packetName ?? existing.handlerType.name) === packetName,
      `Duplicate Entry Spot packet handler '${registration.entrySpotType.name}:${packetName}'.`
    );
    node.entrySpotPacketHandlers = [...(node.entrySpotPacketHandlers ?? []), registration];
  }

  addEntrySpotSubscription(node: MutableSpotNode, registration: ZLinkEntrySpotSubscriptionHandlerRegistration): void {
    this.assertUnique(
      node.entrySpotSubscriptionHandlers,
      (existing) => existing.entrySpotType === registration.entrySpotType &&
        existing.channelName === registration.channelName && existing.topic === registration.topic,
      `Duplicate Entry Spot subscription handler '${registration.entrySpotType.name}:${registration.channelName}:${registration.topic}'.`
    );
    node.entrySpotSubscriptionHandlers = [...(node.entrySpotSubscriptionHandlers ?? []), registration];
  }

  addSpotPacket(node: MutableSpotNode, registration: ZLinkSpotPacketHandlerRegistration): void {
    const packetName = registration.packetName ?? registration.handlerType.name;
    this.assertUnique(
      node.spotPacketHandlers,
      (existing) => existing.spotType === registration.spotType &&
        (existing.packetName ?? existing.handlerType.name) === packetName,
      `Duplicate SPOT packet handler '${registration.spotType.name}:${packetName}'.`
    );
    node.spotPacketHandlers = [...(node.spotPacketHandlers ?? []), registration];
  }

  addSpotSubscription(node: MutableSpotNode, registration: ZLinkSpotSubscriptionHandlerRegistration): void {
    this.assertUnique(
      node.spotSubscriptionHandlers,
      (existing) => existing.spotType === registration.spotType &&
        existing.channelName === registration.channelName && existing.topic === registration.topic,
      `Duplicate SPOT subscription handler '${registration.spotType.name}:${registration.channelName}:${registration.topic}'.`
    );
    node.spotSubscriptionHandlers = [...(node.spotSubscriptionHandlers ?? []), registration];
  }

  addEntrySpotActor(
    node: MutableSpotNode,
    kind: 'send' | 'request',
    registration: ZLinkEntrySpotActorSendHandlerRegistration | ZLinkEntrySpotActorRequestHandlerRegistration
  ): void {
    const existing = kind === 'send'
      ? node.entrySpotActorSendHandlers
      : node.entrySpotActorRequestHandlers;
    this.assertUnique(
      existing,
      (handler) => handler.entrySpotType === registration.entrySpotType &&
        handler.actorType === registration.actorType &&
        handler.packetName === registration.packetName,
      `Duplicate Entry Spot actor handler '${registration.entrySpotType.name}:${registration.actorType.name}:${registration.packetName}'.`
    );
    if (kind === 'send') {
      node.entrySpotActorSendHandlers = [...(node.entrySpotActorSendHandlers ?? []), registration];
    } else {
      node.entrySpotActorRequestHandlers = [...(node.entrySpotActorRequestHandlers ?? []), registration];
    }
  }

  addSpotActor(
    node: MutableSpotNode,
    kind: 'send' | 'request',
    registration: ZLinkSpotActorSendHandlerRegistration | ZLinkSpotActorRequestHandlerRegistration
  ): void {
    const existing = kind === 'send' ? node.spotActorSendHandlers : node.spotActorRequestHandlers;
    this.assertUnique(
      existing,
      (handler) => handler.spotType === registration.spotType &&
        handler.actorType === registration.actorType &&
        handler.packetName === registration.packetName,
      `Duplicate SPOT actor handler '${registration.spotType.name}:${registration.actorType.name}:${registration.packetName}'.`
    );
    if (kind === 'send') {
      node.spotActorSendHandlers = [...(node.spotActorSendHandlers ?? []), registration];
    } else {
      node.spotActorRequestHandlers = [...(node.spotActorRequestHandlers ?? []), registration];
    }
  }

  private requireTargets(predicate: (node: MutableSpotNode) => boolean, missingMessage: string): readonly MutableSpotNode[] {
    const matches = Object.values(this.nodes).filter(predicate);
    if (matches.length === 0) {
      throw new framework.ZLinkConfigurationException(missingMessage);
    }
    return matches;
  }

  private assertUnique<T>(
    existing: readonly T[] | undefined,
    isDuplicate: (registration: T) => boolean,
    duplicateMessage: string
  ): void {
    if ((existing ?? []).some(isDuplicate)) {
      throw new framework.ZLinkConfigurationException(duplicateMessage);
    }
  }
}
