import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException, internalFrameworkErrorKind  } from '../framework-errors-internal';
import type {
  ActorRef,
  ZLinkActor,
  ZLinkMessageSerializer,
  ZLinkSpot
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkFrameworkException,
  zlinkMessageMetadata
} from '../../contracts';
import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { flowIfEnabled } from '../diagnostics';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import type { ZLinkRemoteBoundSessionTarget } from '../actors';
import {
  ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET,
  ZLinkSpotActorDispatcher,
  ZLinkSpotActorHandlerRegistryRuntime
} from '../actors';
import type { ZLinkDispatchErrorReporter } from '../channels';
import {
  decodeStreamHeader,
  messageToBytes,
  streamCodecContentType,
  ZLinkStreamMessageKind
} from '../streams/protocol';
import { decodeFrameworkTypedPayloadMessage } from '../messaging/payload-codec';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkSpotSerialTurnExecutor } from './spot-serial-turn-executor';
import type { ZLinkSerialWorkOptions } from '../execution/serial-execution-queue';
import { zlinkMetadataByteLength, zlinkSerialWorkOptions } from '../execution/serial-work-size';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';

export interface ZLinkActorResponseOptions {
  readonly metadata: ReadonlyMap<string, string>;
  readonly compressPayload: boolean;
}

/** Internal two-phase terminal used when a routed request owns the reply. */
export interface ZLinkActorRequestTerminal {
  (response: unknown, preparedReply?: unknown): Promise<void> | void;
  readonly prepare?: (response: unknown) => Promise<unknown> | unknown;
}

/** One admitted Actor packet and the routing context that must move with it. */
export interface ZLinkActorPacketDelivery {
  readonly actorId: string;
  readonly parts: readonly Message[];
  readonly returnResponse: boolean;
  readonly remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget;
  readonly fallbackActorRef?: ActorRef;
  readonly requestTerminal?: ZLinkActorRequestTerminal;
  readonly messageFollowOrigin?: ZLinkMessageFollowOrigin;
}

/**
 * Spec 15 §4.2 relocation temporary queue: consulted when {@code
 * resolveActor} finds nothing for an arrival that carries an exact object
 * identity. Parks the arrival if an Actor Join admission attempt is in
 * flight for {@code (actorId, objectGeneration)}, or reports not-found so
 * the caller falls through to its existing missing-actor handling
 * unchanged.
 */
export type ZLinkRouteToActorJoinPrewarm = (
  actorId: string,
  objectGeneration: bigint,
  arrival: {
    readonly header: Buffer;
    readonly payload: Buffer;
    readonly returnResponse: boolean;
    readonly remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget;
    readonly fallbackActorRef?: ActorRef;
    readonly requestTerminal?: ZLinkActorRequestTerminal;
    readonly resolve: (value: unknown) => void;
    readonly reject: (reason: unknown) => void;
  }
) => 'parked' | 'not-found';

interface ZLinkSpotActorPacketDispatchOptions {
  readonly spot: ZLinkSpot | (() => ZLinkSpot);
  readonly spotId: () => string;
  readonly registry: ZLinkSpotActorHandlerRegistryRuntime;
  readonly serial?: ZLinkSpotSerialTurnExecutor;
  readonly resolveActor: (actorId: string) => ZLinkActor | undefined;
  readonly actorLeft?: (actorId: string) => boolean;
  readonly routeBeforeLocal?: (
    delivery: ZLinkActorPacketDelivery
  ) => Promise<{ readonly handled: boolean; readonly response?: unknown } | undefined> |
    { readonly handled: boolean; readonly response?: unknown } |
    undefined;
  readonly onRemoteBoundSessionTarget?: (
    actorId: string,
    target: ZLinkRemoteBoundSessionTarget | undefined
  ) => void;
  readonly onDisconnectActor: (actor: ZLinkActor) => Promise<void>;
  readonly actorResponseSender?: (
    actor: ZLinkActor,
    packetName: string,
    requestSeq: bigint,
    response: unknown,
    replyOptions: ZLinkActorResponseOptions,
    fallbackBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    signal?: AbortSignal
  ) => Promise<void> | void;
  readonly actorErrorSender?: (
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    fallbackBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ) => Promise<void> | void;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly routeToActorJoinPrewarm?: ZLinkRouteToActorJoinPrewarm;
}

export class ZLinkSpotActorPacketDispatch {
  constructor(private readonly options: ZLinkSpotActorPacketDispatchOptions) {}

  dispatch(
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: ZLinkActorRequestTerminal
  ): Promise<unknown> {
    return this.dispatchDelivery({
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal
    });
  }

