import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames, townId } from '../shared/constants';
import {
  JoinTownRequest,
  ChangeChannelRequest,
  CreatePartyRequest,
  JoinPartyRequest,
  JoinDungeon,
  TownView,
  restorePush
} from '../shared/messages';
import { Player } from '../shared/player';
import { Town } from './town';
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.joinTownRequest
})
export class TownReconnect {
  async handle(
    spot: Town,
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
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.viewRequest
})
export class TownQuery {
  async handle(spot: Town, actor: Player): Promise<void> {
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.changeChannelRequest
})
export class ChangeChannel {
  async handle(
    _spot: Town,
    actor: Player,
    _context: ZLinkMessageContext,
    request: ChangeChannelRequest
  ): Promise<void> {
    actor.context
      .joinSpot(
        townId(request.channel),
        new JoinTownRequest(actor.context.actorId, request.channel)
      )
      .defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.createPartyRequest
})
export class CreateParty {
  async handle(
    spot: Town,
    actor: Player,
    _context: ZLinkMessageContext,
    request: CreatePartyRequest
  ): Promise<void> {
    await spot.party(actor, request.partyId, true);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.joinPartyRequest
})
export class JoinParty {
  async handle(
    spot: Town,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinPartyRequest
  ): Promise<void> {
    await spot.party(actor, request.partyId, false);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.enterDungeonRequest
})
export class EnterDungeon {
  async handle(spot: Town, actor: Player): Promise<void> {
    await spot.enter(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.joinDungeon
})
export class DungeonTravel {
  async handle(
    _spot: Town,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinDungeon
  ): Promise<void> {
    actor.context
      .joinSpot(request.dungeonId, new JoinDungeon(request.dungeonId, request.partyId))
      .defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.townView
})
export class TownDelivery {
  async handle(
    _spot: Town,
    actor: Player,
    context: ZLinkMessageContext,
    view: TownView
  ): Promise<void> {
    await actor.context.boundSession.send(restorePush(context.packetName, view)).submit();
  }
}
