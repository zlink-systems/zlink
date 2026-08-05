import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  Type,
  ZLinkActor,
  ZLinkActorHandlerRegistry,
  ZLinkSpot,
  ZLinkSpotActorJoinResult,
  ZLinkMessageContext,
  ZLinkSpotActorRequestHandler,
  ZLinkSpotActorSendHandler
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import {
  ZLinkFrameworkException,
  ZLinkMessageMetadataEmpty
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkConfigurationException } from '../configuration';
import { readZLinkDecoratorMetadata } from '../../contracts/Handlers/Attributes';
import { wrapFrameworkPayloadMessage } from '../messaging/payload-codec';
import type { ZLinkMessageSerializer } from '../../contracts';
import { actorJoinIdentity } from './actor-lifecycle-snapshot';
import { runActorHandlerWithDeferredJoins } from './actor-join-deferred-scope';
import { runWithLifecycleHandler } from '../handlers/handler-instance-scope';
import type { ZLinkSerialWorkOptions } from '../execution/serial-scheduler';

export enum ZLinkActorPacketKind {
  Send = 'send',
  Request = 'request'
}

export interface ZLinkActorPacketDescriptor {
  readonly kind: ZLinkActorPacketKind;
  readonly packetName: string;
  readonly actorType: Type<ZLinkActor>;
  readonly handlerType: Type;
}

export class ZLinkSpotActorHandlerRegistryRuntime implements ZLinkActorHandlerRegistry {
  private readonly packets = new Map<string, ZLinkActorPacketDescriptor>();

  addHandler<THandler>(handlerType: Type<THandler>, packetName?: string): this {
    const metadata = readZLinkDecoratorMetadata(handlerType).find((entry) =>
      entry.kind === 'spotActorSend' || entry.kind === 'spotActorRequest');
    const resolvedPacketName = packetName ?? metadata?.packetName;
    if (metadata === undefined || resolvedPacketName === undefined) {
      throw new ZLinkConfigurationException(
        `Actor packet handler '${handlerType.name}' must declare a packet name.`
      );
    }
    return this.addPacket({
      kind: metadata.kind === 'spotActorSend'
        ? ZLinkActorPacketKind.Send
        : ZLinkActorPacketKind.Request,
      packetName: resolvedPacketName,
      actorType: Object as unknown as Type<ZLinkActor>,
      handlerType
    });
  }

  addPacket(descriptor: ZLinkActorPacketDescriptor): this {
    const key = packetKey(descriptor.kind, descriptor.actorType, descriptor.packetName);
    if (this.packets.has(key)) {
      throw new ZLinkConfigurationException(
        `Actor packet '${descriptor.packetName}' for '${descriptor.actorType.name}' is already registered.`
      );
    }
    this.packets.set(key, descriptor);
    return this;
  }

  resolvePacket(
    kind: ZLinkActorPacketKind,
    actor: ZLinkActor,
    packetName: string
  ): ZLinkActorPacketDescriptor | undefined {
    const actorType = actor.constructor as Type<ZLinkActor>;
    const exact = this.packets.get(packetKey(kind, actorType, packetName));
    if (exact !== undefined) {
      return exact;
    }
    const wildcard = this.packets.get(packetKey(kind, Object as unknown as Type<ZLinkActor>, packetName));
    if (wildcard !== undefined) {
      return wildcard;
    }

    for (const descriptor of this.packets.values()) {
      if (
        descriptor.kind === kind
        && descriptor.packetName === packetName
        && actor instanceof descriptor.actorType
      ) {
        return descriptor;
      }
    }
    return undefined;
  }

}

export interface ZLinkSpotActorDispatcherOptions {
  readonly registry: ZLinkSpotActorHandlerRegistryRuntime;
  readonly spot: ZLinkSpot;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly serial?: {
    execute<T>(operation: () => Promise<T> | T, workOptions?: ZLinkSerialWorkOptions): Promise<T>;
  };
  readonly serialWorkOptions?: ZLinkSerialWorkOptions;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
}

