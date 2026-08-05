import * as fs from 'node:fs';
import { Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler, zlinkSpotSubscriptionHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkPublishMessageContext,
  ZLinkSpotSubscriptionHandler,
  ZLinkSpotPacketHandler,
  ZLinkMessageContext,
  ZLinkSpotTimerHandler,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import { PacketNames, WorldAnnounceNotify } from '../../../../../Shared/contracts';
import { ZoneIds, ZoneWorldNames } from '../../../../../Shared/spec';
import type { DeliverAnnounceMsg, ZoneBorderEvent } from '../../../../../Shared/contracts';
import { ZoneSpot } from '../Spots/zone-spot';
import { adjacentZones } from '../../../Domain/world';
import { Inject } from '@nestjs/common';
import { ZONEWORLD_CONFIG } from '../../../../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../../../../Configuration/configuration';
import { OpsReportAdapter } from '../Monitoring/ops-report-adapter';

@Injectable()
class ZoneTickHandler implements ZLinkSpotTimerHandler<ZoneSpot> {
  private faultInjected = false;

  constructor(
    @Inject(ZONEWORLD_CONFIG) private readonly config: ZoneWorldConfiguration,
    private readonly ops: OpsReportAdapter
  ) {}

  async handle(spot: ZoneSpot, _tick: ZLinkTimerTick): Promise<void> {
    try {
      const zoneId = String(spot.context.spotId);
      const faultSignalPath = this.config.zoneNode?.faultTickSignalPath;
      const faultIsArmed = faultSignalPath === undefined || fs.existsSync(faultSignalPath);
      if (this.config.zoneNode?.faultTickZone === zoneId && !this.faultInjected && faultIsArmed) {
        this.faultInjected = true;
        console.log(`zone tick fault injected zone=${zoneId}`);
        throw new Error(`injected tick failure for ${zoneId}`);
      }
      await spot.tick();
    } catch (error) {
      const zoneId = String(spot.context.spotId);
      try {
        console.log(`zone spot event reporting zone=${zoneId}`);
        await this.ops.reportSpotEvent(
          this.config.zoneNode?.nodeId ?? 'unknown',
          'TimerHandlerFailed',
          `spot=${zoneId}; timer=zone-tick-${zoneId}; detail=${error instanceof Error ? error.message : String(error)}`
        );
        console.log(`zone spot event reported zone=${zoneId}`);
      } catch (reportError) {
        // The Framework still owns timer failure logging. A report transport
        // failure must not replace the original handler exception or change
        // the timer policy.
        console.error(
          `zone spot event report failed. zone=${zoneId}`
          + ` error=${reportError instanceof Error ? reportError.message : String(reportError)}`
        );
      }
      throw error;
    }
  }
}

@Injectable()
class BotTickHandler implements ZLinkSpotTimerHandler<ZoneSpot> {
  async handle(spot: ZoneSpot, _tick: ZLinkTimerTick): Promise<void> {
    await spot.tickBots();
  }
}

@Injectable()
@zoneBorderSubscriptionHandler()
class FirstBorderSubscriptionHandler implements ZLinkSpotSubscriptionHandler<ZoneSpot, ZoneBorderEvent> {
  async handle(spot: ZoneSpot, event: ZoneBorderEvent, _context: ZLinkPublishMessageContext): Promise<void> {
    if (event.toZoneId !== String(spot.context.spotId)) return;
    spot.applyBorder(event);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => ZoneSpot, packetName: PacketNames.deliverAnnounceMsg })
class DeliverAnnounceHandler implements ZLinkSpotPacketHandler<ZoneSpot, DeliverAnnounceMsg> {
  async handle(spot: ZoneSpot, message: DeliverAnnounceMsg, _context: ZLinkMessageContext): Promise<void> {
    await spot.pushHumans(new WorldAnnounceNotify(message.announcementId, message.text));
    console.log(`zone spot announcement delivered zone=${String(spot.context.spotId)} id=${message.announcementId}`);
  }
}

class UpdateZonePositionMsg {
  constructor(readonly actorId: string, readonly x: number, readonly y: number) {}
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => ZoneSpot, packetName: 'UpdateZonePositionMsg' })
class UpdateZonePositionHandler implements ZLinkSpotPacketHandler<ZoneSpot, UpdateZonePositionMsg> {
  async handle(spot: ZoneSpot, message: UpdateZonePositionMsg, _context: ZLinkMessageContext): Promise<void> {
    spot.updatePosition(message.actorId, message.x, message.y);
  }
}

export {
  BotTickHandler,
  DeliverAnnounceHandler,
  FirstBorderSubscriptionHandler,
  UpdateZonePositionHandler,
  UpdateZonePositionMsg,
  ZoneTickHandler
};

function zoneBorderSubscriptionHandler(): ClassDecorator {
  const topics = Object.values(ZoneIds).flatMap((from) =>
    adjacentZones(from).map((to) => ZoneWorldNames.borderTopic(from, to))
  );
  return (target) => {
    for (const topic of topics) {
      zlinkSpotSubscriptionHandler({
        spot: () => ZoneSpot,
        channelName: ZoneWorldNames.bridgeMesh,
        topic
      })(target);
    }
  };
}
