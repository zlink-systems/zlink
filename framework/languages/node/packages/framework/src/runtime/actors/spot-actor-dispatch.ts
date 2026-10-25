import { ZlinkStreamContentType } from '@zlink-systems/stream-wire';
import {
  type Type,
  type ZLinkActor,
  type ZLinkActorHandlerRegistry,
  type ZLinkMessageTypeResolver,
  type ZLinkMessageContext,
  type ZLinkMessageSerializer,
  type ZLinkSpot,
  type ZLinkSpotActorJoinResult,
  type ZLinkSpotActorRequestHandler,
  type ZLinkSpotActorSendHandler,
  ZLinkFrameworkException,
  ZLinkMessageMetadataEmpty
} from '../../contracts';

import type { Message } from '../../contracts/Common/Message';
import {
  resolveHandlerMessageType,
  resolveMessageTypeForDecode
} from '../../contracts/Common/HandlerMessageType';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { readZLinkDecoratorMetadata } from '../../contracts/Handlers/Attributes';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkSerialWorkOptions } from '../execution/serial-execution-queue';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { runWithLifecycleHandler } from '../handlers/handler-instance-scope';
import { wrapFrameworkPayloadMessage } from '../messaging/payload-codec';
import { runActorHandlerWithDeferredJoins } from './actor-join-deferred-scope';
import { actorJoinIdentity } from './actor-lifecycle-snapshot';

export enum ZLinkActorPacketKind {
  Send = 'send',
  Request = 'request'
}

export interface ZLinkActorPacketDescriptor {
  readonly kind: ZLinkActorPacketKind;
  readonly packetName: string;
  readonly actorType: Type<ZLinkActor>;
  readonly handlerType: Type;
  readonly messageType?: ZLinkMessageTypeResolver;
  readonly methodName?: string;
}

