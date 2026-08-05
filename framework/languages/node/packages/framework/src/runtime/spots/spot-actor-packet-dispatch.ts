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
  ZLinkStreamMessageKind
} from '../streams/protocol';
import { decodeFrameworkTypedPayloadMessage } from '../messaging/payload-codec';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import type { ZLinkSerialWorkOptions } from '../execution/serial-scheduler';
import { zlinkMetadataByteLength, zlinkSerialWorkOptions } from '../execution/serial-work-size';

export interface ZLinkActorResponseOptions {
  readonly metadata: ReadonlyMap<string, string>;
  readonly compressPayload: boolean;
}

/** Internal two-phase terminal used when a routed request owns the reply. */
export interface ZLinkActorRequestTerminal {
  (response: unknown, preparedReply?: unknown): Promise<void> | void;
  readonly prepare?: (response: unknown) => Promise<unknown> | unknown;
}

interface ZLinkSpotActorPacketDispatchOptions {
  readonly spot: ZLinkSpot;
  readonly spotId: () => string;
  readonly registry: ZLinkSpotActorHandlerRegistryRuntime;
  readonly serial?: ZLinkSpotSerialExecutor;
  readonly resolveActor: (actorId: string) => ZLinkActor | undefined;
  readonly actorLeft?: (actorId: string) => boolean;
  readonly routeBeforeLocal?: (
    actorId: string,
    parts: readonly Message[],
    returnResponse: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
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
}

export class ZLinkSpotActorPacketDispatch {
  constructor(private readonly options: ZLinkSpotActorPacketDispatchOptions) {}

  async dispatch(
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: ZLinkActorRequestTerminal
  ): Promise<unknown> {
    if (parts.length < 2) {
      this.reportInvalidFrame(actorId, ZLinkDispatchMessageKind.ActorSend);
      return undefined;
    }
    let header: ReturnType<typeof decodeStreamHeader>;
    try {
      header = decodeStreamHeader(messageToBytes(parts[0]));
    } catch (error) {
      this.reportInvalidFrame(actorId, ZLinkDispatchMessageKind.ActorSend, error);
      throw error;
    }
    return await runWithFlow(createInboundFlow(
      header.flowId,
      header.flowOrigin,
      this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
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
        actorId,
        parts,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef
      );
      if (routed?.handled === true) {
        return routed.response;
      }
      const actor = this.options.resolveActor(actorId);
      if (actor === undefined) {
        return this.handleMissingActor(
          actorId,
          header,
          messageKind,
          action,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef
        );
      }
      if (header.name === ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET) {
        this.options.onRemoteBoundSessionTarget?.(actorId, undefined);
        await this.options.onDisconnectActor(actor);
        return undefined;
      }
      const decodePayload = this.createPayloadDecoder(parts[1]);
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
    header: ReturnType<typeof decodeStreamHeader>,
    messageKind: ZLinkDispatchMessageKind,
    action: ZLinkDispatchErrorAction,
    returnResponse: boolean,
    fallbackBoundSessionTarget: ZLinkRemoteBoundSessionTarget | undefined,
    fallbackActorRef: ActorRef | undefined
  ): Promise<undefined> {
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

  private createPayloadDecoder(message: Message): () => unknown {
    return () => decodeFrameworkTypedPayloadMessage(message, this.options.messageSerializers);
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
    const dispatcher = new ZLinkSpotActorDispatcher({
      registry: this.options.registry,
      spot: this.options.spot,
      providerResolver: this.options.providerResolver,
      serial: this.options.serial,
      serialWorkOptions: workOptions,
      messageSerializers: this.options.messageSerializers
    });
    try {
      if (header.kind === ZLinkStreamMessageKind.Send) {
        await dispatcher.dispatchSendDecoded(actor, header.name, decodePayload, {
          meshName: this.options.spot.context.meshName,
          metadata: zlinkMessageMetadata(header.metadata),
          correlationId: header.correlationId ?? undefined
        });
        this.trace(ZLinkMessageFlowOutcome.Dispatched, actorId, header, ZLinkDispatchMessageKind.ActorSend);
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
          meshName: this.options.spot.context.meshName,
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
          meshName: this.options.spot.context.meshName,
          metadata: zlinkMessageMetadata(header.metadata),
          correlationId: header.correlationId ?? header.requestSeq.toString()
        });
        this.trace(ZLinkMessageFlowOutcome.Replied, actorId, header, ZLinkDispatchMessageKind.ActorRequest);
        return response;
      }
      await dispatcher.dispatchRequestThenDecoded(actor, header.name, decodePayload, {
        meshName: this.options.spot.context.meshName,
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
