import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PlayerActor } from './player-actor';
import { BingoRoomSpot } from '../Spots/BingoRoomSpot/bingo-room-spot';
import { LeaveFinishedBingoRoom } from '../../../../../Shared/Contracts/bingo-messages.generated';

@Injectable()
class PendingBingoActorDestroyRegistry {
  private readonly actorIds = new Set<string>();

  mark(actorId: string): void {
    this.actorIds.add(actorId);
  }

  consume(actorId: string): boolean {
    return this.actorIds.delete(actorId);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => BingoRoomSpot,
  actor: () => PlayerActor,
  packetName: 'LeaveFinishedBingoRoom'
})
class LeaveFinishedBingoRoomHandler {
  constructor(private readonly pendingDestroys: PendingBingoActorDestroyRegistry) {}

  async handle(
    spot: BingoRoomSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    _message: LeaveFinishedBingoRoom
  ): Promise<void> {
    this.pendingDestroys.mark(actor.actorId);
    await spot.context.leaveActor(actor);
  }
}

export {
  LeaveFinishedBingoRoom,
  LeaveFinishedBingoRoomHandler,
  PendingBingoActorDestroyRegistry
};