interface ZLinkSpotActorReplyOptionsSnapshot {
  readonly metadata: ReadonlyMap<string, string>;
  readonly compressPayload: boolean;
}

export class ZLinkSpotActorDispatcher {
  constructor(private readonly options: ZLinkSpotActorDispatcherOptions) {}

  dispatchSend<TMessage>(
    actor: ZLinkActor,
    packetName: string,
    message: TMessage,
    context: Partial<ZLinkMessageContext> = {}
  ): Promise<void> {
    return this.dispatchSendDecoded(actor, packetName, () => message, context);
  }

  dispatchSendDecoded<TMessage>(
    actor: ZLinkActor,
    packetName: string,
    decode: () => TMessage,
    context: Partial<ZLinkMessageContext> = {}
  ): Promise<void> {
    return this.execute(async () => {
      const descriptor = this.requirePacket(ZLinkActorPacketKind.Send, actor, packetName);
      const message = decode();
      await this.invokeHandler<
        ZLinkSpotActorSendHandler<ZLinkSpot, ZLinkActor, TMessage>,
        void
      >(actor, descriptor, (handler) =>
        runActorHandlerWithDeferredJoins(() =>
          handler.handle(
            this.options.spot,
            actor,
            this.createContext(packetName, context),
            message
          )));
    });
  }

  dispatchRequest<TRequest, TReply>(
    actor: ZLinkActor,
    packetName: string,
    request: TRequest,
    context: Partial<ZLinkMessageContext> = {}
  ): Promise<TReply> {
    return this.dispatchRequestThenDecoded<TRequest, TReply, TReply>(
      actor,
      packetName,
      () => request,
      context,
      (reply) => reply
    );
  }

  dispatchRequestDecoded<TRequest, TReply>(
    actor: ZLinkActor,
    packetName: string,
    decode: () => TRequest,
    context: Partial<ZLinkMessageContext> = {}
  ): Promise<TReply> {
    return this.dispatchRequestThenDecoded<TRequest, TReply, TReply>(
      actor,
      packetName,
      decode,
      context,
      (reply) => reply
    );
  }

  dispatchRequestThen<TRequest, TReply, TResult>(
    actor: ZLinkActor,
    packetName: string,
    request: TRequest,
    context: Partial<ZLinkMessageContext> = {},
    afterReply: (reply: TReply, options: ZLinkSpotActorReplyOptionsSnapshot) => Promise<TResult> | TResult,
    beforeReply?: (reply: TReply, options: ZLinkSpotActorReplyOptionsSnapshot) => Promise<void> | void
  ): Promise<TResult> {
    return this.dispatchRequestThenDecoded(
      actor,
      packetName,
      () => request,
      context,
      afterReply,
      beforeReply
    );
  }

  dispatchRequestThenDecoded<TRequest, TReply, TResult>(
    actor: ZLinkActor,
    packetName: string,
    decode: () => TRequest,
    context: Partial<ZLinkMessageContext> = {},
    afterReply: (reply: TReply, options: ZLinkSpotActorReplyOptionsSnapshot) => Promise<TResult> | TResult,
    beforeReply?: (reply: TReply, options: ZLinkSpotActorReplyOptionsSnapshot) => Promise<void> | void
  ): Promise<TResult> {
    return this.execute(async () => {
      const descriptor = this.requirePacket(ZLinkActorPacketKind.Request, actor, packetName);
      const request = decode();
      return await this.invokeHandler<
        ZLinkSpotActorRequestHandler<ZLinkSpot, ZLinkActor, TRequest, TReply>,
        TResult
      >(actor, descriptor, (handler) =>
        runActorHandlerWithDeferredJoins(
          () => handler.handle(
            this.options.spot,
            actor,
            this.createContext(packetName, context),
            request
          ),
          (reply) => afterReply(reply, {
            metadata: new Map<string, string>(),
            compressPayload: false
          }),
          beforeReply === undefined
            ? undefined
            : (reply) => beforeReply(reply, {
                metadata: new Map<string, string>(),
                compressPayload: false
              })
        ));
    });
  }

