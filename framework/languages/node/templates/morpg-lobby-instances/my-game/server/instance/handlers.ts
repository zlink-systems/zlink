import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { Names, PacketNames } from '../shared/constants';
import {
  PartyView,
  DungeonView,
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
    _actor: Player,
    _context: ZLinkMessageContext,
    _request: AdvanceRequest
  ): Promise<void> {
    await spot.advance();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.dungeonView
})
export class Delivery {
  handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    request: DungeonView
  ): Promise<void> {
    return actor.deliver(request.dungeonId, context.packetName, request);
  }
}
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
  packetName: PacketNames.partyView
})
export class DepartedPartyView {
  handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    message: PartyView
  ): Promise<void> {
    return actor.deliver(Names.partyPrefix + message.partyId, context.packetName, message);
  }
}
