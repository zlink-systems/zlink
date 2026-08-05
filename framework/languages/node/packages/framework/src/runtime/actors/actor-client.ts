import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException, internalFrameworkErrorKind  } from '../framework-errors-internal';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import {
  RequestResult,
  SubmitResult,
  isZLinkBackendResultError,
  ZLINK_BACKEND_SEND_NONE
} from '../backend/runtime-values';
import type {
  ActorRef,
  RoutingId,
  ZLinkActorClient,
  ZLinkActorRequestCall,
  ZLinkActorSendCall,
  ZLinkMessageSerializer
} from '../../contracts';
import { ZLinkSpotKind } from '../../contracts';
import {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} from '../../contracts';
import {
  requireOneWayCompletion,
  throwAlreadySubmitted,
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import type { Message } from '../../contracts/Common/Message';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendMeshNode,
  ZLinkMeshCompletionTable
} from '../backend';
import { closeMeshCompletion } from '../backend';
import { awaitWithAbort, throwIfAborted } from '../abort';
import { encodeFrameworkPayloadMessage, decodeFrameworkPayloadMessage } from '../messaging/payload-codec';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import {
  actorRequestDeadlineMetadata,
  decodeStreamHeader,
  encodeStreamHeader,
  tryDecodeStreamFrame,
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind
} from '../streams/protocol';
import type { ZLinkStoreLocationResolvers } from '../locations';
import type { ZLinkResolvedActorRoute } from '../locations';
import type { ZLinkActorRoutedJoinTransport } from './actor-routed-join-transport';
import { encodeRemoteActorPacketRelayPayload } from './actor-packet-relay-wire';
import { requestRoutedJsonReply } from './actor-routed-json-request';
import {
  attachActorMessageFollowContext,
  createInitialActorMessageFollowContext,
  createMessageFollowId,
  type ZLinkActorMessageFollowContext
} from './actor-message-follow-context';
import {
  captureZLinkSpotSerialTurn,
  requireZLinkYieldTurn,
  type ZLinkSpotSerialTurn
} from '../execution';
import { ZLinkConfigurationException } from '../configuration';
import { ZLinkMeshSubmitterRegistry } from '../messaging';

const LEGACY_ACTOR_SEND_TIMEOUT_MS = 1000;
const LEGACY_ACTOR_SEND_CAPACITY = 4096;

export interface ZLinkActorClientOptions {
  readonly nodeProvider: (meshName: string) => ZLinkBackendMeshNode | undefined;
  readonly completionTableProvider: (meshName: string) => ZLinkMeshCompletionTable | undefined;
  readonly locationResolver: () => ZLinkStoreLocationResolvers | undefined;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly defaultRequestTimeoutMs?: number;
  readonly staleActorRefReporter?: (meshName: string, actorId: string) => void;
  readonly staleActorRefPredicate?: (meshName: string, actor: ActorRef) => boolean;
  readonly handoffCapture?: (
    meshName: string,
    actorId: string,
    parts: readonly Message[],
    returnResponse: boolean,
    actor: ActorRef,
    deadlineUnixMs?: number
  ) => Promise<unknown> | undefined;
  readonly sendErrorReporter?: (error: unknown) => void;
  readonly meshSubmitters?: ZLinkMeshSubmitterRegistry;
  readonly routeTransport?: ZLinkActorRoutedJoinTransport;
  readonly transportDeliveryGate?: () => {
    waitBeforeSubmit(
      actorId: string,
      kind: 'oneWay' | 'request',
      signal?: AbortSignal
    ): Promise<number | void>;
  } | undefined;
  readonly sendTimeoutMs?: number;
  readonly sendHighWaterMark?: number;
}

export class DefaultZLinkActorClient implements ZLinkActorClient {
  private readonly meshSubmitters: ZLinkMeshSubmitterRegistry;

