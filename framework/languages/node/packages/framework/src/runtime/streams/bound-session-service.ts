import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { ZLinkBufferMessage as ZLinkBindingMessage } from '../backend/runtime-message';
import type { ActorRef } from '../../contracts';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import type { Message } from '../../contracts/Common/Message';
import { throwIfAborted } from '../abort';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendActorSessionNode
} from '../backend/contracts';
import {
  encodeStreamHeader,
  resolvePacketName,
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind
} from './protocol';
import {
  ZLinkActorSessionBindingRegistry,
  type ZLinkActorSessionRoute
} from './actor-session-binding-registry';
import type {
  DefaultZLinkSessionActor,
  DefaultZLinkSessionContext
} from './session-context';
import {
  boundSessionErrorPayload
} from './bound-session-response-target';
import {
  ZLinkStreamFrameMessageFactory
} from './stream-frame-factory';
import {
  ZLinkManagedStream
} from './managed-stream';
import { ZLinkMeshSubmitterRegistry } from '../messaging';

const ZLINK_SEND_DONT_WAIT = 1;
const LEGACY_BOUND_SESSION_SEND_TIMEOUT_MS = 1000;
const LEGACY_BOUND_SESSION_SEND_CAPACITY = 4096;

type ZLinkStreamActorSessionRoute = ZLinkActorSessionRoute<DefaultZLinkSessionContext, DefaultZLinkSessionActor>;

export interface ZLinkBoundSessionTransport {
  send(actorId: string, message: unknown, options: ZLinkBoundSessionSendOptions): Promise<ZLinkSubmitResult>;
  disconnect(actorId: string, options: ZLinkBoundSessionDisconnectOptions): Promise<void>;
}

export interface ZLinkBoundSessionSendOptions {
  readonly bindingToken: string;
  readonly packetName?: string;
  readonly metadata: ReadonlyMap<string, string>;
  readonly signal?: AbortSignal;
}

export interface ZLinkBoundSessionDisconnectOptions {
  readonly bindingToken: string;
  readonly signal?: AbortSignal;
}

export interface ZLinkBoundSessionServiceOptions {
  readonly transport?: ZLinkBoundSessionTransport;
  readonly actorBindTimeoutMs?: number;
  readonly meshSubmitters?: ZLinkMeshSubmitterRegistry;
  readonly sendTimeoutMs?: number;
  readonly sendHighWaterMark?: number;
  readonly nativeActorMeshNameProvider?: () => string | undefined;
}

export class ZLinkBoundSessionService {
  private readonly meshSubmitters: ZLinkMeshSubmitterRegistry;

  constructor(
    private readonly routes: ZLinkActorSessionBindingRegistry<DefaultZLinkSessionContext, DefaultZLinkSessionActor>,
    private readonly frameMessages: ZLinkStreamFrameMessageFactory,
    private readonly options: ZLinkBoundSessionServiceOptions = {}
  ) {
    this.meshSubmitters = options.meshSubmitters ?? new ZLinkMeshSubmitterRegistry(
      options.sendTimeoutMs ?? LEGACY_BOUND_SESSION_SEND_TIMEOUT_MS,
      Math.max(1, options.sendHighWaterMark ?? LEGACY_BOUND_SESSION_SEND_CAPACITY)
    );
  }