  async dispatchDelivery(delivery: ZLinkActorPacketDelivery): Promise<unknown> {
    const {
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal
    } = delivery;
    if (parts.length < 2) {
      this.reportInvalidFrame(actorId, ZLinkDispatchMessageKind.ActorSend);
      return undefined;
    }
    //  Spec 26 §4.1: one level read per processing point — the same gate value
    //  drives both the decode and the flow-context install.
    const flowEnabled = this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true;
    let header: ReturnType<typeof decodeStreamHeader>;
    try {
      header = decodeStreamHeader(messageToBytes(parts[0]), flowEnabled);
    } catch (error) {
      this.reportInvalidFrame(actorId, ZLinkDispatchMessageKind.ActorSend, error);
      throw error;
    }
    return runWithFlow(createInboundFlow(
      header.flowId,
      header.flowOrigin,
      flowEnabled
    ), async () => {
      const messageKind = header.kind === ZLinkStreamMessageKind.Request
        ? ZLinkDispatchMessageKind.ActorRequest
        : ZLinkDispatchMessageKind.ActorSend;
      const action = messageKind === ZLinkDispatchMessageKind.ActorRequest
        ? ZLinkDispatchErrorAction.ReplyError
        : ZLinkDispatchErrorAction.Drop;
      this.trace(ZLinkMessageFlowOutcome.Received, actorId, header, messageKind);
      if (
        this.options.actorLeft?.(actorId) === true &&
        header.name === ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET
      ) {
        return undefined;
      }
      if (remoteBoundSessionTarget !== undefined) {
        this.options.onRemoteBoundSessionTarget?.(actorId, remoteBoundSessionTarget);
      }
      const routed = await this.options.routeBeforeLocal?.(
        delivery
      );
      if (routed?.handled === true) {
        return routed.response;
      }
      const actor = this.options.resolveActor(actorId);
      if (actor === undefined) {
        return this.handleMissingActor(
          actorId,
          parts,
          header,
          messageKind,
          action,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef,
          requestTerminal
        );
      }
      if (header.name === ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET) {
        this.options.onRemoteBoundSessionTarget?.(actorId, undefined);
        await this.options.onDisconnectActor(actor);
        return undefined;
      }
      const decodePayload = this.createPayloadDecoder(parts[1], header);
      return this.dispatchActorPacket(
        actor,
        actorId,
        decodePayload,
        header,
        messageKind,
        action,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef,
        requestTerminal,
        zlinkSerialWorkOptions(
          parts[1].data().byteLength,
          zlinkMetadataByteLength(header.metadata)
        )
      );
    });
  }

