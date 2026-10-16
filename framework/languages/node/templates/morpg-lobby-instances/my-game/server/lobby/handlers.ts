import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames } from '../shared/constants';
import {
  PartyView,
  JoinDungeon,
  type JoinLobbyRequest,
  type ReadyRequest,
  type EnterDungeonRequest,
  type ViewRequest
} from '../shared/messages';
import { Player } from '../shared/player';
import { Party } from './spots';
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Party,
  actor: () => Player,
  packetName: PacketNames.joinLobbyRequest
})
export class Reconnect {
  async handle(
    spot: Party,
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
  spot: () => Party,
  actor: () => Player,
  packetName: PacketNames.viewRequest
})
export class View {
  async handle(
    spot: Party,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: ViewRequest
  ): Promise<void> {
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Party,
  actor: () => Player,
  packetName: PacketNames.readyRequest
})
export class Ready {
  async handle(
    spot: Party,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: ReadyRequest
  ): Promise<void> {
    await spot.ready(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Party,
  actor: () => Player,
  packetName: PacketNames.enterDungeonRequest
})
export class EnterDungeon {
  async handle(
    spot: Party,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: EnterDungeonRequest
  ): Promise<void> {
    await spot.enter(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Party,
  actor: () => Player,
  packetName: PacketNames.joinDungeon
})
export class Travel {
  async handle(
    _spot: Party,
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
  spot: () => Party,
  actor: () => Player,
  packetName: PacketNames.partyView
})
export class Delivery {
  async handle(
    _spot: Party,
    actor: Player,
    context: ZLinkMessageContext,
    request: PartyView
  ): Promise<void> {
    // Restore the declared DTO class before forwarding (#1731).
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error('Unregistered party push');
    await actor.context.boundSession.send(Object.assign(new type(), request)).submit();
  }
}
const deliveryPackets: ReadonlyMap<string, new () => object> = new Map([
  [PacketNames.partyView, PartyView]
]);
