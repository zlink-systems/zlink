import { Injectable } from '@nestjs/common';
import {
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorSendHandler,
  zlinkSpotPacketHandler
} from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames, Reasons } from '../../shared/constants';
import {
  RoomPrepared,
  type CancelMatch,
  type CommitRoom,
  type FindMatch,
  type MatchAssigned,
  type MatchCommitted,
  type MatchCancelled,
  type MatchReleased,
  type PrepareRoom,
  type WithdrawRoom
} from '../../shared/messages';
import { Player } from './player';
import { Entry, Table } from './table';

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.findMatch
})
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.findMatch
})
export class FindMatchHandler {
  async handle(
    spot: Entry | Table,
    actor: Player,
    _context: ZLinkMessageContext,
    request: FindMatch
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    if (spot instanceof Table && !actor.match?.ticketId) {
      await actor.reject(Reasons.matchRoomActive);
      return;
    }
    await actor.findMatch(spot.context.outbound);
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.cancelMatch
})
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.cancelMatch
})
export class CancelMatchHandler {
  async handle(
    spot: Entry | Table,
    actor: Player,
    _context: ZLinkMessageContext,
    request: CancelMatch
  ): Promise<void> {
    await actor.cancelMatch(spot.context.outbound, request.ticketId);
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.matchAssigned
})
export class AssignMatchHandler {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    message: MatchAssigned
  ): Promise<void> {
    actor.assign(message);
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.matchCancelled
})
export class MatchCancelledHandler {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    message: MatchCancelled
  ): Promise<void> {
    await actor.ended(message);
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.matchReleased
})
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.matchReleased
})
export class ReleaseMatchHandler {
  async handle(
    _spot: Entry | Table,
    actor: Player,
    _context: ZLinkMessageContext,
    message: MatchReleased
  ): Promise<void> {
    await actor.release(message);
  }
}

@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.matchCommitted
})
export class MatchCommittedHandler {
  async handle(
    _spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    message: MatchCommitted
  ): Promise<void> {
    await actor.committed(message);
  }
}

@zlinkSpotPacketHandler({ spot: () => Table, packetName: PacketNames.prepareRoom })
export class PrepareRoomHandler {
  async handle(spot: Table, request: PrepareRoom): Promise<RoomPrepared> {
    return new RoomPrepared(request.assignmentId, spot.prepare(request));
  }
}

@zlinkSpotPacketHandler({ spot: () => Table, packetName: PacketNames.commitRoom })
export class CommitRoomHandler {
  async handle(spot: Table, request: CommitRoom): Promise<void> {
    await spot.commit(request.assignmentId);
  }
}

@zlinkSpotPacketHandler({ spot: () => Table, packetName: PacketNames.withdrawRoom })
export class WithdrawRoomHandler {
  async handle(spot: Table, request: WithdrawRoom): Promise<void> {
    await spot.withdraw(request.assignmentId);
  }
}
