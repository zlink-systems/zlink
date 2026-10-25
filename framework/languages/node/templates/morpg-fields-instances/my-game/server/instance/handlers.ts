import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames } from '../shared/constants';
import {
  JoinWorldRequest,
  ViewRequest,
  AdvanceRequest,
  DungeonView,
  PrepareDungeon,
  DungeonPrepared, PartyView, EnterDungeonRequest, JoinDungeon
} from '../shared/messages';
import { Player } from '../shared/player';
import { Dungeon } from './spots';
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.joinWorldRequest
})
export class Reconnect {
  async handle(
    spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinWorldRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.viewRequest
})
export class View {
  async handle(
    spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: ViewRequest
  ): Promise<void> {
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
  async handle(
    spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: AdvanceRequest
  ): Promise<void> {
    await spot.advance(actor);
  }
}
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Dungeon, packetName: PacketNames.prepareDungeon })
export class Prepare {
  async handle(
    spot: Dungeon,
    request: PrepareDungeon,
    _context: ZLinkMessageContext
  ): Promise<DungeonPrepared> {
    return spot.prepare(request);
  }
}
const deliveryPackets: ReadonlyMap<string, new () => object> = new Map<string, new () => object>([
  [PacketNames.partyView, PartyView],
  [PacketNames.dungeonView, DungeonView]
]);
@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Dungeon, actor: () => Player, packetName: PacketNames.partyView })
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.dungeonView
})
export class Delivery {
  async handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    message: DungeonView | PartyView
  ): Promise<void> {
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error(`Unregistered dungeon push: ${context.packetName}`);
    // Restore the declared public DTO class before forwarding (#1731).
    await actor.context.boundSession.send(Object.assign(new type(), message)).submit();
  }
}

@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Dungeon, actor: () => Player, packetName: PacketNames.enterDungeonRequest })
export class EnterAgain {
  async handle(spot: Dungeon, actor: Player, _context: ZLinkMessageContext, _request: EnterDungeonRequest): Promise<void> {
    await spot.enter(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Dungeon, actor: () => Player, packetName: PacketNames.joinDungeon })
export class TravelAgain {
  async handle(spot: Dungeon, actor: Player, _context: ZLinkMessageContext, request: JoinDungeon): Promise<void> {
    if (request.dungeonId !== String(spot.context.spotId)) actor.context.joinSpot(request.dungeonId, request).defer();
  }
}
