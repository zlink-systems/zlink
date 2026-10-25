import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { PacketNames } from '../shared/constants';
import {
  DungeonView, PartyView, JoinDungeon, ActionRejected, EnterDungeonRequest,
  type JoinLobbyRequest,
  type ViewRequest,
  type AdvanceRequest,
  type PrepareDungeon,
  type DungeonPrepared
} from '../shared/messages';
import { Player } from '../shared/player';
import { Dungeon } from './dungeon';
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.joinLobbyRequest
})
export class Reconnect {
  async handle(
    spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinLobbyRequest
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
    request: DungeonView
  ): Promise<void> {
    // Restore the declared DTO class before forwarding (#1731).
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error('Unregistered dungeon push');
    await actor.context.boundSession.send(Object.assign(new type(), request)).submit();
  }
}
const deliveryPackets: ReadonlyMap<string, new () => object> = new Map([
  [PacketNames.dungeonView, DungeonView]
]);
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Dungeon, packetName: PacketNames.prepareDungeon })
export class Prepare implements ZLinkSpotRequestHandler<Dungeon, PrepareDungeon, DungeonPrepared> {
  async handle(spot: Dungeon, request: PrepareDungeon): Promise<DungeonPrepared> {
    return spot.prepare(request);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Dungeon, actor: () => Player, packetName: PacketNames.enterDungeonRequest })
export class Enter { async handle(spot: Dungeon, actor: Player, _context: ZLinkMessageContext, _request: EnterDungeonRequest): Promise<void> { await spot.enter(actor); } }
@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Dungeon, actor: () => Player, packetName: PacketNames.joinDungeon })
export class Travel { async handle(_spot: Dungeon, actor: Player, _context: ZLinkMessageContext, request: JoinDungeon): Promise<void> { actor.context.joinSpot(request.dungeonId, request).defer(); } }
@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Dungeon, actor: () => Player, packetName: PacketNames.partyView })
export class PartyDelivery { async handle(_spot: Dungeon, actor: Player, _context: ZLinkMessageContext, request: PartyView): Promise<void> { await actor.context.boundSession.send(Object.assign(new PartyView(), request)).submit(); } }
@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Dungeon, actor: () => Player, packetName: PacketNames.actionRejected })
export class RejectDelivery { async handle(_spot: Dungeon, actor: Player, _context: ZLinkMessageContext, request: ActionRejected): Promise<void> { await actor.context.boundSession.send(new ActionRejected(request.reason, request.retryAfterMs)).submit(); } }