  private async handleMissingActor(
    actorId: string,
    parts: readonly Message[],
    header: ReturnType<typeof decodeStreamHeader>,
    messageKind: ZLinkDispatchMessageKind,
    action: ZLinkDispatchErrorAction,
    returnResponse: boolean,
    fallbackBoundSessionTarget: ZLinkRemoteBoundSessionTarget | undefined,
    fallbackActorRef: ActorRef | undefined,
    requestTerminal: ZLinkActorRequestTerminal | undefined
  ): Promise<unknown> {
    if (
      this.options.routeToActorJoinPrewarm !== undefined
      && fallbackActorRef !== undefined
      && parts.length >= 2
      //  A Request without a terminal has no way to complete once
      //  migrated later — parking it would leave the caller hanging
      //  forever with no reply route. Fall through to the existing
      //  missing-actor handling for that shape instead.
      && (messageKind !== ZLinkDispatchMessageKind.ActorRequest || requestTerminal !== undefined)
    ) {
      const parked = this.parkForActorJoinPrewarm(
        actorId,
        parts,
        messageKind,
        returnResponse,
        fallbackBoundSessionTarget,
        fallbackActorRef,
        requestTerminal
      );
      if (parked !== undefined) return parked;
    }
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.SpotActor,
      messageKind,
      reason: ZLinkDispatchErrorReason.HandlerMissing,
      action,
      packetName: header.name,
      spotId: this.options.spotId(),
      actorId,
      correlationId: header.correlationId ?? header.requestSeq?.toString()
    });
    if (messageKind !== ZLinkDispatchMessageKind.ActorRequest) {
      return undefined;
    }
    const missingActorError = createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorDispatchHandlerNotFound,
      `SPOT actor is not registered locally: ${actorId}`
    );
    if (header.requestSeq !== undefined && !returnResponse && this.options.actorErrorSender !== undefined) {
      await this.options.actorErrorSender(
        actorId,
        header.name,
        header.requestSeq,
        missingActorError,
        header.metadata,
        fallbackBoundSessionTarget,
        fallbackActorRef
      );
      return undefined;
    }
    throw missingActorError;
  }

  /**
   * Spec 15 §4.2 relocation temporary queue: parks one arrival for a
   * missing Actor that carries an exact object identity, instead of
   * dropping it (Send) or replying HandlerNotFound (Request). Returns
   * {@code undefined} when no admission attempt owns this object — the
   * caller falls through to its existing missing-actor handling unchanged.
   * Header/payload are copied to owned buffers before parking so the
   * arrival survives independently of the original {@link Message}
   * lifetime.
   */
  private parkForActorJoinPrewarm(
    actorId: string,
    parts: readonly Message[],
    messageKind: ZLinkDispatchMessageKind,
    returnResponse: boolean,
    remoteBoundSessionTarget: ZLinkRemoteBoundSessionTarget | undefined,
    fallbackActorRef: ActorRef,
    requestTerminal: ZLinkActorRequestTerminal | undefined
  ): Promise<unknown> | undefined {
    const isRequest = messageKind === ZLinkDispatchMessageKind.ActorRequest;
    let resolve: (value: unknown) => void = () => undefined;
    let reject: (reason: unknown) => void = () => undefined;
    const result = new Promise<unknown>((res, rej) => {
      resolve = res;
      reject = rej;
    });
    //  A Request's real reply must still travel through the exact mailbox
    //  correlation the original caller captured in `requestTerminal` — not
    //  through this method's own return value, which the caller only uses
    //  to decide whether it still owes a reply itself. Wrapping keeps that
    //  reply route intact across the (possibly much later) redelivery at
    //  migration time while also releasing the original caller once the
    //  real reply has gone out, so it never sends a premature empty one.
    const wrappedRequestTerminal: ZLinkActorRequestTerminal | undefined =
      requestTerminal === undefined
        ? undefined
        : Object.assign(
            async (response: unknown, preparedReply?: unknown) => {
              try {
                await requestTerminal(response, preparedReply);
                resolve(undefined);
              } catch (error) {
                reject(error);
                throw error;
              }
            },
            { prepare: requestTerminal.prepare }
          );
    const route = this.options.routeToActorJoinPrewarm!(
      actorId,
      fallbackActorRef.objectGeneration,
      {
        header: Buffer.from(parts[0].data()),
        payload: Buffer.from(parts[1].data()),
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef,
        requestTerminal: wrappedRequestTerminal,
        resolve,
        reject
      }
    );
    if (route !== 'parked') return undefined;
    //  A Send has no reply to wait for: the migrated redelivery still runs
    //  in order later, but this caller's own await is done once parking is
    //  confirmed.
    if (!isRequest) resolve(undefined);
    return result;
  }

  private createPayloadDecoder(
    message: Message,
    header: ReturnType<typeof decodeStreamHeader>
  ): () => unknown {
    return () => decodeFrameworkTypedPayloadMessage(
      message,
      this.options.messageSerializers,
      undefined,
      streamCodecContentType(header.codec),
      header.name
    );
  }

  private async dispatchActorPacket(
    actor: ZLinkActor,
    actorId: string,
    decodePayload: () => unknown,
    header: ReturnType<typeof decodeStreamHeader>,
    messageKind: ZLinkDispatchMessageKind,
    action: ZLinkDispatchErrorAction,
    returnResponse: boolean,
    fallbackBoundSessionTarget: ZLinkRemoteBoundSessionTarget | undefined,
    fallbackActorRef: ActorRef | undefined,
    requestTerminal: ZLinkActorRequestTerminal | undefined,
    workOptions: ZLinkSerialWorkOptions
  ): Promise<unknown> {
    const spot = typeof this.options.spot === 'function'
      ? this.options.spot()
      : this.options.spot;
    const dispatcher = new ZLinkSpotActorDispatcher({
      registry: this.options.registry,
      spot,
      providerResolver: this.options.providerResolver,
      serial: this.options.serial,
      serialWorkOptions: workOptions,
      messageSerializers: this.options.messageSerializers,
      onAdmitted: () => this.trace(
        ZLinkMessageFlowOutcome.Admitted,
        actorId,
        header,
        messageKind
      ),
      onHandlerStart: () => this.trace(
        ZLinkMessageFlowOutcome.Dispatched,
        actorId,
        header,
        messageKind
      )
    });
    try {
      if (header.kind === ZLinkStreamMessageKind.Send) {
        await dispatcher.dispatchSendDecoded(actor, header.name, decodePayload, {
          meshName: spot.context.meshName,
          metadata: zlinkMessageMetadata(header.metadata),
          correlationId: header.correlationId ?? undefined
        });
        this.trace(ZLinkMessageFlowOutcome.Completed, actorId, header, ZLinkDispatchMessageKind.ActorSend);
        return undefined;
      }
      if (header.kind !== ZLinkStreamMessageKind.Request || header.requestSeq === undefined) {
        this.options.dispatchErrors?.report({
          surface: ZLinkDispatchErrorSurface.SpotActor,
          messageKind: ZLinkDispatchMessageKind.ActorRequest,
          reason: ZLinkDispatchErrorReason.InvalidFrame,
          action: ZLinkDispatchErrorAction.Drop,
          packetName: header.name,
          spotId: this.options.spotId(),
          actorId
        });
        return undefined;
      }
      const requestSeq = header.requestSeq;
      if (returnResponse && requestTerminal !== undefined) {
        let preparedReply: unknown;
        let preparedReplyReady = false;
        await dispatcher.dispatchRequestThenDecoded(actor, header.name, decodePayload, {
          meshName: spot.context.meshName,
          metadata: zlinkMessageMetadata(header.metadata),
          correlationId: header.correlationId ?? header.requestSeq.toString()
        }, async (response) => {
          this.trace(ZLinkMessageFlowOutcome.Replied, actorId, header, ZLinkDispatchMessageKind.ActorRequest);
          await requestTerminal(response, preparedReplyReady ? preparedReply : undefined);
        }, requestTerminal.prepare === undefined
          ? undefined
          : async (response) => {
              preparedReply = await requestTerminal.prepare!(response);
              preparedReplyReady = true;
            });
        return undefined;
      }
      if (returnResponse || this.options.actorResponseSender === undefined) {
        const response = await dispatcher.dispatchRequestDecoded(actor, header.name, decodePayload, {
          meshName: spot.context.meshName,
          metadata: zlinkMessageMetadata(header.metadata),
          correlationId: header.correlationId ?? header.requestSeq.toString()
        });
        this.trace(ZLinkMessageFlowOutcome.Replied, actorId, header, ZLinkDispatchMessageKind.ActorRequest);
        return response;
      }
      await dispatcher.dispatchRequestThenDecoded(actor, header.name, decodePayload, {
        meshName: spot.context.meshName,
        metadata: zlinkMessageMetadata(header.metadata),
        correlationId: header.correlationId ?? header.requestSeq.toString()
      }, async (response, replyOptions) => {
        this.trace(ZLinkMessageFlowOutcome.Replied, actorId, header, ZLinkDispatchMessageKind.ActorRequest);
        await this.options.actorResponseSender?.(
          actor,
          header.name,
          requestSeq,
          response,
          replyOptions,
          fallbackBoundSessionTarget,
          fallbackActorRef,
          undefined
        );
      });
      return undefined;
    } catch (error) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotActor,
        messageKind,
        reason: error instanceof ZLinkFrameworkException
          && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.ActorDispatchHandlerNotFound
          ? ZLinkDispatchErrorReason.HandlerMissing
          : error instanceof ZLinkFrameworkException
            && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed
            ? ZLinkDispatchErrorReason.PayloadDecodeFailed
            : ZLinkDispatchErrorReason.HandlerException,
        action,
        packetName: header.name,
        spotId: this.options.spotId(),
        actorId,
        correlationId: header.correlationId ?? header.requestSeq?.toString(),
        error
      });
      if (
        messageKind === ZLinkDispatchMessageKind.ActorRequest &&
        header.requestSeq !== undefined &&
        !returnResponse &&
        this.options.actorErrorSender !== undefined
      ) {
        await this.options.actorErrorSender(
          actorId,
          header.name,
          header.requestSeq,
          error,
          header.metadata,
          fallbackBoundSessionTarget,
          fallbackActorRef
        );
        return undefined;
      }
      throw error;
    }
  }

  private reportInvalidFrame(
    actorId: string,
    messageKind: ZLinkDispatchMessageKind,
    error?: unknown
  ): void {
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.SpotActor,
      messageKind,
      reason: ZLinkDispatchErrorReason.InvalidFrame,
      action: ZLinkDispatchErrorAction.Drop,
      spotId: this.options.spotId(),
      actorId,
      error
    });
  }

  private trace(
    outcome: ZLinkMessageFlowOutcome,
    actorId: string,
    header: ReturnType<typeof decodeStreamHeader>,
    messageKind: ZLinkDispatchMessageKind
  ): void {
    flowIfEnabled(this.options.dispatchErrors?.flow, outcome)?.trace({
      outcome,
      surface: ZLinkDispatchErrorSurface.SpotActor,
      messageKind,
      packetName: header.name,
      spotId: this.options.spotId(),
      actorId,
      correlationId: header.correlationId ?? header.requestSeq?.toString()
    });
  }
}
