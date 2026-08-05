import { Inject, Injectable, Optional } from '@nestjs/common';
import { zlinkPublishHandler, zlinkRequestHandler } from '@zlink-systems/nestjs';
import {
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND
} from '@zlink-systems/nestjs';
import { ZONEWORLD_CONFIG } from '../../../../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../../../../Configuration/configuration';
import { zonesOf } from '../../../../../Shared/spec';
import { DeliverAnnounceMsg, PacketNames } from '../../../../../Shared/contracts';
import type {
  ApplyNodeMaintenanceReq,
  ApplyNodeMaintenanceRes,
  GetNodeDiagnosticsReq,
  GetNodeDiagnosticsRes
} from '../../../../../Shared/contracts';
import type {
  ZLinkPublishMessageContext,
  ZLinkFanoutHandler,
  ZLinkMessageContext,
  ZLinkRequestHandler,
  ZLinkSpotManager,
  ZLinkSpotOutbound
} from '@zlink-systems/framework';
import type { NodeMaintenanceChangedEvent, WorldAnnounceEvent } from '../../../../../Shared/contracts';
import { NodeRuntimeState } from '../../../Domain/node-runtime-state';

@Injectable()
@zlinkRequestHandler('zone-ops', PacketNames.applyNodeMaintenanceReq)
class ApplyNodeMaintenanceHandler implements
  ZLinkRequestHandler<ApplyNodeMaintenanceReq, ApplyNodeMaintenanceRes> {
  constructor(
    @Inject(ZONEWORLD_CONFIG) private readonly config: ZoneWorldConfiguration,
    private readonly state: NodeRuntimeState
  ) {}

  async handle(request: ApplyNodeMaintenanceReq, _context: ZLinkMessageContext): Promise<ApplyNodeMaintenanceRes> {
    const nodeId = this.nodeId();
    if (request.nodeId !== nodeId) throw new Error(`Maintenance request targets '${request.nodeId}', not '${nodeId}'.`);
    this.state.setMaintenance(nodeId, request.enabled);
    return { nodeId, enabled: request.enabled, zones: [...zonesOf(nodeId)] };
  }

  private nodeId(): string {
    if (this.config.zoneNode === undefined) throw new Error('ZoneNode configuration is required.');
    return this.config.zoneNode.nodeId;
  }
}

@Injectable()
@zlinkRequestHandler('zone-ops', PacketNames.getNodeDiagnosticsReq)
class GetNodeDiagnosticsHandler implements
  ZLinkRequestHandler<GetNodeDiagnosticsReq, GetNodeDiagnosticsRes> {
  constructor(
    @Inject(ZONEWORLD_CONFIG) private readonly config: ZoneWorldConfiguration,
    private readonly state: NodeRuntimeState
  ) {}

  async handle(request: GetNodeDiagnosticsReq, _context: ZLinkMessageContext): Promise<GetNodeDiagnosticsRes> {
    const nodeId = this.config.zoneNode?.nodeId;
    if (nodeId === undefined || request.nodeId !== nodeId) throw new Error('Diagnostics request targets another node.');
    return {
      nodeId,
      zones: [...zonesOf(nodeId)],
      playerCount: this.state.playerCount(),
      maintenance: this.state.ownMaintenance()
    };
  }
}

@Injectable()
@zlinkPublishHandler('zone-broadcast', PacketNames.worldAnnounceEvent)
class WorldAnnounceSubscriber implements ZLinkFanoutHandler<WorldAnnounceEvent> {
  constructor(
    @Inject(ZONEWORLD_CONFIG) private readonly config: ZoneWorldConfiguration,
    @Optional() @Inject(ZLINK_SPOT_MANAGER) private readonly handles: ZLinkSpotManager | undefined,
    @Optional() @Inject(ZLINK_SPOT_OUTBOUND) private readonly outbound: ZLinkSpotOutbound | undefined
  ) {}

  async handle(message: WorldAnnounceEvent, context: ZLinkPublishMessageContext): Promise<void> {
    console.log(`fanout subscriber received announcement id=${message.announcementId} topic=${context.topic}`);
    const nodeId = this.config.zoneNode?.nodeId;
    if (nodeId === undefined || this.handles === undefined || this.outbound === undefined) return;
    for (const zoneId of zonesOf(nodeId)) {
      const handle = await this.handles.find(zoneId);
      if (handle !== undefined) {
        await this.outbound
          .sendToSpot(handle.spotId, new DeliverAnnounceMsg(message.announcementId, message.text))
          .submit();
      }
    }
  }
}

@Injectable()
@zlinkPublishHandler('zone-broadcast', PacketNames.nodeMaintenanceChangedEvent)
class MaintenanceChangedSubscriber implements ZLinkFanoutHandler<NodeMaintenanceChangedEvent> {
  constructor(private readonly state: NodeRuntimeState) {}

  async handle(message: NodeMaintenanceChangedEvent, context: ZLinkPublishMessageContext): Promise<void> {
    this.state.setMaintenance(message.nodeId, message.enabled);
    console.log(`maintenance cache updated node=${message.nodeId} enabled=${message.enabled} topic=${context.topic}`);
  }
}

export {
  ApplyNodeMaintenanceHandler,
  GetNodeDiagnosticsHandler,
  MaintenanceChangedSubscriber,
  WorldAnnounceSubscriber
};
