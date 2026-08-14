import type {
  RoutingId,
  Type,
  ZLinkSpot,
  ZLinkSpotPacketHandler,
  ZLinkSpotRequestHandler
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkApplicationWorkClaim } from '../admission';
import { ZLinkFrameworkException, zlinkMessageMetadata } from '../../contracts';
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
  createInternalFrameworkException,
  internalFrameworkErrorKind
} from '../framework-errors-internal';
import { resolveLifecycleHandler } from '../handlers/handler-instance-scope';
import type { ZLinkSpotHandlerRegistration } from './spot-handler-registry';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import type { ZLinkSerialWorkOptions } from '../execution/serial-scheduler';
import {
  detachApplicationJobPermit,
  releaseApplicationJobPermitBeforeHandler
} from '../application-jobs/application-job-queue-scope';

interface ZLinkRoutedSpotPacketActivation {
  readonly meshName?: string;
  readonly spotId: RoutingId;
  readonly spot: ZLinkSpot;
  readonly serial: ZLinkSpotSerialExecutor;
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
    return await this.dispatch(spotId, packetName, () => request, context, true) as TReply;
  }

  async requestEncoded<TReply>(
    spotId: RoutingId,
    packetName: string | undefined,
    decodePayload: () => unknown,
    context: ZLinkRoutedSpotPacketContext
  ): Promise<TReply> {
    return await this.dispatch(spotId, packetName, decodePayload, context, true) as TReply;
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

    const registrations = activation.handlers.snapshot().filter((registration) =>
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
    const applicationClaim = activation.meshName === undefined
      ? undefined
      : this.options.claimApplicationWork?.(activation.meshName);
    const runHandler = async () => {
      // Decode only after the Spot has acquired both application admission
      // and its execution authority.
      const payload = decodePayload();
      for (const registration of registrations) {
        const handler = await resolveLifecycleHandler(
          activation.spot,
          registration.handlerType as Type<
            ZLinkSpotPacketHandler<ZLinkSpot, unknown> |
            ZLinkSpotRequestHandler<ZLinkSpot, unknown, unknown>
          >,
          this.options.providerResolver
        );
        releaseApplicationJobPermitBeforeHandler();
        response = await handler.handle(activation.spot, payload, {
          channelName: context.channelName,
          contentType: context.contentType,
          packetName: packetName!,
          metadata: zlinkMessageMetadata({})
        });
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
            (error) => this.reportFailure(spotId, packetName, context, false, error),
            context.workOptions,
            { signal: context.signal }
          );
        } catch (error) {
          detached = false;
          detachedApplicationPermit?.releaseAfterInternalProcessing();
          throw error;
        }
      } else {
        if (activation.serial.isCurrentTurn) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.InvalidOperation,
            `Spot '${spotId}' cannot await a request to its current serial turn.`
          );
        }
        await activation.serial.execute(runHandler, context.workOptions);
      }
    } catch (error) {
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
    const frameworkErrorKind = error instanceof ZLinkFrameworkException
      ? internalFrameworkErrorKind(error)
      : undefined;
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.SpotRoute,
      messageKind: returnResponse ? ZLinkDispatchMessageKind.Request : ZLinkDispatchMessageKind.Send,
      reason: frameworkErrorKind === ZLinkFrameworkInternalErrorKind.WorkerQueueFull
        || frameworkErrorKind === ZLinkFrameworkInternalErrorKind.DeadlineExceeded
        ? ZLinkDispatchErrorReason.Backpressure
        : ZLinkDispatchErrorReason.HandlerException,
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
    context: { readonly channelName: string },
    returnResponse: boolean
  ): void {
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.SpotRoute,
      messageKind: returnResponse ? ZLinkDispatchMessageKind.Request : ZLinkDispatchMessageKind.Send,
      reason: ZLinkDispatchErrorReason.HandlerMissing,
      action: returnResponse ? ZLinkDispatchErrorAction.FailCaller : ZLinkDispatchErrorAction.Drop,
      packetName,
      channelName: context.channelName,
      spotId: String(spotId)
    });
  }
}
