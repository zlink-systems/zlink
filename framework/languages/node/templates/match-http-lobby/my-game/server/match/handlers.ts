import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames } from '../shared/constants';
import {
  MatchEnded,
  RoomView,
  JoinRoomRequest,
  type RoomViewRequest,
  type MoveRequest,
  type ShootRequest
} from '../shared/messages';
import { Entry, Room } from './spots';
import { Player } from './player';
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.joinRoomRequest
})
export class JoinRoom {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinRoomRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId || !request.roomId.trim())
      throw new Error('Join identity mismatch');
    actor.context
      .joinSpot(request.roomId, new JoinRoomRequest(request.playerId, request.roomId))
      .defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Room,
  actor: () => Player,
  packetName: PacketNames.joinRoomRequest
})
export class Reconnect {
  async handle(
    spot: Room,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinRoomRequest
  ): Promise<void> {
    if (
      request.playerId !== actor.context.actorId ||
      request.roomId !== String(spot.context.spotId)
    )
      throw new Error('Reconnect identity mismatch');
    await spot.sendView(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Room,
  actor: () => Player,
  packetName: PacketNames.roomViewRequest
})
export class View {
  async handle(
    spot: Room,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: RoomViewRequest
  ): Promise<void> {
    await spot.sendView(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Room,
  actor: () => Player,
  packetName: PacketNames.moveRequest
})
export class Move {
  async handle(
    spot: Room,
    actor: Player,
    _context: ZLinkMessageContext,
    request: MoveRequest
  ): Promise<void> {
    await spot.move(actor, request);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Room,
  actor: () => Player,
  packetName: PacketNames.shootRequest
})
export class Shoot {
  async handle(
    spot: Room,
    actor: Player,
    _context: ZLinkMessageContext,
    request: ShootRequest
  ): Promise<void> {
    await spot.shoot(actor, request);
  }
}
const deliveryPackets: ReadonlyMap<string, new () => object> = new Map<string, new () => object>([
  [PacketNames.roomView, RoomView],
  [PacketNames.matchEnded, MatchEnded]
]);
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Room,
  actor: () => Player,
  packetName: PacketNames.roomView
})
@zlinkSpotActorSendHandler({
  spot: () => Room,
  actor: () => Player,
  packetName: PacketNames.matchEnded
})
export class Delivery {
  async handle(
    _spot: Room,
    actor: Player,
    context: ZLinkMessageContext,
    message: RoomView | MatchEnded
  ): Promise<void> {
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error(`Unregistered room push: ${context.packetName}`);
    // Restore the declared public DTO class before forwarding (#1731).
    await actor.context.boundSession.send(Object.assign(new type(), message)).submit();
  }
}