export class ZLinkSpotActorHandlerRegistryRuntime implements ZLinkActorHandlerRegistry {
  private readonly packets = new Map<
    ZLinkActorPacketKind,
    Map<string, Map<string, ZLinkActorPacketDescriptor>>
  >();

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
    const messageType = resolveHandlerMessageType(handlerType);
    return this.addPacket({
      kind:
        metadata.kind === 'spotActorSend'
          ? ZLinkActorPacketKind.Send
          : ZLinkActorPacketKind.Request,
      packetName: resolvedPacketName,
      actorType: Object as unknown as Type<ZLinkActor>,
      handlerType,
      ...(messageType === undefined ? {} : { messageType }),
      methodName: metadata.methodName
    });
  }

  addPacket(descriptor: ZLinkActorPacketDescriptor): this {
    const actorTypeName = descriptor.actorType.name;
    let packets = this.packets.get(descriptor.kind);
    let actors = packets?.get(descriptor.packetName);
    if (actors?.has(actorTypeName) === true) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `Actor packet '${descriptor.packetName}' for '${descriptor.actorType.name}' is already registered.`
      );
    }
    const metadata = readZLinkDecoratorMetadata(descriptor.handlerType).find(
      (entry) =>
        entry.kind ===
          (descriptor.kind === ZLinkActorPacketKind.Send ? 'spotActorSend' : 'spotActorRequest') &&
        entry.packetName === descriptor.packetName
    );
    if (packets === undefined) {
      packets = new Map();
      this.packets.set(descriptor.kind, packets);
    }
    if (actors === undefined) {
      actors = new Map();
      packets.set(descriptor.packetName, actors);
    }
    actors.set(actorTypeName, {
      ...descriptor,
      methodName: descriptor.methodName ?? metadata?.methodName
    });
    return this;
  }

  resolvePacket(
    kind: ZLinkActorPacketKind,
    actor: ZLinkActor,
    packetName: string
  ): ZLinkActorPacketDescriptor | undefined {
    const actors = this.packets.get(kind)?.get(packetName);
    if (actors === undefined) return undefined;
    const actorType = actor.constructor as Type<ZLinkActor>;
    const exact = actors.get(actorType.name);
    if (exact !== undefined) {
      return exact;
    }
    const wildcard = actors.get(Object.name);
    if (wildcard !== undefined) {
      return wildcard;
    }

    for (const descriptor of actors.values()) {
      if (actor instanceof descriptor.actorType) {
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
  readonly onAdmitted?: () => void;
  readonly onHandlerStart?: () => void;
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
    context?: Partial<ZLinkMessageContext>
  ): Promise<void> {
    return this.dispatchSendDecoded(actor, packetName, () => message, context);
  }

  dispatchSendDecoded<TMessage>(
    actor: ZLinkActor,
    packetName: string,
    decode: (messageType?: Type) => TMessage,
    context?: Partial<ZLinkMessageContext>
  ): Promise<void> {
    return this.execute(async () => {
      const descriptor = this.requirePacket(ZLinkActorPacketKind.Send, actor, packetName);
      const message = decode(resolveMessageTypeForDecode(descriptor.messageType));
      this.options.onHandlerStart?.();
      await this.invokeHandler<ZLinkSpotActorSendHandler<ZLinkSpot, ZLinkActor, TMessage>, void>(
        actor,
        descriptor,
        (handler, method) =>
          runActorHandlerWithDeferredJoins(() =>
            method.call(
              handler,
              this.options.spot,
              actor,
              this.createContext(packetName, context),
              message
            )
          )
      );
    });
  }

  dispatchRequest<TRequest, TReply>(
    actor: ZLinkActor,
    packetName: string,
    request: TRequest,
    context?: Partial<ZLinkMessageContext>
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
    decode: (messageType?: Type) => TRequest,
    context?: Partial<ZLinkMessageContext>
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
    context: Partial<ZLinkMessageContext> | undefined,
    afterReply: (
      reply: TReply,
      options: ZLinkSpotActorReplyOptionsSnapshot
    ) => Promise<TResult> | TResult,
    beforeReply?: (
      reply: TReply,
      options: ZLinkSpotActorReplyOptionsSnapshot
    ) => Promise<void> | void
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
    decode: (messageType?: Type) => TRequest,
    context: Partial<ZLinkMessageContext> | undefined,
    afterReply: (
      reply: TReply,
      options: ZLinkSpotActorReplyOptionsSnapshot
    ) => Promise<TResult> | TResult,
    beforeReply?: (
      reply: TReply,
      options: ZLinkSpotActorReplyOptionsSnapshot
    ) => Promise<void> | void
  ): Promise<TResult> {
    return this.execute(async () => {
      const descriptor = this.requirePacket(ZLinkActorPacketKind.Request, actor, packetName);
      const request = decode(resolveMessageTypeForDecode(descriptor.messageType));
      this.options.onHandlerStart?.();
      return await this.invokeHandler<
        ZLinkSpotActorRequestHandler<ZLinkSpot, ZLinkActor, TRequest, TReply>,
        TResult
      >(actor, descriptor, (handler, method) =>
        runActorHandlerWithDeferredJoins(
          () =>
            method.call(
              handler,
              this.options.spot,
              actor,
              this.createContext(packetName, context),
              request
            ),
          (reply) =>
            afterReply(reply, {
              metadata: ZLinkMessageMetadataEmpty.values,
              compressPayload: false
            }),
          beforeReply === undefined
            ? undefined
            : (reply) =>
                beforeReply(reply, {
                  metadata: ZLinkMessageMetadataEmpty.values,
                  compressPayload: false
                })
        )
      );
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
    request: Message,
    contentType: string = ZlinkStreamContentType.Json
  ): Promise<ZLinkSpotActorJoinResult> {
    return this.execute(async () => {
      const payload = wrapFrameworkPayloadMessage(
        request,
        this.options.messageSerializers,
        contentType
      );
      const onActorJoin = (this.options.spot as Partial<ZLinkSpot>).onActorJoin;
      return onActorJoin === undefined
        ? { accepted: false }
        : await onActorJoin.call(this.options.spot, actorJoinIdentity(actor), payload);
    });
  }

  commitActorJoin(actor: ZLinkActor, commit: () => Promise<void> | void): Promise<void> {
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
      throw actorDispatchHandlerNotFound(
        `No Spot actor ${kind} handler is registered for '${packetName}'.`
      );
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

  private invokeHandler<THandler, TResult>(
    actor: ZLinkActor,
    descriptor: Pick<ZLinkActorPacketDescriptor, 'handlerType' | 'methodName'>,
    callback: (
      handler: THandler,
      method: THandler extends { handle: infer TMethod } ? TMethod : never
    ) => Promise<TResult>
  ): Promise<TResult> {
    return runWithLifecycleHandler(
      actor,
      descriptor.handlerType,
      this.options.providerResolver,
      (resolved) => {
        const methodName = descriptor.methodName ?? 'handle';
        const method = (resolved as Record<string, unknown>)[methodName];
        if (typeof method !== 'function') {
          throw new ZLinkConfigurationException(
            `Actor handler '${descriptor.handlerType.name}.${methodName}' is not callable.`
          );
        }
        return callback(
          resolved as THandler,
          method as THandler extends { handle: infer TMethod } ? TMethod : never
        );
      }
    );
  }

  private execute<T>(operation: () => Promise<T> | T): Promise<T> {
    const pending =
      this.options.serial?.execute(operation, this.options.serialWorkOptions) ??
      Promise.resolve().then(operation);
    this.options.onAdmitted?.();
    return pending;
  }

  private createContext(
    packetName: string,
    context: Partial<ZLinkMessageContext> | undefined
  ): ZLinkMessageContext {
    return {
      meshName: context?.meshName,
      channelName: context?.channelName,
      packetName,
      contentType: context?.contentType,
      metadata: context?.metadata ?? ZLinkMessageMetadataEmpty,
      correlationId: context?.correlationId
    };
  }
}

function actorDispatchHandlerNotFound(message: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorDispatchHandlerNotFound,
    message
  );
}
