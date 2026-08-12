import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkActor, ZLinkActorContext, ZLinkMessageContext } from '@zlink-systems/framework';
import { BingoRoomSpot } from '../Spots/BingoRoomSpot/bingo-room-spot';
import {
  DeliverBingoGameEndedMsg,
  DeliverBingoGameStartedMsg,
  DeliverBingoNumberDrawnMsg,
  DeliverBingoRewardAnnouncedMsg,
  DeliverPlayerJoinedMsg
} from '../../../../../Shared/Contracts/bingo-messages.generated';

class PlayerActor implements ZLinkActor {
  readonly context!: ZLinkActorContext;
  destroyAfterEntrySpotJoin = false;
  private nextSeq = 0;

  constructor(
    readonly actorId: string,
    public displayName: string
  ) {}

  async push(payload: unknown): Promise<void> {
    this.nextSeq += 1;
    await this.context.boundSession
      .send(payload)
      .metadata('seq', String(this.nextSeq))
      .submit();
  }

  markForDestroyAfterRoomLeave(): void {
    this.destroyAfterEntrySpotJoin = true;
  }

  consumeDestroyAfterEntrySpotJoin(): boolean {
    const destroy = this.destroyAfterEntrySpotJoin;
    this.destroyAfterEntrySpotJoin = false;
    return destroy;
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'DeliverPlayerJoinedMsg' })
class PlayerJoinedNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: DeliverPlayerJoinedMsg): Promise<void> {
    await actor.push(message.notification);
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'DeliverBingoGameStartedMsg' })
class BingoGameStartedNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: DeliverBingoGameStartedMsg): Promise<void> {
    await actor.push(message.notification);
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'DeliverBingoNumberDrawnMsg' })
class BingoNumberDrawnNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: DeliverBingoNumberDrawnMsg): Promise<void> {
    await actor.push(message.notification);
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'DeliverBingoGameEndedMsg' })
class BingoGameEndedNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: DeliverBingoGameEndedMsg): Promise<void> {
    await actor.push(message.notification);
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'DeliverBingoRewardAnnouncedMsg' })
class BingoRewardAnnouncedNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: DeliverBingoRewardAnnouncedMsg): Promise<void> {
    await actor.push(message.notification);
  }
}

export {
  BingoGameEndedNotificationHandler,
  BingoGameStartedNotificationHandler,
  BingoNumberDrawnNotificationHandler,
  BingoRewardAnnouncedNotificationHandler,
  PlayerJoinedNotificationHandler,
  PlayerActor
};
