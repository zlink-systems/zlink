import { Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler, ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames, Rejection } from '../shared/constants';
import {
  ActionRejected,
  PrepareDungeon,
  DungeonPrepared,
  DungeonView,
  JoinDungeon,
  ChangeChannelRequest,
  TownView,
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
    await spot.showWithParty(actor);
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
    await spot.showWithParty(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.advanceRequest
})
export class Advance {
  async handle(spot: Dungeon, actor: Player): Promise<void> {
    await spot.advance(actor);
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

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.actionRejected
})
export class DungeonActionRejectedDelivery {
  async handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    message: ActionRejected
  ): Promise<void> {
    await actor.context.boundSession.send(restorePush(context.packetName, message)).submit();
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.townView
})
export class TownDelivery {
  async handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    view: TownView
  ): Promise<void> {
    await actor.context.boundSession.send(restorePush(context.packetName, view)).submit();
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.enterDungeonRequest
})
export class DungeonEnter {
  async handle(spot: Dungeon, actor: Player): Promise<void> {
    await spot.show(actor);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.joinDungeon
})
export class DungeonTravelRepeat {
  async handle(
    _spot: Dungeon,
    _actor: Player,
    _context: ZLinkMessageContext,
    _request: JoinDungeon
  ): Promise<void> {}
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.changeChannelRequest
})
export class DungeonChangeChannel {
  async handle(
    _spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: ChangeChannelRequest
  ): Promise<void> {
    await actor.context.boundSession.send(new ActionRejected(Rejection.runInProgress)).submit();
  }
}