  async sendBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
    const route = this.routes.requireRoute(actorId);
    const frame = this.frameMessages.createJsonFrameMessage(
      ZLinkStreamMessageKind.Send,
      resolvePacketName(message, packetName),
      metadata,
      false,
      undefined,
      message
    );
    try {
      const result = await this.requireTransport().send(actorId, frame, {
        bindingToken: route.bindingToken,
        packetName,
        metadata,
        signal
      });
      this.routes.requireCurrentToken(actorId, route.bindingToken);
      return result;
    } finally {
      frame.close();
    }
  }

  async submitLocalBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    const route = this.routes.route(actorId);
    if (route === undefined) {
      return { status: ZLinkSubmitStatus.TargetNotFound };
    }
    const frame = this.frameMessages.createJsonFrameMessage(
      ZLinkStreamMessageKind.Send,
      resolvePacketName(message, packetName),
      metadata,
      false,
      undefined,
      message
    );
    try {
      throwIfAborted(signal);
      if (this.routes.route(actorId)?.bindingToken !== route.bindingToken) {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      }
      return await route.context.stream.submitRaw(frame, signal);
    } finally {
      frame.close();
    }
  }

  sendLocalBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>
  ): boolean {
    const route = this.routes.route(actorId);
    if (route === undefined) {
      return false;
    }
    return this.writeLocalBoundSessionFrame(
      actorId,
      route,
      ZLinkStreamMessageKind.Send,
      resolvePacketName(message, packetName),
      metadata,
      false,
      undefined,
      message,
      'send'
    );
  }

  sendLocalBoundSessionResponse(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean
  ): boolean {
    const route = this.routes.route(actorId);
    if (route === undefined) {
      return false;
    }
    return this.writeLocalBoundSessionFrame(
      actorId,
      route,
      ZLinkStreamMessageKind.Response,
      packetName,
      metadata,
      compressPayload,
      requestSeq,
      message,
      'response'
    );
  }

  sendLocalBoundSessionError(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): boolean {
    const route = this.routes.route(actorId);
    if (route === undefined) {
      return false;
    }
    return this.writeLocalBoundSessionFrame(
      actorId,
      route,
      ZLinkStreamMessageKind.Error,
      packetName,
      metadata,
      false,
      requestSeq,
      boundSessionErrorPayload(error),
      'error response'
    );
  }

  async sendNativeBoundSession(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
    return await this.submitNativeBoundSessionPayload(
      node,
      actorRef,
      ZLinkStreamMessageKind.Send,
      resolvePacketName(message, packetName),
      metadata,
      false,
      undefined,
      message,
      signal
    );
  }

  async sendNativeBoundSessionResponse(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    await this.sendNativeBoundSessionPayload(
      node,
      actorRef,
      ZLinkStreamMessageKind.Response,
      packetName,
      metadata,
      compressPayload,
      requestSeq,
      message,
      signal
    );
  }

  async sendNativeBoundSessionError(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    await this.sendNativeBoundSessionPayload(
      node,
      actorRef,
      ZLinkStreamMessageKind.Error,
      packetName,
      metadata,
      false,
      requestSeq,
      boundSessionErrorPayload(error),
      signal
    );
  }

  async disconnectNativeBoundSession(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    await node.closeActorBoundSession(
      actorRef as unknown as ZLinkBackendActorRef,
      requireBoundSessionGeneration(actorRef),
      0,
      signal
    );
  }

  async disconnectBoundSession(actorId: string, signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    const route = this.routes.requireRoute(actorId);
    try {
      await this.requireTransport().disconnect(actorId, {
        bindingToken: route.bindingToken,
        signal
      });
    } finally {
      this.routes.unbind(actorId, route.context, route.bindingToken);
    }
  }

  relayRemoteBoundSessionBind(stream: ZLinkManagedStream, actorRef: ActorRef): void {
    const header = ZLinkBindingMessage.from(Buffer.from(encodeStreamHeader({
      kind: ZLinkStreamMessageKind.Send,
      codec: ZLinkStreamCodec.Raw,
      flags: ZLinkStreamHeaderFlags.None,
      name: 'zlink.framework.actor.bound_session.bind',
      metadata: new Map()
    })));
    const body = ZLinkBindingMessage.from(Buffer.alloc(0));
    try {
      if (!stream.sendBoundActor(actorRef.actorId, [header, body], 0)) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
          `Actor '${actorRef.actorId}' remote bound session bind relay failed.`
        );
      }
    } finally {
      header.close();
      body.close();
    }
  }

  private writeLocalBoundSessionFrame(
    actorId: string,
    route: ZLinkStreamActorSessionRoute,
    kind: ZLinkStreamMessageKind,
    packetName: string,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean,
    requestSeq: bigint | undefined,
    payload: unknown,
    operationName: string
  ): boolean {
    const frame = this.frameMessages.createJsonFrameMessage(
      kind,
      packetName,
      metadata,
      compressPayload,
      requestSeq,
      payload
    );
    try {
      if (this.routes.route(actorId)?.bindingToken !== route.bindingToken) {
        return false;
      }
      if (!route.context.stream.writeRaw(frame)) {
        throw new Error(`Actor '${actorId}' local bound session ${operationName} failed.`);
      }
      return true;
    } finally {
      frame.close();
    }
  }

  private async sendNativeBoundSessionPayload(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    kind: ZLinkStreamMessageKind,
    packetName: string,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean,
    requestSeq: bigint | undefined,
    payload: unknown,
    signal?: AbortSignal
  ): Promise<void> {
    const result = await this.submitNativeBoundSessionPayload(
      node,
      actorRef,
      kind,
      packetName,
      metadata,
      compressPayload,
      requestSeq,
      payload,
      signal
    );
    if (result.status !== ZLinkSubmitStatus.Submitted) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RouteNotConnected,
        `Actor '${actorRef.actorId}' bound session route rejected '${result.status}'.`,
        result.status === ZLinkSubmitStatus.RouteNotConnected
      );
    }
  }

  private async submitNativeBoundSessionPayload(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    kind: ZLinkStreamMessageKind,
    packetName: string,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean,
    requestSeq: bigint | undefined,
    payload: unknown,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    const frame = this.frameMessages.createJsonFrameMessage(
      kind,
      packetName,
      metadata,
      compressPayload,
      requestSeq,
      payload
    );
    try {
      return await this.submitNativeBoundSessionFrame(node, actorRef, frame, signal);
    } finally {
      frame.close();
    }
  }

  private async submitNativeBoundSessionFrame(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    frame: Message,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    const backendActorRef = toBoundSessionSendActorRef(actorRef);
    const meshName = this.options.nativeActorMeshNameProvider?.() ?? '__native_bound_session';
    return await this.meshSubmitters.submit(meshName, () =>
      node.sendActorBoundSession(
          backendActorRef,
          requireBoundSessionGeneration(actorRef),
          [frame],
          ZLINK_SEND_DONT_WAIT
      ), signal);
  }

  private requireTransport(): ZLinkBoundSessionTransport {
    if (this.options.transport === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
        'Bound session transport is not started.',
        true
      );
    }
    return this.options.transport;
  }
}

function requireBoundSessionGeneration(actorRef: ActorRef): bigint {
  const generation = (actorRef as ActorRef & { bindingGeneration?: bigint }).bindingGeneration;
  if (generation === undefined || generation <= 0n) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
      `Actor '${actorRef.actorId}' has no current bound-session generation.`,
      true
    );
  }
  return generation;
}

function toBoundSessionSendActorRef(actor: ActorRef): ZLinkBackendActorRef {
  return {
    nodeRid: actor.nodeRid,
    actorId: actor.actorId,
    generation: actor.objectGeneration
  };
}
