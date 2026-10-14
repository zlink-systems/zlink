import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { Game, PacketNames, zoneId } from '../shared/constants';
import {
  EnterWorldRequest,
  PlayerView,
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
    request: EarnRequest
  ): Promise<void> {
    await actor.earn(request.amount);
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
const deliveryPackets: ReadonlyMap<string, new () => PlayerView> = new Map([
  [PacketNames.playerView, PlayerView]
]);
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
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error(`Unregistered player push: ${context.packetName}`);
    // Restore the declared public DTO class before forwarding (#1731).
    await actor.context.boundSession.send(Object.assign(new type(), message)).submit();
  }
}