  admitActorJoin(
    actor: ZLinkActor,
    request: Message,
    commit: () => Promise<void> | void
  ): Promise<ZLinkSpotActorJoinResult> {
    return this.evaluateActorJoin(actor, request).then(async (result) => {
      if (result.accepted) {
        await this.commitActorJoin(actor, commit);
      }
      return result;
    });
  }

  evaluateActorJoin(
    actor: ZLinkActor,
    request: Message
  ): Promise<ZLinkSpotActorJoinResult> {
    return this.execute(async () => {
      const payload = wrapFrameworkPayloadMessage(request, this.options.messageSerializers);
      const onActorJoin = (this.options.spot as Partial<ZLinkSpot>).onActorJoin;
      return onActorJoin === undefined
        ? { accepted: false }
        : await onActorJoin.call(this.options.spot, actorJoinIdentity(actor), payload);
    });
  }

  commitActorJoin(
    actor: ZLinkActor,
    commit: () => Promise<void> | void
  ): Promise<void> {
    return this.execute(async () => {
      await commit();
      await this.options.spot.onJoinedActor(actor);
    });
  }

  notifyJoinActor(actor: ZLinkActor): Promise<void> {
    return this.execute(() => this.options.spot.onJoinedActor(actor));
  }

  notifyLeaveActor(actor: ZLinkActor): Promise<void> {
    return this.execute(() => this.options.spot.onLeaveActor(actor));
  }

  notifyDisconnectActor(actor: ZLinkActor): Promise<void> {
    return this.execute(() => this.options.spot.onDisconnectActor?.(actor));
  }

  private requirePacket(
    kind: ZLinkActorPacketKind,
    actor: ZLinkActor,
    packetName: string
  ): ZLinkActorPacketDescriptor {
    const descriptor = this.options.registry.resolvePacket(kind, actor, packetName);
    if (descriptor === undefined) {
      throw actorDispatchHandlerNotFound(`No Spot actor ${kind} handler is registered for '${packetName}'.`);
    }
    this.ensureActorType(descriptor, actor);
    return descriptor;
  }

  private ensureActorType(descriptor: ZLinkActorPacketDescriptor, actor: ZLinkActor): void {
    if (descriptor.actorType === (Object as unknown as Type<ZLinkActor>)) {
      return;
    }
    if (actor instanceof descriptor.actorType) {
      return;
    }
    throw actorDispatchHandlerNotFound(
      `Actor handler '${descriptor.handlerType.name}' expects actor '${descriptor.actorType.name}'.`
    );
  }

  private async invokeHandler<THandler, TResult>(
    actor: ZLinkActor,
    descriptor: Pick<ZLinkActorPacketDescriptor, 'handlerType'>,
    callback: (handler: THandler) => Promise<TResult>
  ): Promise<TResult> {
    return await runWithLifecycleHandler(
      actor,
      descriptor.handlerType,
      this.options.providerResolver,
      (resolved) => callback(resolved as THandler)
    );
  }

  private execute<T>(operation: () => Promise<T> | T): Promise<T> {
    return this.options.serial?.execute(operation, this.options.serialWorkOptions)
      ?? Promise.resolve().then(operation);
  }

  private createContext(
    packetName: string,
    context: Partial<ZLinkMessageContext>
  ): ZLinkMessageContext {
    return {
      meshName: context.meshName,
      channelName: context.channelName,
      packetName,
      contentType: context.contentType,
      metadata: context.metadata ?? ZLinkMessageMetadataEmpty,
      correlationId: context.correlationId
    };
  }
}

function packetKey(kind: ZLinkActorPacketKind, actorType: Type<ZLinkActor>, packetName: string): string {
  return `${kind}:${actorType.name}:${packetName}`;
}

function actorDispatchHandlerNotFound(message: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorDispatchHandlerNotFound,
    message
  );
}
