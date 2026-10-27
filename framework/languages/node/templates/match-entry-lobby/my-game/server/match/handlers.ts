import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { Names, PacketNames } from '../shared/constants';
import {
  MatchEnded,
  MatchFound,
  RoomView,
  RoomAdmission,
  type RoomAssignment,
  QueueRequest,
  type RoomViewRequest,
  type MoveRequest,
  type ShootRequest
} from '../shared/messages';
import { Entry, Matchmaking, Room } from './spots';
import { Player } from './player';
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.queueRequest
})
export class EnterQueue {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: QueueRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Queue identity mismatch');
    actor.context
      .joinSpot(Names.matchmakingId, new QueueRequest(request.playerId, request.mode))
      .defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Matchmaking,
  actor: () => Player,
  packetName: PacketNames.queueRequest
})
export class QueueAgain {
  async handle(
    spot: Matchmaking,
    actor: Player,
    _context: ZLinkMessageContext,
    request: QueueRequest
  ): Promise<void> {
    await spot.queue(actor, request);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Matchmaking,
  actor: () => Player,
  packetName: PacketNames.roomAssignment
})
export class Assignment {
  async handle(
    _spot: Matchmaking,
    actor: Player,
    _context: ZLinkMessageContext,
    message: RoomAssignment
  ): Promise<void> {
    actor.context
      .joinSpot(message.roomId, new RoomAdmission(actor.context.actorId, message.roomId))
      .defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Room,
  actor: () => Player,
  packetName: PacketNames.queueRequest
})
export class Reconnect {
  async handle(
    spot: Room,
    actor: Player,
    _context: ZLinkMessageContext,
    request: QueueRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Reconnect identity mismatch');
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
  [PacketNames.matchFound, MatchFound],
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
  packetName: PacketNames.matchFound
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
    message: RoomView | MatchFound | MatchEnded
  ): Promise<void> {
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error(`Unregistered room push: ${context.packetName}`);
    // Restore the declared public DTO class before forwarding (#1731).
    await actor.context.boundSession.send(Object.assign(new type(), message)).submit();
  }
}
