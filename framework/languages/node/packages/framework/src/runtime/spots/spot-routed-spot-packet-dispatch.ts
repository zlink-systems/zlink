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
import { resolveLifecycleHandler } from '../handlers/handler-instance-scope';
import type { ZLinkSpotHandlerRegistration } from './spot-handler-registry';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import type { ZLinkSerialWorkOptions } from '../execution/serial-scheduler';

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
    try {
      const applicationClaim = activation.meshName === undefined
        ? undefined
        : this.options.claimApplicationWork?.(activation.meshName);
      try {
        await activation.serial.execute(async () => {
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
            response = await handler.handle(activation.spot, payload, {
              channelName: context.channelName,
              contentType: context.contentType,
              packetName: packetName!,
              metadata: zlinkMessageMetadata({})
            });
          }
        }, context.workOptions);
      } finally {
        applicationClaim?.close();
      }
    } catch (error) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotRoute,
        messageKind: returnResponse ? ZLinkDispatchMessageKind.Request : ZLinkDispatchMessageKind.Send,
        reason: ZLinkDispatchErrorReason.HandlerException,
        action: returnResponse ? ZLinkDispatchErrorAction.FailCaller : ZLinkDispatchErrorAction.Drop,
        packetName,
        channelName: context.channelName,
        spotId: String(spotId),
        error
      });
      throw error;
    }
    return returnResponse ? response : undefined;
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
