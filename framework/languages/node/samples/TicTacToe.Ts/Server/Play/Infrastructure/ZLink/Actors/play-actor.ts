import type {
  ZLinkActor,
  ZLinkActorJoinCompletion,
  ZLinkActorContext,
  ZLinkEntrySpotActorSendHandler,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { zlinkEntrySpotActorSendHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import { PlayEntrySpot } from '../Spots/EntrySpot/play-entry-spot';
import { TicTacToeGameSpot } from '../Spots/TicTacToeGameSpot/tictactoe-game-spot';
import type { TicTacToeActor } from '../../../../../Shared/Contracts/messages';
import {
  GameStateNotify,
  PlayerJoinedNotify,
  WinMilestoneNotify,
  gameStateNotify
} from '../../../../../Shared/Contracts/messages';
import type { TicTacToeGameJoinRes } from '../../../../../Shared/Contracts/messages';

type PlayNotification = PlayerJoinedNotify | GameStateNotify | WinMilestoneNotify;

class DeliverPlayNotification {
  readonly kind: 'gameState' | 'playerJoined' | 'winMilestone';

  constructor(readonly payload: PlayNotification) {
    this.kind = payload instanceof PlayerJoinedNotify
      ? 'playerJoined'
      : payload instanceof WinMilestoneNotify
        ? 'winMilestone'
        : 'gameState';
  }
}

class PlayActor implements ZLinkActor, TicTacToeActor {
  readonly actorId: string;
  readonly context!: ZLinkActorContext;
  displayName: string;
  level: number;
  wins: number;
  roomId?: string;
  private nextSeq: number;

  constructor(actorId: string, displayName: string, context?: ZLinkActorContext, level = 0, wins = 0) {
    this.actorId = actorId;
    if (context !== undefined) {
      Object.defineProperty(this, 'context', {
        configurable: true,
        enumerable: true,
        value: context
      });
    }
    this.displayName = displayName;
    this.level = level;
    this.wins = wins;
    this.nextSeq = 0;
  }

  async push(payload: PlayNotification): Promise<void> {
    this.nextSeq += 1;
    await this.context.boundSession
      .send(payload)
      .metadata('seq', String(this.nextSeq))
      .submit();
  }

  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status !== 'accepted' || completion.reply === undefined) {
      return;
    }
    // The deferred Join reply is delivered only after the target membership is
    // committed, so the client receives the authoritative Spot state.
    const joined = completion.reply.decode<TicTacToeGameJoinRes>();
    this.roomId = joined.state.roomId;
    await this.push(gameStateNotify(joined.state));
  }
}

@zlinkSpotActorSendHandler({
  spot: () => TicTacToeGameSpot,
  actor: () => PlayActor,
  packetName: 'DeliverPlayNotification'
})
class DeliverPlayNotificationHandler {
  async handle(_spot: TicTacToeGameSpot, actor: PlayActor, _context: ZLinkMessageContext, message: DeliverPlayNotification): Promise<void> {
    await deliverPlayNotification(actor, message);
  }
}

@zlinkEntrySpotActorSendHandler({
  actor: () => PlayActor,
  entrySpot: () => PlayEntrySpot,
  packetName: 'DeliverPlayNotification'
})
class DeliverPlayNotificationEntryHandler
  implements ZLinkEntrySpotActorSendHandler<PlayEntrySpot, PlayActor, DeliverPlayNotification> {
  async handle(
    _spot: PlayEntrySpot,
    actor: PlayActor,
    _context: ZLinkMessageContext,
    message: DeliverPlayNotification
  ): Promise<void> {
    await deliverPlayNotification(actor, message);
  }
}

async function deliverPlayNotification(
  actor: PlayActor,
  message: DeliverPlayNotification
): Promise<void> {
  const payload = message.payload;
  if (message.kind === 'playerJoined') {
    const value = payload as PlayerJoinedNotify;
    await actor.push(new PlayerJoinedNotify(
      value.roomId,
      value.actorId,
      value.displayName,
      value.level,
      value.mark,
      value.state
    ));
    return;
  }
  if (message.kind === 'winMilestone') {
    const value = payload as WinMilestoneNotify;
    await actor.push(new WinMilestoneNotify(
      value.roomId,
      value.actorId,
      value.displayName,
      value.wins
    ));
    return;
  }
  const value = payload as GameStateNotify;
  await actor.push(new GameStateNotify(value.state));
}

export {
  DeliverPlayNotification,
  DeliverPlayNotificationEntryHandler,
  DeliverPlayNotificationHandler,
  PlayActor
};