  constructor(private readonly options: ZLinkActorClientOptions) {
    this.meshSubmitters = options.meshSubmitters ?? new ZLinkMeshSubmitterRegistry(
      options.sendTimeoutMs ?? LEGACY_ACTOR_SEND_TIMEOUT_MS,
      Math.max(1, options.sendHighWaterMark ?? LEGACY_ACTOR_SEND_CAPACITY)
    );
  }

  sendToActor(actorId: string, message: unknown): ZLinkActorSendCall {
    requireActorId(actorId);
    return new DefaultZLinkActorSendCall(
      (packetName, metadata, signal) =>
        this.send(actorId, packetName, message, metadata, signal),
      message
    );
  }

  requestToActor(actorId: string, request: unknown): ZLinkActorRequestCall {
    requireActorId(actorId);
    return new DefaultZLinkActorRequestCall(
      (packetName, timeoutMs, metadata, signal) =>
        this.request(actorId, packetName, request, timeoutMs, metadata, signal),
      request,
      this.options.defaultRequestTimeoutMs
    );
  }

  private async send(
    actorId: string,
    explicitPacketName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
    const route = await this.resolveActorRoute(actorId, signal);
    const { meshName, actorRef: actor } = route;
    this.throwIfKnownStale(meshName, actor);
    const parts = this.createPacketParts(ZLinkStreamMessageKind.Send, undefined, explicitPacketName, message, metadata);
    const messageFollow = createActorMessageFollowContext(
      route,
      parts,
      false
    );
    const routedActor = attachActorMessageFollowContext(actor, messageFollow);
    try {
      const submissionCopies = await this.options.transportDeliveryGate?.()?.waitBeforeSubmit(
        actorId,
        'oneWay',
        signal
      ) ?? 1;
      const handoff = this.options.handoffCapture?.(
        meshName,
        actor.actorId,
        parts,
        false,
        routedActor
      );
      if (handoff !== undefined) {
        const submissions = [handoff];
        for (let copy = 1; copy < submissionCopies; copy++) {
          const duplicate = this.options.handoffCapture?.(
            meshName,
            actor.actorId,
            parts,
            false,
            routedActor
          );
          if (duplicate !== undefined) submissions.push(duplicate);
        }
        await awaitWithAbort(Promise.all(submissions), signal);
        return submitted();
      }
      const node = this.requireNode(meshName);
      if (
        this.options.routeTransport !== undefined
        && String(node.status().routingId) !== String(actor.nodeRid)
      ) {
        await this.options.routeTransport.sendToSpot(
          remoteActorRouteTarget(route),
          encodeRemoteActorPacketRelayPayload({
            actorId,
            header: parts[0].data(),
            payload: parts[1].data(),
            bindingActorRef: actor,
            actorRef: actor,
            returnResponse: false,
            messageFollowContext: messageFollow
          }),
          { packetName: '__zlink.actor.packet.relay' }
        );
        return submitted();
      }
      return await this.meshSubmitters.submit(
        meshName,
        () => this.submitActorSend(meshName, toBackendActorRef(actor), parts),
        signal
      );
    } catch (error) {
      if (isStaleActorError(error)) {
        this.invalidateActorRoute(actorId);
        this.options.staleActorRefReporter?.(meshName, actor.actorId);
        throw actorLocationStale(actor.actorId, error);
      }
      throw error;
    } finally {
      closeMessages(parts);
    }
  }

