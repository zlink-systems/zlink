import { dispatchReasonFromError } from '../diagnostics/dispatch-error-details';
import type {
  RoutingId,
  Type,
  ZLinkSpot,
  ZLinkSpotPacketHandler,
  ZLinkSpotRequestHandler
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkApplicationWorkClaim } from '../admission';
import { zlinkMessageMetadata } from '../../contracts';
import {
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { ZLinkDispatchErrorReporter } from '../channels';
import { ZLinkConfigurationException } from '../configuration';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { resolveLifecycleHandler } from '../handlers/handler-instance-scope';
import type { ZLinkSpotHandlerRegistration } from './spot-handler-registry';
import type { ZLinkSpotSerialTurnExecutor } from './spot-serial-turn-executor';
import type { ZLinkSerialWorkOptions } from '../execution/serial-execution-queue';
import {
  detachApplicationJobPermit,
  releaseApplicationJobPermitBeforeHandler
} from '../application-jobs/application-job-queue-scope';

interface ZLinkRoutedSpotPacketActivation {
  readonly objectGeneration?: bigint;
  readonly meshName?: string;
  readonly spotId: RoutingId;
  readonly spot: ZLinkSpot;
  readonly serial: ZLinkSpotSerialTurnExecutor;
  readonly handlers: {
    snapshot(): readonly ZLinkSpotHandlerRegistration[];
  };
}

interface ZLinkRoutedSpotPacketDispatchOptions {
  readonly resolveActivation: (spotId: RoutingId) => ZLinkRoutedSpotPacketActivation | undefined;
  readonly claimApplicationWork?: (meshName: string) => ZLinkApplicationWorkClaim;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
}

interface ZLinkRoutedSpotPacketContext {
  readonly channelName: string;
  readonly contentType?: string;
  readonly workOptions?: ZLinkSerialWorkOptions;
  readonly admissionTimeoutMs?: number;
  readonly signal?: AbortSignal;
  /** Wait until a recovered activation has actually entered its first handler. */
  readonly awaitFirstHandlerTurn?: boolean;
  readonly onOneWayError?: (error: unknown) => void;
  /** Retains the caller's Instance address intent through an incarnation boundary. */
  readonly activationRecord?: import('../foundation/service-runtime-contracts').ReceiveRecord;
}

export class ZLinkRoutedSpotPacketDispatch {
  constructor(private readonly options: ZLinkRoutedSpotPacketDispatchOptions) {}

  async send(
    spotId: RoutingId,
    packetName: string | undefined,
    message: unknown,
    context: ZLinkRoutedSpotPacketContext
  ): Promise<void> {
    await this.dispatch(spotId, packetName, () => message, context, false);
  }

  async sendEncoded(
    spotId: RoutingId,
    packetName: string | undefined,
    decodePayload: () => unknown,
    context: ZLinkRoutedSpotPacketContext
  ): Promise<void> {
    await this.dispatch(spotId, packetName, decodePayload, context, false);
  }

  async request<TReply>(
    spotId: RoutingId,
    packetName: string | undefined,
    request: unknown,
    context: ZLinkRoutedSpotPacketContext
  ): Promise<TReply> {
    return (await this.dispatch(spotId, packetName, () => request, context, true)) as TReply;
  }

  async requestEncoded<TReply>(
    spotId: RoutingId,
    packetName: string | undefined,
    decodePayload: () => unknown,
    context: ZLinkRoutedSpotPacketContext
  ): Promise<TReply> {
    return (await this.dispatch(spotId, packetName, decodePayload, context, true)) as TReply;
  }

  private async dispatch(
    spotId: RoutingId,
    packetName: string | undefined,
    decodePayload: () => unknown,
    context: ZLinkRoutedSpotPacketContext,
    returnResponse: boolean
  ): Promise<unknown> {
    const activation = this.options.resolveActivation(spotId);
    if (activation === undefined) {
      this.reportMissing(spotId, packetName, context, returnResponse);
      if (!returnResponse) {
        return undefined;
      }
      throw new ZLinkConfigurationException(`Spot '${spotId}' is not active.`);
    }

    const registrations = activation.handlers
      .snapshot()
      .filter(
        (registration) =>
          registration.kind === 'packet' &&
          (registration.packetName ?? registration.handlerType.name) === (packetName ?? '')
      );
    if (registrations.length === 0) {
      this.reportMissing(spotId, packetName, context, returnResponse);
      if (!returnResponse) {
        return undefined;
      }
      throw new ZLinkConfigurationException(`SPOT route handler not found: ${packetName}`);
    }

    let response: unknown;
    let detached = false;
    let resolveFirstHandlerTurn: (() => void) | undefined;
    let rejectFirstHandlerTurn: ((error: unknown) => void) | undefined;
    const firstHandlerTurn =
      context.awaitFirstHandlerTurn === true
        ? new Promise<void>((resolve, reject) => {
            resolveFirstHandlerTurn = resolve;
            rejectFirstHandlerTurn = reject;
          })
        : undefined;
    void firstHandlerTurn?.catch(() => undefined);
    const applicationClaim =
      activation.meshName === undefined
        ? undefined
        : this.options.claimApplicationWork?.(activation.meshName);
    const originalRecord = context.activationRecord?.activationRecord;
    const instanceIntent =
      originalRecord?.activation === 'missing' ||
      (originalRecord?.activation === 'ready' && originalRecord.instanceIntent);
    const runHandler = async (failure?: unknown) => {
      try {
        if (failure !== undefined) throw failure;
        const current = this.options.resolveActivation(spotId);
        if (
          current === undefined ||
          (!instanceIntent &&
            (current !== activation ||
              (originalRecord?.activation === 'ready' &&
                originalRecord.route.objectGeneration !== current.objectGeneration)))
        ) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
            `Spot '${String(spotId)}' is not active.`
          );
        }
        if (
          context.activationRecord?.deadlineUnixMs !== undefined &&
          context.activationRecord.deadlineUnixMs < BigInt(Date.now())
        ) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            `Spot '${String(spotId)}' request deadline has expired.`
          );
        }
        const currentRegistrations =
          current === activation
            ? registrations
            : current.handlers
                .snapshot()
                .filter(
                  (registration) =>
                    registration.kind === 'packet' &&
                    (registration.packetName ?? registration.handlerType.name) ===
                      (packetName ?? '')
                );
        // Decode only after the Spot has acquired both application admission
        // and its execution authority.
        const payload = decodePayload();
        for (const registration of currentRegistrations) {
          const handler = await resolveLifecycleHandler(
            current.spot,
            registration.handlerType as Type<
              | ZLinkSpotPacketHandler<ZLinkSpot, unknown>
              | ZLinkSpotRequestHandler<ZLinkSpot, unknown, unknown>
            >,
            this.options.providerResolver
          );
          resolveFirstHandlerTurn?.();
          resolveFirstHandlerTurn = undefined;
          context.activationRecord?.onHandlerTurnStarted?.();
          releaseApplicationJobPermitBeforeHandler();
          response = await handler.handle(current.spot, payload, {
            channelName: context.channelName,
            contentType: context.contentType,
            packetName: packetName!,
            metadata: zlinkMessageMetadata({})
          });
        }
      } catch (error) {
        rejectFirstHandlerTurn?.(error);
        throw error;
      }
    };
    try {
      if (!returnResponse) {
        detached = true;
        const detachedApplicationPermit = detachApplicationJobPermit();
        try {
          await activation.serial.postOneWay(
            async () => {
              try {
                await runHandler();
              } finally {
                detachedApplicationPermit?.releaseAfterInternalProcessing();
                applicationClaim?.close();
              }
            },
            (error) => {
              if (context.onOneWayError === undefined) {
                this.reportFailure(spotId, packetName, context, false, error);
              } else if (resolveFirstHandlerTurn === undefined) {
                context.onOneWayError(error);
              }
            },
            context.workOptions,
            { signal: context.signal },
            instanceIntent
              ? async (failure?: unknown) => {
                  try {
                    await runHandler(failure);
                  } finally {
                    detachedApplicationPermit?.releaseAfterInternalProcessing();
                    applicationClaim?.close();
                  }
                }
              : undefined
          );
        } catch (error) {
          detached = false;
          detachedApplicationPermit?.releaseAfterInternalProcessing();
          rejectFirstHandlerTurn?.(error);
          throw error;
        }
        await firstHandlerTurn;
      } else {
        if (activation.serial.isCurrentTurn) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.InvalidOperation,
            `Spot '${spotId}' cannot await a request to its current serial turn.`
          );
        }
        await activation.serial.execute(
          runHandler,
          context.workOptions,
          instanceIntent ? runHandler : undefined
        );
      }
    } catch (error) {
      if (context.activationRecord?.activationRecord === undefined)
        this.reportFailure(spotId, packetName, context, returnResponse, error);
      throw error;
    } finally {
      if (!detached) applicationClaim?.close();
    }
    return returnResponse ? response : undefined;
  }

  private reportFailure(
    spotId: RoutingId,
    packetName: string | undefined,
    context: ZLinkRoutedSpotPacketContext,
    returnResponse: boolean,
    error: unknown
  ): void {
    const reporter = this.options.dispatchErrors;
    if (reporter?.captureEnabled() !== true) return;
    reporter.report({
      surface:
        context.activationRecord?.activationRecord?.kind === 'instanceSpot'
          ? ZLinkDispatchErrorSurface.InstanceSpot
          : ZLinkDispatchErrorSurface.SpotRoute,
      messageKind: returnResponse
        ? ZLinkDispatchMessageKind.Request
        : ZLinkDispatchMessageKind.Send,
      reason: dispatchReasonFromError(error),
      action: returnResponse ? ZLinkDispatchErrorAction.FailCaller : ZLinkDispatchErrorAction.Drop,
      packetName,
      channelName: context.channelName,
      spotId: String(spotId),
      error
    });
  }

  private reportMissing(
    spotId: RoutingId,
    packetName: string | undefined,
    context: ZLinkRoutedSpotPacketContext,
    returnResponse: boolean
  ): void {
    if (context.activationRecord?.activationRecord !== undefined) {
      if (!returnResponse && context.onOneWayError !== undefined)
        context.onOneWayError(
          createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.HandlerNotFound,
            `SPOT route handler not found: ${packetName}`
          )
        );
      return;
    }
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.SpotRoute,
      messageKind: returnResponse
        ? ZLinkDispatchMessageKind.Request
        : ZLinkDispatchMessageKind.Send,
      reason: ZLinkDispatchErrorReason.HandlerMissing,
      action: returnResponse ? ZLinkDispatchErrorAction.FailCaller : ZLinkDispatchErrorAction.Drop,
      packetName,
      channelName: context.channelName,
      spotId: String(spotId)
    });
  }
}
