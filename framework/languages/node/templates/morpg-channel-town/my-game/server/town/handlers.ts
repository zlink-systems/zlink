import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames } from '../shared/constants';
import {
  JoinTownRequest,
  ChangeChannelRequest,
  JoinPartyRequest,
  JoinDungeon,
  PartyViewRequest,
  RunStarted,
  RunCompleted,
  ActionRejected,
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
    spot: Town,
    actor: Player,
    _context: ZLinkMessageContext,
    request: ChangeChannelRequest
  ): Promise<void> {
    await spot.changeChannel(actor, request.channel);
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
    _context: ZLinkMessageContext
  ): Promise<void> {
    await spot.party(actor, null, true);
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
      .joinSpot(request.dungeonId, new JoinDungeon(request.dungeonId, request.runId))
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

@Injectable()
@zlinkSpotPacketHandler({ spot: () => Town, packetName: PacketNames.runStarted })
export class RunStartedHandler {
  async handle(spot: Town, message: RunStarted): Promise<void> {
    await spot.runStarted(message);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => Town, packetName: PacketNames.runCompleted })
export class RunCompletedHandler {
  async handle(spot: Town, message: RunCompleted): Promise<void> {
    await spot.runCompleted(message);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => Town, packetName: PacketNames.partyViewRequest })
export class PartyViewRequestHandler {
  async handle(spot: Town, message: PartyViewRequest): Promise<void> {
    await spot.showParty(message);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Town,
  actor: () => Player,
  packetName: PacketNames.actionRejected
})
export class TownActionRejectedDelivery {
  async handle(
    _spot: Town,
    actor: Player,
    context: ZLinkMessageContext,
    message: ActionRejected
  ): Promise<void> {
    await actor.context.boundSession.send(restorePush(context.packetName, message)).submit();
  }
}