  private async request<TReply>(
    actorId: string,
    explicitPacketName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<TReply> {
    throwIfAborted(signal);
    const effectiveTimeoutMs = timeoutMs ?? this.options.defaultRequestTimeoutMs;
    const deadlineUnixMs = effectiveTimeoutMs === undefined
      ? undefined
      : Date.now() + effectiveTimeoutMs;
    const route = await this.resolveActorRoute(actorId, signal);
    const { meshName, actorRef: actor } = route;
    this.throwIfKnownStale(meshName, actor);
    const operationId = createMessageFollowId();
    const correlationId = createMessageFollowId();
    const requestSequence = BigInt(`0x${operationId.slice(16)}`);
    const parts = this.createPacketParts(
      ZLinkStreamMessageKind.Request,
      requestSequence === 0n ? 1n : requestSequence,
      explicitPacketName,
      request,
      new Map([
        ...metadata,
        ...actorRequestDeadlineMetadata(deadlineUnixMs)
      ]),
      correlationId
    );
    const messageFollow = createActorMessageFollowContext(
      route,
      parts,
      true,
      deadlineUnixMs,
      correlationId,
      operationId
    );
    const routedActor = attachActorMessageFollowContext(actor, messageFollow);
    try {
      const submissionCopies = await this.options.transportDeliveryGate?.()?.waitBeforeSubmit(
        actorId,
        'request',
        signal
      ) ?? 1;
      const handoff = this.options.handoffCapture?.(
        meshName,
        actor.actorId,
        parts,
        true,
        routedActor,
        deadlineUnixMs
      );
      if (handoff !== undefined) {
        const submissions = [handoff];
        for (let copy = 1; copy < submissionCopies; copy++) {
          const duplicate = this.options.handoffCapture?.(
            meshName,
            actor.actorId,
            parts,
            true,
            routedActor,
            deadlineUnixMs
          );
          if (duplicate !== undefined) submissions.push(duplicate);
        }
        const replies = await waitHandoffReply<unknown[]>(
          Promise.all(submissions),
          remainingActorRequestTimeout(actorId, deadlineUnixMs)
        );
        return replies[0] as TReply;
      }
      return await this.submitActorRequest<TReply>(
        meshName,
        toBackendActorRef(actor),
        parts,
        remainingActorRequestTimeout(actorId, deadlineUnixMs),
        signal,
        route,
        messageFollow
      );
    } catch (error) {
      if (isStaleActorError(error)) {
        this.invalidateActorRoute(actorId);
        this.options.staleActorRefReporter?.(meshName, actor.actorId);
        throw actorLocationStale(actor.actorId, error);
      }
      throw error;
    } finally {
      closeMessages(parts);
    }
  }

  private async resolveActorRoute(
    actorId: string,
    signal?: AbortSignal
  ): Promise<ZLinkResolvedActorRoute> {
    const resolver = this.options.locationResolver();
    if (resolver === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
        'Actor direct messaging requires a Location Store.'
      );
    }
    const row = await resolver.resolveDirectActorRoute(actorId, signal);
    if (row === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor route '${actorId}' was not found.`
      );
    }
    return row;
  }

  private invalidateActorRoute(actorId: string): void {
    this.options.locationResolver()?.invalidateActorRoute(actorId);
  }

  private throwIfKnownStale(meshName: string, actor: ActorRef): void {
    if (this.options.staleActorRefPredicate?.(meshName, actor) !== true) return;
    this.invalidateActorRoute(actor.actorId);
    this.options.staleActorRefReporter?.(meshName, actor.actorId);
    throw actorLocationStale(actor.actorId, undefined);
  }

  private createPacketParts(
    kind: ZLinkStreamMessageKind,
    requestSeq: bigint | undefined,
    explicitPacketName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    correlationId?: string
  ): readonly Message[] {
    const packetName = resolveFrameworkPacketName(message, explicitPacketName, 'Actor');
    const flags =
      (requestSeq === undefined ? ZLinkStreamHeaderFlags.None : ZLinkStreamHeaderFlags.HasRequestSeq)
      | (metadata.size === 0 ? ZLinkStreamHeaderFlags.None : ZLinkStreamHeaderFlags.HasMetadata)
      | (correlationId === undefined
        ? ZLinkStreamHeaderFlags.None
        : ZLinkStreamHeaderFlags.HasCorrelationId);
    const header = encodeStreamHeader({
      kind,
      codec: ZLinkStreamCodec.Json,
      flags,
      requestSeq,
      name: packetName,
      metadata,
      correlationId
    });
    return [
      RuntimeMessage.from(Buffer.from(header)) as Message,
      encodeFrameworkPayloadMessage(message, this.options.messageSerializers)
    ];
  }

  private submitActorSend(
    meshName: string,
    actor: ZLinkBackendActorRef,
    parts: readonly Message[]
  ): ZLinkSubmitResult {
    const node = this.requireNode(meshName);
    try {
      return mapSubmitResult(
        node.sendToActor(actor, toMessageLikeParts(parts), { flags: ZLINK_BACKEND_SEND_NONE }),
        'Actor send'
      );
    } catch (error) {
      if (isZLinkBackendResultError(error)) {
        return mapSubmitResult(error.result, 'Actor send');
      }
      throw isZLinkBackendResultError(error)
        ? mapRequestError(error, 'Actor send')
        : mapSubmitError(error, 'Actor send');
    }
  }

  private async submitActorRequest<TReply>(
    meshName: string,
    actor: ZLinkBackendActorRef,
    parts: readonly Message[],
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    route?: ZLinkResolvedActorRoute,
    messageFollowContext?: ZLinkActorMessageFollowContext
  ): Promise<TReply> {
    const node = this.requireNode(meshName);
    if (
      this.options.routeTransport !== undefined
      && String(node.status().routingId) !== String(actor.nodeRid)
      && route !== undefined
    ) {
      const relay = encodeRemoteActorPacketRelayPayload({
        actorId: actor.actorId,
        header: parts[0].data(),
        payload: parts[1].data(),
        bindingActorRef: {
          actorId: actor.actorId,
          objectGeneration: actor.generation,
          meshName,
          nodeRid: actor.nodeRid as unknown as RoutingId
        },
        actorRef: {
          actorId: actor.actorId,
          objectGeneration: actor.generation,
          meshName,
          nodeRid: actor.nodeRid as unknown as RoutingId
        },
        returnResponse: true,
        messageFollowContext
      });
      const reply = await requestRoutedJsonReply<{
        readonly ok?: boolean;
        readonly error?: unknown;
        readonly errorKind?: unknown;
        readonly response?: TReply;
      }>(
        this.options.routeTransport,
        remoteActorRouteTarget(route),
        relay,
        { timeoutMs, signal },
        `Remote Actor request is not available for '${actor.actorId}'.`,
        (replyParts) => JSON.parse(replyParts[0]?.getString('utf8') ?? '{}')
      );
      if (reply.ok !== true) {
        throw createInternalFrameworkException(
          remoteRelayErrorKind(reply.errorKind),
          typeof reply.error === 'string' ? reply.error : `Remote Actor request failed for '${actor.actorId}'.`
        );
      }
      return reply.response as TReply;
    }
    const completions = this.options.completionTableProvider(meshName);
    if (completions === undefined) {
      throw routeNotConnected('Actor request requires a running MeshNode completion table.');
    }
    const operationId = node.requestToActor(actor, toMessageLikeParts(parts), {
      flags: ZLINK_BACKEND_SEND_NONE,
      timeoutMs
    });
    const completion = await completions.wait(operationId, signal);
    if (completion.terminalResult !== RequestResult.Ok) {
      closeMeshCompletion(completion);
      throw mapRequestResult(completion.terminalResult, 'Actor request');
    }
    return decodeActorReply<TReply>(completion.parts, this.options.messageSerializers);
  }

  private requireNode(meshName: string): ZLinkBackendMeshNode {
    const node = this.options.nodeProvider(meshName);
    if (node === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RouteNotConnected,
        `Actor client requires a running MeshNode for RouteMesh '${meshName}'.`,
        true
      );
    }
    return node;
  }
}

function remoteActorRouteTarget(route: ZLinkResolvedActorRoute) {
  if (route.enclosingSpotRoute !== undefined) {
    return route.enclosingSpotRoute;
  }
  return {
    routerChannelId: route.meshName,
    targetNodeRid: route.actorRef.nodeRid,
    spotId: (route.spotId ?? String(route.actorRef.nodeRid)) as unknown as RoutingId,
    spotKind: route.spotId === undefined ? ZLinkSpotKind.Entry : ZLinkSpotKind.User,
    targetSpotGeneration: route.spotGeneration ?? route.ownerNodeGeneration,
    targetNodeGeneration: route.ownerNodeGeneration,
    targetOwnerId: route.ownerId,
    ownerLeaseGeneration: route.ownerLeaseGeneration,
    authorityOwnerGeneration: route.authorityOwnerGeneration,
    authorityStoreVersion: route.authorityStoreVersion
  };
}

function createActorMessageFollowContext(
  route: ZLinkResolvedActorRoute,
  parts: readonly Message[],
  request: boolean,
  deadlineUnixMs?: number,
  correlationId?: string,
  operationId?: string
): ZLinkActorMessageFollowContext {
  if (typeof route.ownerId !== 'string' || route.ownerId.length === 0
      || typeof route.ownerLeaseGeneration !== 'bigint'
      || route.ownerLeaseGeneration <= 0n
      || typeof route.ownerNodeGeneration !== 'bigint'
      || route.ownerNodeGeneration <= 0n
      || typeof route.authorityOwnerGeneration !== 'bigint'
      || route.authorityOwnerGeneration <= 0n
      || typeof route.actorRef.objectGeneration !== 'bigint'
      || route.actorRef.objectGeneration <= 0n) {
    throw actorLocationStale(
      route.actorRef.actorId,
      new Error('Resolved Actor route does not contain an exact authority owner fence.')
    );
  }
  return createInitialActorMessageFollowContext(
    route,
    parts,
    request,
    deadlineUnixMs,
    correlationId,
    operationId
  );
}

function remoteRelayErrorKind(value: unknown): ZLinkFrameworkInternalErrorKind {
  return Object.values(ZLinkFrameworkInternalErrorKind).includes(value as ZLinkFrameworkInternalErrorKind)
    ? value as ZLinkFrameworkInternalErrorKind
    : ZLinkFrameworkInternalErrorKind.RequestFailed;
}

function remainingActorRequestTimeout(actorId: string, deadlineUnixMs: number | undefined): number | undefined {
  if (deadlineUnixMs === undefined) return undefined;
  const remaining = deadlineUnixMs - Date.now();
  if (remaining <= 0) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
      `Actor request '${actorId}' exceeded its deadline before submission.`
    );
  }
  return Math.max(1, Math.ceil(remaining));
}

function requireActorId(actorId: string): void {
  const byteLength = Buffer.byteLength(actorId, 'utf8');
  if (byteLength < 1 || byteLength > 255) {
    throw new ZLinkConfigurationException('Actor ID must contain 1..255 UTF-8 bytes.');
  }
}

async function waitHandoffReply<TReply>(
  reply: Promise<unknown>,
  timeoutMs: number | undefined
): Promise<TReply> {
  if (timeoutMs === undefined) return await reply as TReply;
  let deadline: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      reply as Promise<TReply>,
      new Promise<TReply>((_resolve, reject) => {
        deadline = setTimeout(
          () => reject(createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RequestFailed,
            'Actor handoff request timed out.',
            true
          )),
          timeoutMs
        );
      })
    ]);
  } finally {
    if (deadline !== undefined) clearTimeout(deadline);
  }
}

export async function forwardEncodedActorPacket(
  node: ZLinkBackendMeshNode,
  completions: ZLinkMeshCompletionTable,
  actor: ActorRef,
  header: Buffer,
  payload: Buffer,
  returnResponse: boolean,
  timeoutMs: number | undefined,
  serializers?: ReadonlyMap<string, ZLinkMessageSerializer>
): Promise<unknown> {
  const target = toBackendActorRef(actor);
  const parts = [header, payload];
  if (!returnResponse) {
    if (node.sendToActor(target, parts, { flags: ZLINK_BACKEND_SEND_NONE }) !== SubmitResult.Ok) {
      throw routeNotConnected('Actor handoff send submit was not accepted.');
    }
    return undefined;
  }
  const operationId = node.requestToActor(target, parts, {
    flags: ZLINK_BACKEND_SEND_NONE,
    timeoutMs
  });
  const completion = await completions.wait(operationId);
  if (completion.terminalResult !== RequestResult.Ok) {
    closeMeshCompletion(completion);
    throw mapRequestResult(completion.terminalResult, 'Actor handoff request');
  }
  return decodeActorReply(completion.parts, serializers);
}

function toBackendActorRef(actor: ActorRef): ZLinkBackendActorRef {
  return {
    nodeRid: actor.nodeRid,
    actorId: actor.actorId,
    generation: actor.objectGeneration
  };
}

function toMessageLikeParts(parts: readonly Message[]): readonly Buffer[] {
  return parts.map((part) => Buffer.from(part.data()));
}

class DefaultZLinkActorSendCall implements ZLinkActorSendCall {
  private packet?: string;
  private readonly selectedMetadata = new Map<string, string>();
  private executed = false;

  constructor(
    private readonly submitter: (
      packetName: string | undefined,
      metadata: ReadonlyMap<string, string>,
      signal?: AbortSignal
    ) => Promise<ZLinkSubmitResult>,
    private readonly message: unknown
  ) {}

  packetName(packetName: string): this {
    this.packet = packetName;
    return this;
  }

  metadata(key: string, value: string): this {
    this.selectedMetadata.set(key, value);
    return this;
  }

  async submit(signal?: AbortSignal): Promise<void> {
    ensureSingleSubmit(this.executed);
    this.executed = true;
    const result = await this.submitter(
      this.packet ?? resolveFrameworkPacketName(this.message, undefined, 'Actor'),
      this.selectedMetadata,
      signal
    );
    requireOneWayCompletion(
      result,
      'Actor send',
      ZLinkFrameworkInternalErrorKind.ActorRouteNotFound
    );
  }
}

class DefaultZLinkActorRequestCall implements ZLinkActorRequestCall {
  private packet?: string;
  private timeoutMs?: number;
  private executed = false;
  private readonly selectedMetadata = new Map<string, string>();
  private readonly turn: ZLinkSpotSerialTurn | undefined = captureZLinkSpotSerialTurn();

  constructor(
    private readonly submitter: <TReply>(
      packetName: string | undefined,
      timeoutMs: number | undefined,
      metadata: ReadonlyMap<string, string>,
      signal?: AbortSignal
    ) => Promise<TReply>,
    private readonly request: unknown,
    private readonly defaultRequestTimeoutMs?: number
  ) {}

  packetName(packetName: string): this {
    this.packet = packetName;
    return this;
  }

  metadata(key: string, value: string): this {
    this.selectedMetadata.set(key, value);
    return this;
  }

  timeout(timeoutMs: number): this {
    this.timeoutMs = timeoutMs;
    return this;
  }

  submit<TReply>(signal?: AbortSignal): Promise<TReply> {
    return this.execute<TReply>(signal);
  }

  yield<TReply>(signal?: AbortSignal): Promise<TReply> {
    const turn = requireZLinkYieldTurn(this.turn);
    const pending = this.execute<TReply>(signal);
    return turn.yieldPromise(pending);
  }

  private execute<TReply>(signal?: AbortSignal): Promise<TReply> {
    ensureSingleSubmit(this.executed);
    this.executed = true;
    throwIfAborted(signal);
    return this.submitter<TReply>(
      this.packet ?? resolveFrameworkPacketName(this.request, undefined, 'Actor'),
      this.timeoutMs ?? this.defaultRequestTimeoutMs,
      this.selectedMetadata,
      signal
    );
  }
}

function decodeActorReply<TReply>(
  reply: readonly Message[],
  serializers?: ReadonlyMap<string, ZLinkMessageSerializer>
): TReply {
  try {
    if (reply.length === 1) {
      const frame = tryDecodeStreamFrame(reply[0].data());
      if (frame !== undefined) {
        const header = decodeStreamHeader(frame.header);
        const payload = RuntimeMessage.from(frame.payload) as Message;
        try {
          return decodeActorReplyPayload<TReply>(header.kind, payload, serializers);
        } finally {
          payload.close();
        }
      }
    }
    if (reply.length >= 2) {
      const header = decodeStreamHeader(reply[0].data());
      return decodeActorReplyPayload<TReply>(header.kind, reply[1], serializers);
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestProtocolError,
      'Actor request reply is empty.'
    );
  } finally {
    closeMessages(reply);
  }
}

function closeMessages(parts: readonly Message[]): void {
  for (const part of parts) part.close();
}

function decodeActorReplyPayload<TReply>(
  kind: ZLinkStreamMessageKind,
  payload: Message,
  serializers?: ReadonlyMap<string, ZLinkMessageSerializer>
): TReply {
  if (kind === ZLinkStreamMessageKind.Error) {
    const error = decodeFrameworkPayloadMessage<{
      readonly message?: string;
      readonly kind?: number;
    }>(payload, serializers);
    const wireKind = error.kind;
    const publicKind = typeof wireKind === 'number'
      && Number.isInteger(wireKind)
      && wireKind >= ZLinkFrameworkErrorKind.NotFound
      && wireKind <= ZLinkFrameworkErrorKind.InternalFailure
      ? wireKind as ZLinkFrameworkErrorKind
      : ZLinkFrameworkErrorKind.InternalFailure;
    throw new ZLinkFrameworkException(publicKind, error.message ?? 'Actor request failed.');
  }
  return decodeFrameworkPayloadMessage<TReply>(payload, serializers);
}

function mapSubmitError(error: unknown, operationName: string): Error {
  if (error instanceof ZLinkFrameworkException) {
    return error;
  }
  if (isZLinkBackendResultError(error)) {
    switch (error.result) {
      case SubmitResult.NotConnected:
        return routeNotConnected(`${operationName} failed because the target route is not connected.`, error);
      case SubmitResult.NotFound:
        return createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
          `${operationName} failed because the actor route was not found.`,
          false,
          error
        );
      default:
        return createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestFailed,
          `${operationName} failed with submit result ${error.result}.`,
          false,
          error
        );
    }
  }
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RouteNotConnected,
    `${operationName} failed before a reply was received.`,
    true,
    error
  );
}

function mapSubmitResult(result: number, operationName: string): ZLinkSubmitResult {
  switch (result) {
    case SubmitResult.Ok:
      return submitted();
    case SubmitResult.Backpressured:
    case SubmitResult.NotAdmitted:
      return { status: ZLinkSubmitStatus.Backpressured };
    case SubmitResult.NotConnected:
      return { status: ZLinkSubmitStatus.RouteNotConnected };
    case SubmitResult.NotFound:
      return { status: ZLinkSubmitStatus.TargetNotFound };
    case SubmitResult.Terminated:
      return { status: ZLinkSubmitStatus.Shutdown };
    default:
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        `${operationName} failed with submit result ${result}.`
      );
  }
}

function submitted(): ZLinkSubmitResult {
  return { status: ZLinkSubmitStatus.Submitted };
}

function mapRequestResult(result: number, operationName: string): Error {
  switch (result) {
    case RequestResult.NotConnected:
      return routeNotConnected(`${operationName} failed because the target route is not connected.`);
    case RequestResult.NotFound:
      return createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `${operationName} failed because the actor route was not found.`
      );
    case RequestResult.Conflict:
      return createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `${operationName} failed because the actor location is stale.`
      );
    default:
      return createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        `${operationName} failed with request result ${result}.`
      );
  }
}

function mapRequestError(error: { readonly result: number }, operationName: string): Error {
  return mapRequestResult(error.result, operationName);
}


function routeNotConnected(message: string, cause?: unknown): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RouteNotConnected,
    message,
    true,
    cause
  );
}

function isStaleActorError(error: unknown): boolean {
  return error instanceof ZLinkFrameworkException
    && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.ActorLocationStale;
}

function actorLocationStale(actorId: string, cause: unknown): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorLocationStale,
    `Actor route '${actorId}' is stale after re-resolve.`,
    true,
    cause
  );
}

function ensureSingleSubmit(executed: boolean): void {
  if (executed) {
    throwAlreadySubmitted('Actor client call');
  }
}
