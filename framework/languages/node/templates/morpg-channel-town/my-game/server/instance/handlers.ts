import { Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler, ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames } from '../shared/constants';
import {
  PrepareDungeon,
  DungeonPrepared,
  DungeonView,
  restorePush,
  JoinTownRequest
} from '../shared/messages';
import { Player } from '../shared/player';
import { Dungeon } from './dungeon';
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Dungeon, packetName: PacketNames.prepareDungeon })
export class Prepare implements ZLinkSpotRequestHandler<Dungeon, PrepareDungeon, DungeonPrepared> {
  async handle(spot: Dungeon, request: PrepareDungeon): Promise<DungeonPrepared> {
    return spot.prepare(request);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.joinTownRequest
})
export class DungeonReconnect {
  async handle(
    spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinTownRequest
  ): Promise<void> {
    actor.assertIdentity(request.playerId);
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.viewRequest
})
export class DungeonQuery {
  async handle(spot: Dungeon, actor: Player): Promise<void> {
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.advanceRequest
})
export class Advance {
  async handle(spot: Dungeon): Promise<void> {
    await spot.advance();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.dungeonView
})
export class DungeonDelivery {
  async handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    view: DungeonView
  ): Promise<void> {
    await actor.context.boundSession.send(restorePush(context.packetName, view)).submit();
  }
}
