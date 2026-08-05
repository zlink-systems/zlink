import { Injectable } from '@nestjs/common';
import type { InjectionToken } from '@nestjs/common';
import type {
  Type,
  ZLinkActor,
  ZLinkEntrySpot,
  ZLinkInstanceSpot,
  ZLinkSpot
} from '@zlink-systems/framework';
import type {
  ZLinkNestEntrySpotActorRequestHandlerOptions,
  ZLinkNestEntrySpotActorSendHandlerOptions,
  ZLinkNestEntrySpotPacketHandlerOptions,
  ZLinkNestEntrySpotSubscriptionHandlerOptions,
  ZLinkNestHandlerOptions,
  ZLinkNestSpotActorRequestHandlerOptions,
  ZLinkNestSpotActorSendHandlerOptions,
  ZLinkNestSpotPacketHandlerOptions,
  ZLinkNestSpotSubscriptionHandlerOptions,
  ZLinkNestSpotTimerHandlerOptions
} from './contracts';
import { framework } from './framework-loader';
import {
  appendNestHandlerMetadata,
  appendNestSpotActorHandlerMetadata,
  appendNestSpotHandlerMetadata,
  appendNestSpotTimerHandlerMetadata,
  markNestSpotTimerHandler,
  type ZLinkNestHandlerKind
} from './handler-metadata';


export function zlinkRequestHandler(
  groupName: string,
  packetName?: string,
  options: ZLinkNestHandlerOptions = {}
): ClassDecorator {
  return zlinkHandler(groupName, 'request', packetName, options);
}

export function zlinkSendHandler(
  groupName: string,
  packetName?: string,
  options: ZLinkNestHandlerOptions = {}
): ClassDecorator {
  return zlinkHandler(groupName, 'send', packetName, options);
}

export function zlinkPublishHandler(
  groupName: string,
  packetName?: string,
  options: ZLinkNestHandlerOptions = {}
): ClassDecorator {
  return zlinkHandler(groupName, 'publish', packetName, options);
}

export function zlinkSpotActorRequestHandler<TSpot extends ZLinkSpot, TActor extends ZLinkActor>(
  options: ZLinkNestSpotActorRequestHandlerOptions<TSpot, TActor>
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestSpotActorHandlerMetadata(target as Type, {
      actor: options.actor,
      handlerType: target as Type,
      kind: 'spotActorRequest',
      methodName: options.methodName ?? 'handle',
      packetName: options.packetName,
      spot: options.spot
    });
  };
}

export function zlinkSpotPacketHandler<TSpot extends ZLinkSpot | ZLinkInstanceSpot>(
  options: ZLinkNestSpotPacketHandlerOptions<TSpot>
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestSpotHandlerMetadata(target as Type, {
      handlerType: target as Type,
      kind: 'spotPacket',
      packetName: options.packetName,
      spot: options.spot
    });
  };
}

export function zlinkEntrySpotPacketHandler<TEntrySpot extends ZLinkEntrySpot>(
  options: ZLinkNestEntrySpotPacketHandlerOptions<TEntrySpot>
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestSpotHandlerMetadata(target as Type, {
      entrySpot: options.entrySpot,
      handlerType: target as Type,
      kind: 'entrySpotPacket',
      packetName: options.packetName
    });
  };
}

export function zlinkSpotSubscriptionHandler<TSpot extends ZLinkSpot>(
  options: ZLinkNestSpotSubscriptionHandlerOptions<TSpot>
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestSpotHandlerMetadata(target as Type, {
      handlerType: target as Type,
      kind: 'spotSubscription',
      spot: options.spot,
      channelName: options.channelName,
      topic: options.topic
    });
  };
}

export function zlinkEntrySpotSubscriptionHandler<TEntrySpot extends ZLinkEntrySpot>(
  options: ZLinkNestEntrySpotSubscriptionHandlerOptions<TEntrySpot>
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestSpotHandlerMetadata(target as Type, {
      entrySpot: options.entrySpot,
      handlerType: target as Type,
      kind: 'entrySpotSubscription',
      channelName: options.channelName,
      topic: options.topic
    });
  };
}

export function zlinkSpotActorSendHandler<TSpot extends ZLinkSpot, TActor extends ZLinkActor>(
  options: ZLinkNestSpotActorSendHandlerOptions<TSpot, TActor>
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestSpotActorHandlerMetadata(target as Type, {
      actor: options.actor,
      handlerType: target as Type,
      kind: 'spotActorSend',
      methodName: options.methodName ?? 'handle',
      packetName: options.packetName,
      spot: options.spot
    });
  };
}

export function zlinkEntrySpotActorRequestHandler<TEntrySpot extends ZLinkEntrySpot, TActor extends ZLinkActor>(
  options: ZLinkNestEntrySpotActorRequestHandlerOptions<TEntrySpot, TActor>
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestSpotActorHandlerMetadata(target as Type, {
      actor: options.actor,
      entrySpot: options.entrySpot,
      handlerType: target as Type,
      kind: 'entrySpotActorRequest',
      methodName: options.methodName ?? 'handle',
      packetName: options.packetName
    });
  };
}

export function zlinkEntrySpotActorSendHandler<TEntrySpot extends ZLinkEntrySpot, TActor extends ZLinkActor>(
  options: ZLinkNestEntrySpotActorSendHandlerOptions<TEntrySpot, TActor>
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestSpotActorHandlerMetadata(target as Type, {
      actor: options.actor,
      entrySpot: options.entrySpot,
      handlerType: target as Type,
      kind: 'entrySpotActorSend',
      methodName: options.methodName ?? 'handle',
      packetName: options.packetName
    });
  };
}

export function zlinkSpotTimerHandler<TSpot extends ZLinkSpot = ZLinkSpot>(
  options: ZLinkNestSpotTimerHandlerOptions<TSpot> = {}
): ClassDecorator {
  return (target: Function) => {
    Injectable()(target as Type);
    markNestSpotTimerHandler(target as Type);
    if (options.name !== undefined && options.periodMs !== undefined) {
      appendNestSpotTimerHandlerMetadata(target as Type, {
        entrySpot: options.entrySpot,
        handlerType: target as Type,
        name: options.name,
        options: options.options,
        periodMs: options.periodMs,
        spot: options.spot
      });
    }
  };
}

export function zlinkHandler(
  groupName: string,
  kind: ZLinkNestHandlerKind,
  packetName?: string,
  options: ZLinkNestHandlerOptions = {}
): ClassDecorator {
  validateHandlerGroupName(groupName);
  return (target: Function) => {
    Injectable()(target as Type);
    appendNestHandlerMetadata(target as Type, {
      decodePayload: options.decodePayload,
      encodeResult: options.encodeResult,
      groupName,
      kind,
      methodName: options.methodName ?? 'handle',
      packetName: packetName ?? inferPacketName(target as Type, target as Type)
    });
  };
}

function validateHandlerGroupName(groupName: string): void {
  if (groupName.trim() === '') {
    throw new framework.ZLinkConfigurationException('ZLink handler group name must not be empty.');
  }
}

function inferPacketName(handlerType: Type | undefined, handlerToken: InjectionToken): string {
  if (handlerType !== undefined) {
    return handlerType.name.endsWith('Handler')
      ? handlerType.name.slice(0, -'Handler'.length)
      : handlerType.name;
  }
  const tokenName = typeof handlerToken === 'symbol'
    ? handlerToken.description
    : String(handlerToken);
  if (tokenName === undefined || tokenName.trim() === '') {
    throw new framework.ZLinkConfigurationException('ZLink handler packetName is required for anonymous provider tokens.');
  }
  return tokenName;
}
