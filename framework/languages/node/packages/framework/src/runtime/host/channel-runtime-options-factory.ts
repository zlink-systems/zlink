import {
  ZLINK_REMOTE_ACTOR_JOIN_PACKET,
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET
} from '../actors';
import type { ZLinkChannelRuntimeManagerOptions } from '../channels';
import type { ZLinkMonitoringBackendAdapter } from '../backend';
import type { DefaultZLinkSpotManager } from '../spots';
import type { ZLinkMessageFlowModeCell } from '../diagnostics';
import type { ZLinkBoundSessionRelay } from './bound-session-relay';
import type { ZLinkInboundDispatchBudget } from '../dispatch/inbound-dispatch-budget';

export interface ZLinkChannelRuntimeOptionsFactoryOptions {
  readonly monitoringAdapter: ZLinkMonitoringBackendAdapter;
  readonly messageFlowModeCell: ZLinkMessageFlowModeCell;
  readonly boundSessionRelay: ZLinkBoundSessionRelay;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
  readonly oneWayFailureSink: (error: unknown) => void;
  readonly inboundDispatchBudget: ZLinkInboundDispatchBudget;
}

export class ZLinkChannelRuntimeOptionsFactory {
  constructor(private readonly options: ZLinkChannelRuntimeOptionsFactoryOptions) {}

  create(): ZLinkChannelRuntimeManagerOptions {
    return {
      monitoringAdapter: this.options.monitoringAdapter,
      messageFlowModeCell: this.options.messageFlowModeCell,
      oneWayFailureSink: this.options.oneWayFailureSink,
      inboundDispatchBudget: this.options.inboundDispatchBudget,
      internalRouteSendHandlers: this.internalRouteSendHandlers(),
      internalRouteRequestHandlers: this.internalRouteRequestHandlers(),
      localSpotRouteDispatcher: {
        send: async (spotId, packetName, message, routeContext) => {
          await this.requireSpotManager().dispatchRoutedSpotSend(spotId, packetName, message, routeContext);
        },
        request: async (spotId, packetName, request, routeContext) =>
          await this.requireSpotManager().dispatchRoutedSpotRequest(spotId, packetName, request, routeContext)
      }
    };
  }

  private internalRouteSendHandlers(): ZLinkChannelRuntimeManagerOptions['internalRouteSendHandlers'] {
    return new Map([
      [ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET, {
        handle: async (payload) => {
          await this.options.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSend(payload);
        }
      }],
      [ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET, {
        handle: async (payload) => {
          await this.options.boundSessionRelay.boundSessions.receiveRemoteBoundSessionResponse(payload);
        }
      }],
      [ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET, {
        handle: async (payload) => {
          await this.options.boundSessionRelay.boundSessions.receiveRemoteBoundSessionError(payload);
        }
      }],
      [ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET, {
        handle: async (payload, routeContext) => {
          await this.options.boundSessionRelay.actorPackets.receiveRemoteActorPacketRelay(payload, routeContext);
        }
      }]
    ]);
  }

  private internalRouteRequestHandlers(): ZLinkChannelRuntimeManagerOptions['internalRouteRequestHandlers'] {
    return new Map([
      [ZLINK_REMOTE_ACTOR_JOIN_PACKET, {
        handle: (payload, routeContext) =>
          this.options.boundSessionRelay.actorJoins.receive(payload, routeContext)
      }],
      [ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET, {
        handle: (payload, routeContext) =>
          this.options.boundSessionRelay.actorPackets.receiveRemoteActorPacketRelay(payload, routeContext)
      }]
    ]);
  }

  private requireSpotManager(): DefaultZLinkSpotManager {
    const manager = this.options.spotManager();
    if (manager === undefined) {
      throw new Error('Local SPOT route dispatch requires ZLINK_SPOT_MANAGER.');
    }
    return manager;
  }
}
