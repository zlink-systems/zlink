import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_FANOUT_CLIENT, ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import { zlinkSendHandler } from '@zlink-systems/nestjs';
import { ZLinkPacket } from '@zlink-systems/framework';
import {
  AnnounceWorldRes,
  ApplyNodeMaintenanceReq,
  GetNodeDiagnosticsReq,
  NodeDiagnosticsRes,
  NodeAlertNotify,
  NodeMaintenanceChangedEvent,
  SetMaintenanceRes,
  WatchNodesRes,
  WorldAnnounceEvent
} from '../../Shared/contracts';
import { PacketNames } from '../../Shared/contracts';
import { ZoneWorldErrors, ZoneWorldNames } from '../../Shared/spec';
import { NodeRegistry } from './node-registry';
import { OpsConsoleRegistry } from './ops-console-registry';
import { MaintenanceStore } from '../Configuration/maintenance-store';
import type {
  AnnounceWorldReq,
  ApplyNodeMaintenanceRes,
  GetNodeDiagnosticsRes,
  NodeDiagnosticsReq,
  ReportNodeStatusMsg,
  ReportSpotEventMsg,
  SetMaintenanceReq
} from '../../Shared/contracts';
import type {
  ZLinkFanoutClient,
  ZLinkMessage,
  ZLinkMessageContext,
  ZLinkRouteMessageContext,
  ZLinkRouteClient,
  ZLinkRouteSendHandler,
  ZLinkSendHandler,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext
} from '@zlink-systems/framework';

@Injectable()
@zlinkSendHandler('ops', PacketNames.reportNodeStatusMsg)
class ReportNodeStatusHandler implements ZLinkRouteSendHandler<ReportNodeStatusMsg> {
  constructor(private readonly nodes: NodeRegistry, private readonly consoles: OpsConsoleRegistry) {}

  async handle(message: ReportNodeStatusMsg, context: ZLinkRouteMessageContext): Promise<void> {
    console.log(`node status received node=${message.nodeId}`);
    this.consoles.publish(this.nodes.report(message, context.sourceNodeRid));
  }
}

@Injectable()
@zlinkSendHandler('ops', PacketNames.reportSpotEventMsg)
class ReportSpotEventHandler implements ZLinkSendHandler<ReportSpotEventMsg> {
  constructor(private readonly consoles: OpsConsoleRegistry) {}

  async handle(message: ReportSpotEventMsg, _context: ZLinkMessageContext): Promise<void> {
    this.consoles.publishAlert(new NodeAlertNotify(
      message.nodeId,
      message.kind as never,
      message.detail,
      message.occurredAt
    ));
  }
}

@Injectable()
@ZLinkPacket(PacketNames.watchNodesReq)
class WatchNodesHandler {
  constructor(private readonly nodes: NodeRegistry, private readonly consoles: OpsConsoleRegistry) {}

  async handle(context: ZLinkSessionContext): Promise<void> {
    context.client.reply(new WatchNodesRes(this.nodes.snapshot())).submit();
    this.consoles.replayAlerts(context);
  }
}

@Injectable()
@ZLinkPacket(PacketNames.announceWorldReq)
class AnnounceWorldHandler {
  constructor(@Inject(ZLINK_FANOUT_CLIENT) private readonly fanout: ZLinkFanoutClient) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode<AnnounceWorldReq>(Object as never);
    const id = `announce-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    await this.fanout
      .publish(ZoneWorldNames.broadcastChannel, new WorldAnnounceEvent(id, request.text))
      .submit();
    context.client.reply(new AnnounceWorldRes(id)).submit();
  }
}

@Injectable()
@ZLinkPacket(PacketNames.setMaintenanceReq)
class SetMaintenanceHandler {
  constructor(
    @Inject(ZLINK_ROUTE_CLIENT) private readonly channels: ZLinkRouteClient,
    @Inject(ZLINK_FANOUT_CLIENT) private readonly fanout: ZLinkFanoutClient,
    private readonly maintenance: MaintenanceStore
  ) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode<SetMaintenanceReq>(Object as never);
    await this.maintenance.write(request.nodeId, request.enabled);
    try {
      const applied = await this.channels
        .requestToChannel(
          ZoneWorldNames.opsChannel(request.nodeId),
          new ApplyNodeMaintenanceReq(request.nodeId, request.enabled)
        )
        .timeout(3_000)
        .submit<ApplyNodeMaintenanceRes>();
      await this.fanout.publish(
        ZoneWorldNames.broadcastChannel,
        new NodeMaintenanceChangedEvent(request.nodeId, request.enabled)
      ).submit();
      context.client.reply(new SetMaintenanceRes(applied.nodeId, applied.enabled, applied.zones)).submit();
    } catch {
      context.client.reply(new SetMaintenanceRes(request.nodeId, request.enabled, [], ZoneWorldErrors.nodeUnavailable)).submit();
    }
  }
}

@Injectable()
@ZLinkPacket(PacketNames.nodeDiagnosticsReq)
class NodeDiagnosticsHandler {
  constructor(@Inject(ZLINK_ROUTE_CLIENT) private readonly channels: ZLinkRouteClient) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode<NodeDiagnosticsReq>(Object as never);
    try {
      const result = await this.channels
        .requestToChannel(
          ZoneWorldNames.opsChannel(request.nodeId),
          new GetNodeDiagnosticsReq(request.nodeId)
        )
        .timeout(3_000)
        .submit<GetNodeDiagnosticsRes>();
      context.client.reply(new NodeDiagnosticsRes(
        result.nodeId,
        result.zones,
        result.playerCount,
        result.maintenance
      )).submit();
    } catch {
      context.client.reply(new NodeDiagnosticsRes(request.nodeId, [], 0, false, ZoneWorldErrors.nodeUnavailable)).submit();
    }
  }
}

export {
  AnnounceWorldHandler,
  NodeDiagnosticsHandler,
  ReportNodeStatusHandler,
  ReportSpotEventHandler,
  SetMaintenanceHandler,
  WatchNodesHandler
};
