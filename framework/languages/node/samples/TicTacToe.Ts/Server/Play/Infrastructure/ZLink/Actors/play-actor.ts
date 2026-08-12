import type {
  ZLinkActor,
  ZLinkActorJoinCompletion,
  ZLinkActorContext,
  ZLinkEntrySpotActorSendHandler,
  ZLinkMessage,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { ZLinkSpotActorSend } from '@zlink-systems/framework';
import { Injectable } from '@nestjs/common';
import { PlayEntrySpot } from '../Spots/EntrySpot/play-entry-spot';
import { TicTacToeGameSpot } from '../Spots/TicTacToeGameSpot/tictactoe-game-spot';
import type { TicTacToeActor } from '../../../../../Shared/Contracts/messages';
import {
  GameStateNotify,
  JoinGameFailedNotify,
  JoinGameNotify,
  PacketNames,
  PlayerJoinedNotify,
  WinMilestoneNotify,
  joinGameNotify
} from '../../../../../Shared/Contracts/messages';
import type { TicTacToeGameJoinRes } from '../../../../../Shared/Contracts/messages';

type PlayNotification =
  | JoinGameNotify
  | JoinGameFailedNotify
  | PlayerJoinedNotify
  | GameStateNotify
  | WinMilestoneNotify;

type RelayedPlayNotification = PlayerJoinedNotify | GameStateNotify | WinMilestoneNotify;

class DeliverPlayNotificationMsg {
  readonly kind: 'gameState' | 'playerJoined' | 'winMilestone';
  readonly gameState?: GameStateNotify;
  readonly playerJoined?: PlayerJoinedNotify;
  readonly winMilestone?: WinMilestoneNotify;

  constructor(payload: RelayedPlayNotification) {
    if (payload instanceof PlayerJoinedNotify) {
      this.kind = 'playerJoined';
      this.playerJoined = payload;
      return;
    }
    if (payload instanceof WinMilestoneNotify) {
      this.kind = 'winMilestone';
      this.winMilestone = payload;
      return;
    }
    this.kind = 'gameState';
    this.gameState = payload;
  }
}

class PlayActor implements ZLinkActor, TicTacToeActor {
  readonly actorId: string;
  readonly context!: ZLinkActorContext;
  displayName: string;
  level: number;
  wins: number;
  roomId?: string;
  pendingJoinRoomId?: string;
  destroyAfterEntrySpotJoin = false;
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
    const roomId = this.pendingJoinRoomId ?? this.roomId ?? '';
    this.pendingJoinRoomId = undefined;
    if (completion.status === 'rejected') {
      await this.push(new JoinGameFailedNotify(roomId, joinRejectionError(completion.reply)));
      return;
    }
    if (completion.status === 'failed') {
      await this.push(new JoinGameFailedNotify(roomId, `Room join failed: ${completion.kind}`));
      return;
    }
    if (completion.reply === undefined) {
      await this.push(new JoinGameFailedNotify(roomId, 'Room join completed without state.'));
      return;
    }
    // The deferred Join reply is delivered only after the target membership is
    // committed, so the client receives the authoritative Spot state.
    let joined: TicTacToeGameJoinRes;
    try {
      joined = completion.reply.decode<TicTacToeGameJoinRes>();
    } catch (error) {
      await this.push(new JoinGameFailedNotify(roomId, errorText(error)));
      return;
    }
    this.roomId = joined.state.roomId;
    await this.push(joinGameNotify(joined.state));
  }

  markForDestroyAfterRoomLeave(): void {
    this.destroyAfterEntrySpotJoin = true;
  }
}

@Injectable()
class DeliverPlayNotificationHandler {
  @ZLinkSpotActorSend(PacketNames.deliverPlayNotificationMsg)
  async handle(_spot: TicTacToeGameSpot, actor: PlayActor, _context: ZLinkMessageContext, message: DeliverPlayNotificationMsg): Promise<void> {
    await deliverPlayNotification(actor, message);
  }
}

@Injectable()
class DeliverPlayNotificationEntryHandler
  implements ZLinkEntrySpotActorSendHandler<PlayEntrySpot, PlayActor, DeliverPlayNotificationMsg> {
  @ZLinkSpotActorSend(PacketNames.deliverPlayNotificationMsg)
  async handle(
    _spot: PlayEntrySpot,
    actor: PlayActor,
    _context: ZLinkMessageContext,
    message: DeliverPlayNotificationMsg
  ): Promise<void> {
    await deliverPlayNotification(actor, message);
  }
}

async function deliverPlayNotification(
  actor: PlayActor,
  message: DeliverPlayNotificationMsg
): Promise<void> {
  if (message.kind === 'playerJoined') {
    const value = message.playerJoined;
    if (value === undefined) {
      throw new Error('PlayerJoined notification payload is missing.');
    }
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
    const value = message.winMilestone;
    if (value === undefined) {
      throw new Error('WinMilestone notification payload is missing.');
    }
    await actor.push(new WinMilestoneNotify(
      value.roomId,
      value.actorId,
      value.displayName,
      value.wins
    ));
    return;
  }
  const value = message.gameState;
  if (value === undefined) {
    throw new Error('GameState notification payload is missing.');
  }
  await actor.push(new GameStateNotify(value.state));
}

function joinRejectionError(reply: ZLinkMessage | undefined): string {
  if (reply !== undefined) {
    try {
      const rejection = reply.decode<{ readonly error?: unknown }>();
      if (typeof rejection.error === 'string' && rejection.error.length > 0) {
        return rejection.error;
      }
    } catch {
      // A malformed rejection reply still terminates as a typed join failure.
    }
  }
  return 'Room join was rejected.';
}

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export {
  DeliverPlayNotificationMsg,
  DeliverPlayNotificationEntryHandler,
  DeliverPlayNotificationHandler,
  PlayActor
};
