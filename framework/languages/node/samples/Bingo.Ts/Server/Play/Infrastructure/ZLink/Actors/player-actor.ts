import { zlinkEntrySpotActorSendHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkActor, ZLinkActorContext, ZLinkMessageContext } from '@zlink-systems/framework';
import { BingoEntrySpot } from '../Spots/EntrySpot/bingo-entry-spot';
import { BingoRoomSpot } from '../Spots/BingoRoomSpot/bingo-room-spot';
import {
  BingoGameEndedNotify,
  BingoGameStartedNotify,
  BingoNumberDrawnNotify,
  BingoRewardAnnouncedNotify,
  EnsurePlayerActorReq,
  PlayerJoinedNotify
} from '../../../../../Shared/Contracts/bingo-messages.generated';

class PlayerActor implements ZLinkActor {
  readonly context!: ZLinkActorContext;
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
}

@zlinkEntrySpotActorSendHandler({ entrySpot: () => BingoEntrySpot, actor: () => PlayerActor, packetName: 'EnsurePlayerActorReq' })
class InitializePlayerActorHandler {
  async handle(_spot: BingoEntrySpot, actor: PlayerActor, _context: ZLinkMessageContext, message: EnsurePlayerActorReq): Promise<void> {
    actor.displayName = message.displayName;
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'PlayerJoinedNotify' })
class PlayerJoinedNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: PlayerJoinedNotify): Promise<void> {
    await actor.push(message);
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'BingoGameStartedNotify' })
class BingoGameStartedNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: BingoGameStartedNotify): Promise<void> {
    await actor.push(message);
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'BingoNumberDrawnNotify' })
class BingoNumberDrawnNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: BingoNumberDrawnNotify): Promise<void> {
    await actor.push(message);
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'BingoGameEndedNotify' })
class BingoGameEndedNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: BingoGameEndedNotify): Promise<void> {
    await actor.push(message);
  }
}

@zlinkSpotActorSendHandler({ spot: () => BingoRoomSpot, actor: () => PlayerActor, packetName: 'BingoRewardAnnouncedNotify' })
class BingoRewardAnnouncedNotificationHandler {
  async handle(_spot: BingoRoomSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: BingoRewardAnnouncedNotify): Promise<void> {
    await actor.push(message);
  }
}

export {
  BingoGameEndedNotificationHandler,
  BingoGameStartedNotificationHandler,
  BingoNumberDrawnNotificationHandler,
  BingoRewardAnnouncedNotificationHandler,
  InitializePlayerActorHandler,
  PlayerJoinedNotificationHandler,
  PlayerActor
};
