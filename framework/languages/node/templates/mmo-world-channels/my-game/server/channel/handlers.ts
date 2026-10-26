import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { Game, PacketNames, zoneId } from '../shared/constants';
import {
  ActionRejected,
  ChannelView,
  ChannelViewRequest,
  EnterWorldRequest,
  GameRules,
  PlayerView,
  Rejection,
  type ChangeChannelRequest,
  type EarnRequest,
  type ViewRequest
} from '../shared/messages';
import { Entry, Zone } from './spots';
import { Player } from './player';
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.enterWorldRequest
})
export class EnterPlayer {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: EnterWorldRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    actor.context
      .joinSpot(zoneId(Game.firstChannel), new EnterWorldRequest(request.playerId))
      .defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.enterWorldRequest
})
export class EnterZone {
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    request: EnterWorldRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.changeChannelRequest
})
export class ChangeChannel {
  async handle(
    _spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    request: ChangeChannelRequest
  ): Promise<void> {
    if (request.channel > GameRules.maxInteger) {
      await actor.context.boundSession
        .send(new ActionRejected(Rejection.integerOutOfRange, 0))
        .submit();
      return;
    }
    actor.context
      .joinSpot(zoneId(request.channel), new EnterWorldRequest(actor.context.actorId))
      .defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.earnRequest
})
export class EarnGold {
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: EarnRequest
  ): Promise<void> {
    const rejection = await actor.earn();
    if (rejection) {
      await actor.context.boundSession.send(rejection).submit();
      return;
    }
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.viewRequest
})
export class ViewPlayer {
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: ViewRequest
  ): Promise<void> {
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.channelViewRequest
})
export class ViewChannel {
  async handle(spot: Zone, actor: Player, _context: ZLinkMessageContext, _request: ChannelViewRequest) {
    await spot.showChannelView(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.playerView
})
export class DeliverView {
  async handle(
    _spot: Zone,
    actor: Player,
    context: ZLinkMessageContext,
    message: PlayerView
  ): Promise<void> {
    await actor.deliver(context.packetName, message);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.channelView
})
export class DeliverChannelView {
  async handle(
    _spot: Zone,
    actor: Player,
    context: ZLinkMessageContext,
    message: ChannelView
  ): Promise<void> {
    await actor.deliver(context.packetName, message);
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.playerView
})
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.channelView
})
export class EntryNotifications {
  async handle(
    _spot: Entry,
    actor: Player,
    context: ZLinkMessageContext,
    message: PlayerView | ChannelView
  ): Promise<void> {
    await actor.deliver(context.packetName, message);
  }
}
