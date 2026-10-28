import {
  resolveActorIngress,
  type ZLinkRouteToActorJoinPrewarm,
  type ZLinkActorIngressRoute
} from './spot-actor-ingress-admission';
import type { ZLinkSpotActorHandoffRuntime } from './spot-runtime-ports';
import { DefaultZLinkActorContext } from '../actors/actor-context';
import { decodeSessionDisconnected } from '../actors/session-disconnected-wire';
import { routingIdsEqual } from '../routing-id';
import type { ApplicationJobOrigin } from '../application-jobs/contracts';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import type {
  ActorRef,
  RoutingId,
  Type,
  ZLinkActor,
  ZLinkMessageSerializer,
  ZLinkSpot
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind,
  ZLinkMessage,
  ZLinkEncodedPayload,
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
import { dispatchReasonFromError } from '../diagnostics/dispatch-error-details';
import { createInboundFlow, currentFlowContext, runWithFlow } from '../diagnostics/flow-context';
import { streamSessionIdFromRoutingId } from '../streams/managed-stream';
import type { ZLinkRemoteBoundSessionTarget } from '../actors';
import {
  ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET,
  ZLinkSpotActorDispatcher,
  ZLinkSpotActorHandlerRegistryRuntime
} from '../actors';
import type { ZLinkDispatchErrorReporter } from '../channels';
import {
  decodeActorRequestDeadlineUnixMs,
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
import { releaseApplicationJobPermitBeforeHandler } from '../application-jobs/application-job-queue-scope';

const emptyDisconnectedReply = ZLinkMessage.fromEncoded(
  ZLinkEncodedPayload.fromOwned(Buffer.alloc(0))
);

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
  readonly origin: ApplicationJobOrigin;
  readonly actorId: string;
  readonly parts: readonly Message[];
  readonly returnResponse: boolean;
  readonly remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget;
  readonly fallbackActorRef?: ActorRef;
  readonly requestTerminal?: ZLinkActorRequestTerminal;
  readonly messageFollowOrigin?: ZLinkMessageFollowOrigin;
  readonly sourceSessionRid?: RoutingId;
}

export type { ZLinkRouteToActorJoinPrewarm } from './spot-actor-ingress-admission';
interface ZLinkSpotActorPacketDispatchOptions {
  readonly admission?: import('../admission').ZLinkRuntimeAdmissionGate;
  readonly spot: ZLinkSpot | (() => ZLinkSpot);
  readonly spotId: () => string;
  readonly registry: ZLinkSpotActorHandlerRegistryRuntime;
  readonly resolveActor: (actorId: string) => ZLinkActor | undefined;
  readonly hasDepartedActor?: (actorId: string) => boolean;
  readonly routeBeforeLocal?: ZLinkActorIngressRoute;
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

export type ZLinkActorPacketExecution =
  { readonly terminal: Promise<unknown> } | { readonly response: unknown };

export class ZLinkSpotActorPacketDispatch {
  constructor(private readonly options: ZLinkSpotActorPacketDispatchOptions) {}

  dispatch(
    serial: ZLinkSpotSerialTurnExecutor | undefined,
    origin: ApplicationJobOrigin,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: ZLinkActorRequestTerminal
  ): Promise<unknown> {
    return this.dispatchDelivery(serial, {
      origin,
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal
    });
  }

  async dispatchDelivery(
    serial: ZLinkSpotSerialTurnExecutor | undefined,
    delivery: ZLinkActorPacketDelivery
  ): Promise<unknown> {
    const result = await this.admitDelivery(serial, delivery);
    return 'terminal' in result ? await result.terminal : result.response;
  }

  async admitDelivery(
    serial: ZLinkSpotSerialTurnExecutor | undefined,
    delivery: ZLinkActorPacketDelivery,
    handoffRuntime?: ZLinkSpotActorHandoffRuntime
  ): Promise<ZLinkActorPacketExecution> {
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
      return { response: undefined };
    }
    //  Spec 26 §4.1: one level read per processing point — the same gate value
    //  drives both the decode and the flow-context install.
    const flowEnabled = this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true;
    let header: ReturnType<typeof decodeStreamHeader>;
    let requestDeadlineUnixMs: number | undefined;
    try {
      header = decodeStreamHeader(messageToBytes(parts[0]), flowEnabled);
      requestDeadlineUnixMs =
        header.kind === ZLinkStreamMessageKind.Request
          ? decodeActorRequestDeadlineUnixMs(messageToBytes(parts[0]))
          : undefined;
    } catch (error) {
      this.reportInvalidFrame(actorId, ZLinkDispatchMessageKind.ActorSend, error);
      throw error;
    }
    return runWithFlow(
      createInboundFlow(
        header.flowId,
        header.flowOrigin,
        flowEnabled,
        !flowEnabled
          ? undefined
          : delivery.sourceSessionRid === undefined
            ? currentFlowContext()?.streamSessionId
            : streamSessionIdFromRoutingId(delivery.sourceSessionRid)
      ),
      async () => {
        const messageKind =
          header.kind === ZLinkStreamMessageKind.Request
            ? ZLinkDispatchMessageKind.ActorRequest
            : ZLinkDispatchMessageKind.ActorSend;
        const action =
          messageKind === ZLinkDispatchMessageKind.ActorRequest
            ? ZLinkDispatchErrorAction.ReplyError
            : ZLinkDispatchErrorAction.Drop;
        this.trace(ZLinkMessageFlowOutcome.Received, actorId, header, messageKind);
        const disconnected = header.name === ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET;
        const disconnectedReply =
          disconnected && header.kind === ZLinkStreamMessageKind.Request
            ? emptyDisconnectedReply
            : undefined;
        const admitted = resolveActorIngress(
          delivery,
          messageKind === ZLinkDispatchMessageKind.ActorRequest,
          {
            handoffRuntime,
            routeBeforeLocal: this.options.routeBeforeLocal,
            resolveActor: this.options.resolveActor,
            hasDepartedActor: this.options.hasDepartedActor,
            routeToActorJoinPrewarm: this.options.routeToActorJoinPrewarm
          }
        );
        if ('terminal' in admitted) return { terminal: admitted.terminal };
        if (admitted.kind === 'stale')
          return {
            response: await this.dispatchUnavailableActor(
              admitted.error,
              actorId,
              header,
              messageKind,
              action,
              returnResponse,
              remoteBoundSessionTarget,
              fallbackActorRef
            )
          };
        const actor = admitted.actor;
        if (disconnected) {
          const identity = decodeSessionDisconnected(messageToBytes(parts[1]));
          if (identity === undefined) {
            if (this.options.dispatchErrors?.captureEnabled() === true)
              this.reportInvalidFrame(
                actorId,
                messageKind,
                new ZLinkFrameworkException(
                  ZLinkFrameworkErrorKind.ProtocolError,
                  'ProtocolError: malformed Session disconnect identity.'
                ),
                'warning'
              );
            return { response: disconnectedReply };
          }
          const current =
            actor.context instanceof DefaultZLinkActorContext
              ? actor.context.currentSessionBinding
              : undefined;
          if (
            current !== undefined &&
            (!routingIdsEqual(current.sessionNodeRid, identity.sessionOwnerNodeRid) ||
              current.sessionOwnerNodeGeneration !== identity.sessionOwnerNodeGeneration ||
              current.bindingGeneration !== identity.bindingGeneration)
          )
            return { response: disconnectedReply };
          await this.options.onDisconnectActor(actor);
          this.trace(
            messageKind === ZLinkDispatchMessageKind.ActorRequest
              ? ZLinkMessageFlowOutcome.Replied
              : ZLinkMessageFlowOutcome.Completed,
            actorId,
            header,
            messageKind
          );
          return { response: disconnectedReply };
        }
        const decodePayload = this.createPayloadDecoder(parts[1], header);
        return {
          response: await this.dispatchActorPacket(
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
            requestDeadlineUnixMs,
            serial,
            zlinkSerialWorkOptions(
              parts[1].data().byteLength,
              zlinkMetadataByteLength(header.metadata)
            )
          )
        };
      }
    );
  }

  private async dispatchUnavailableActor(
    missingActorError: unknown,
    actorId: string,
    header: ReturnType<typeof decodeStreamHeader>,
    messageKind: ZLinkDispatchMessageKind,
    action: ZLinkDispatchErrorAction,
    returnResponse: boolean,
    fallbackBoundSessionTarget: ZLinkRemoteBoundSessionTarget | undefined,
    fallbackActorRef: ActorRef | undefined
  ): Promise<unknown> {
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.SpotActor,
      messageKind,
      reason: dispatchReasonFromError(missingActorError),
      action,
      packetName: header.name,
      spotId: this.options.spotId(),
      actorId,
      correlationId: header.correlationId ?? header.requestSeq?.toString(),
      error: missingActorError
    });
    if (messageKind !== ZLinkDispatchMessageKind.ActorRequest) {
      return undefined;
    }
    if (
      header.requestSeq !== undefined &&
      !returnResponse &&
      this.options.actorErrorSender !== undefined
    ) {
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

  private createPayloadDecoder(
    message: Message,
    header: ReturnType<typeof decodeStreamHeader>
  ): (messageType?: Type) => unknown {
    return (messageType) =>
      decodeFrameworkTypedPayloadMessage(
        message,
        this.options.messageSerializers,
        messageType,
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
    requestDeadlineUnixMs: number | undefined,
    serial: ZLinkSpotSerialTurnExecutor | undefined,
    workOptions: ZLinkSerialWorkOptions
  ): Promise<unknown> {
    const spot = typeof this.options.spot === 'function' ? this.options.spot() : this.options.spot;
    const dispatcher = new ZLinkSpotActorDispatcher({
      registry: this.options.registry,
      spot,
      providerResolver: this.options.providerResolver,
      serial,
      serialWorkOptions: workOptions,
      messageSerializers: this.options.messageSerializers,
      onAdmitted: () => this.trace(ZLinkMessageFlowOutcome.Admitted, actorId, header, messageKind),
      onHandlerStart: () => {
        if (this.options.admission?.ownerAdmissionOpen() === false) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RequestRejected,
            'Actor handler was rejected because Location owner admission is closed.'
          );
        }
        // A transferred backlog is returned to the Actor queue as one FIFO
        // prefix. Check the request deadline when its owner turn actually
        // starts, not when that prefix is admitted.
        if (
          messageKind === ZLinkDispatchMessageKind.ActorRequest &&
          requestDeadlineUnixMs !== undefined &&
          Date.now() >= requestDeadlineUnixMs
        ) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            `Actor request '${actorId}' expired before handler admission.`
          );
        }
        releaseApplicationJobPermitBeforeHandler();
        this.trace(ZLinkMessageFlowOutcome.Dispatched, actorId, header, messageKind);
      }
    });
    try {
      if (header.kind === ZLinkStreamMessageKind.Send) {
        await dispatcher.dispatchSendDecoded(actor, header.name, decodePayload, {
          meshName: spot.context.meshName,
          metadata: zlinkMessageMetadata(header.metadata),
          correlationId: header.correlationId ?? undefined
        });
        this.trace(
          ZLinkMessageFlowOutcome.Completed,
          actorId,
          header,
          ZLinkDispatchMessageKind.ActorSend
        );
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
        await dispatcher.dispatchRequestThenDecoded(
          actor,
          header.name,
          decodePayload,
          {
            meshName: spot.context.meshName,
            metadata: zlinkMessageMetadata(header.metadata),
            correlationId: header.correlationId ?? header.requestSeq.toString()
          },
          async (response) => {
            this.trace(
              ZLinkMessageFlowOutcome.Replied,
              actorId,
              header,
              ZLinkDispatchMessageKind.ActorRequest
            );
            await requestTerminal(response, preparedReplyReady ? preparedReply : undefined);
          },
          requestTerminal.prepare === undefined
            ? undefined
            : async (response) => {
                preparedReply = await requestTerminal.prepare!(response);
                preparedReplyReady = true;
              }
        );
        return undefined;
      }
      if (returnResponse || this.options.actorResponseSender === undefined) {
        const response = await dispatcher.dispatchRequestDecoded(
          actor,
          header.name,
          decodePayload,
          {
            meshName: spot.context.meshName,
            metadata: zlinkMessageMetadata(header.metadata),
            correlationId: header.correlationId ?? header.requestSeq.toString()
          }
        );
        this.trace(
          ZLinkMessageFlowOutcome.Replied,
          actorId,
          header,
          ZLinkDispatchMessageKind.ActorRequest
        );
        return response;
      }
      await dispatcher.dispatchRequestThenDecoded(
        actor,
        header.name,
        decodePayload,
        {
          meshName: spot.context.meshName,
          metadata: zlinkMessageMetadata(header.metadata),
          correlationId: header.correlationId ?? header.requestSeq.toString()
        },
        async (response, replyOptions) => {
          this.trace(
            ZLinkMessageFlowOutcome.Replied,
            actorId,
            header,
            ZLinkDispatchMessageKind.ActorRequest
          );
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
        }
      );
      return undefined;
    } catch (error) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotActor,
        messageKind,
        reason: dispatchReasonFromError(error),
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
    error?: unknown,
    severity?: 'warning'
  ): void {
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.SpotActor,
      messageKind,
      reason: ZLinkDispatchErrorReason.InvalidFrame,
      action: ZLinkDispatchErrorAction.Drop,
      spotId: this.options.spotId(),
      actorId,
      error,
      severity
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
