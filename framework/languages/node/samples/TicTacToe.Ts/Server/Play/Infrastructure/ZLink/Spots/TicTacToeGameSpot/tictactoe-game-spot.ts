import { Injectable, Scope } from '@nestjs/common';
import { TicTacToeGameTimerHandler } from './Handlers/tictactoe-game-timer-handler';
import { TicTacToeMatch } from '../../../../Domain/TicTacToe/tictactoe-match';
import {
  GameStatus,
  gameStateNotify,
  placeMarkRes,
  playerJoinedNotify,
  playerWinMilestoneEvent
} from '../../../../../../Shared/Contracts/messages';
import { SampleDefaults, SampleNames } from '../../../../../Configuration/sample-settings';
import { DeliverPlayNotification, PlayActor } from '../../Actors/play-actor';
import type {
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotActorJoinResult,
  ZLinkSpotCreateResponse,
  ZLinkSpotContext,
  ZLinkTimer
} from '@zlink-systems/framework';
import type {
  GameState,
  PlaceMarkRes,
  TicTacToeGameJoinReq,
  TicTacToeGameJoinRes
} from '../../../../../../Shared/Contracts/messages';
import type { TicTacToeGameCreateReq } from '../../../../../../Shared/Contracts/messages';
import type { TicTacToeMatch as TicTacToeMatchType } from '../../../../Domain/TicTacToe/tictactoe-match';

interface GameParticipant {
  readonly actorId: string;
  readonly displayName: string;
  readonly level: number;
  readonly wins: number;
}

const GameTickPeriodMs = 1000;
const InitialRoomId = 'tictactoe-room';

@Injectable({ scope: Scope.TRANSIENT })
class TicTacToeGameSpot implements ZLinkSpot<PlayActor> {
  readonly context!: ZLinkSpotContext<PlayActor, TicTacToeGameSpot>;
  private roomId = InitialRoomId;
  private match: TicTacToeMatchType<GameParticipant> = new TicTacToeMatch<GameParticipant>(InitialRoomId);
  private readonly pendingJoins = new Map<string, TicTacToeGameJoinReq>();
  private readonly actors = new Map<string, PlayActor>();
  private gameTick?: ZLinkTimer;
  private requiredLevel: number = SampleDefaults.requiredLevel;

  async onCreate(requestMessage: ZLinkMessage): Promise<ZLinkSpotCreateResponse> {
    const request = requestMessage.decode<TicTacToeGameCreateReq>();
    this.requiredLevel = request.requiredLevel;
    return { accepted: true };
  }

  async configure(): Promise<void> {
    this.gameTick = await this.context.addTimer(
      'game-tick',
      GameTickPeriodMs,
      TicTacToeGameTimerHandler
    );
  }

  async onInitialize(): Promise<void> {
    this.roomId = String(this.context.spotId);
    this.match = new TicTacToeMatch<GameParticipant>(this.roomId);
  }

  async onClosing(): Promise<void> {
    await this.gameTick?.cancel();
    this.gameTick = undefined;
  }

  async onActorJoin(
    actorId: string,
    requestMessage: ZLinkMessage
  ): Promise<ZLinkSpotActorJoinResult> {
    try {
      console.log(`game spot: onActorJoin received. actor=${actorId} roomId=${this.roomId}`);
      const request = requestMessage.decode<TicTacToeGameJoinReq>();
      const response = this.admit(actorId, request);
      console.log(`game spot: onActorJoin completed. actor=${actorId} roomId=${this.roomId}`);
      return { accepted: true, reply: response };
    } catch (error) {
      return {
        accepted: false,
        reply: { error: error instanceof Error ? error.message : String(error) }
      };
    }
  }

  async onJoinedActor(actor: PlayActor): Promise<void> {
    const actorId = actor.actorId;
    this.actors.set(actorId, actor);
    const request = this.pendingJoins.get(actorId);
    if (request !== undefined) {
      this.pendingJoins.delete(actorId);
      const joined = this.requireMatch().players.get(actorId);
      if (joined === undefined) {
        throw new Error(`Accepted TicTacToe actor '${actorId}' has no room membership.`);
      }
      const state = this.requireMatch().snapshot();
      for (const existing of this.actors.values()) {
        if (existing.actorId === actorId) {
          continue;
        }
        await existing.push(playerJoinedNotify(
          this.roomId,
          actorId,
          request.player.displayName,
          request.player.level,
          joined.mark,
          state
        ));
      }
    }
    console.log(`game spot: actor joined. actor=${actorId} roomId=${this.roomId}`);
  }

  async onLeaveActor(actor: PlayActor): Promise<void> {
    this.requireMatch().players.delete(actor.actorId);
    this.actors.delete(actor.actorId);
    if (this.requireMatch().players.size === 0 && isTerminal(this.requireMatch().snapshot().status)) {
      await this.context.close();
    }
  }

  async onDisconnectActor(_actor: PlayActor): Promise<void> {}

  async placeMark(actorId: string, cell: number): Promise<PlaceMarkRes> {
    const match = this.requireMatch();
    const before = match.snapshot();
    const change = match.placeMark(actorId, cell);
    const state = change.state;
    for (const joined of match.players.values()) {
      if (joined.actorId !== actorId) {
        await this.notifyActor(this.requireActorId(joined.actorId), gameStateNotify(state));
      }
    }
    await this.publishWinMilestone(actorId, before, state);
    return placeMarkRes(state);
  }

  async tick(): Promise<void> {
    const match = this.requireMatch();
    const change = match.tick();
    if (change.changed) {
      for (const player of match.players.values()) {
        await this.notifyActor(this.requireActorId(player.actorId), gameStateNotify(change.state));
      }
    }
  }

  verifyLeave(actorId: string, roomId: string): void {
    if (roomId !== this.requireRoomId()) {
      throw new Error(`Actor requested leave for a different room. roomId=${roomId}`);
    }
    if (!this.requireMatch().players.has(actorId)) {
      throw new Error(`Actor '${actorId}' has not joined room '${roomId}'.`);
    }
    if (!isTerminal(this.requireMatch().snapshot().status)) {
      throw new Error('Game is not finished.');
    }
  }

  private async publishWinMilestone(actorId: string, before: GameState, after: GameState): Promise<void> {
    if (before.status === GameStatus.Won || after.status !== GameStatus.Won || after.winner !== actorId) {
      return;
    }
    const participant = this.requireMatch().players.get(actorId)?.player;
    if (participant === undefined) return;
    const wins = participant.wins + 1;
    if (wins !== 100) return;
    await this.context.outbound
      .publish(
        SampleNames.playerMilestoneChannel,
        SampleNames.playerMilestoneTopic,
        playerWinMilestoneEvent(after.roomId, actorId, participant.displayName, wins)
      )
      .submit();
  }

  private admit(actorId: string, request: TicTacToeGameJoinReq): TicTacToeGameJoinRes {
    const roomId = this.requireRoomId();
    if (request.roomId !== roomId) {
      throw new Error(`Actor requested join for a different room. roomId=${request.roomId}`);
    }
    if (request.player.actorId !== actorId) {
      throw new Error(`Join player '${request.player.actorId}' does not match actor '${actorId}'.`);
    }
    if (request.player.level < this.requiredLevel) {
      throw new Error(`Player level ${request.player.level} is below required level ${this.requiredLevel}.`);
    }
    const result = this.requireMatch().joinPlayer({
      actorId,
      displayName: request.player.displayName,
      level: request.player.level,
      wins: request.player.wins
    });
    this.pendingJoins.set(actorId, request);
    return { state: result.state };
  }

  private requireActorId(actorId: string): string {
    if (!this.actors.has(actorId)) {
      throw new Error(`TicTacToe actor '${actorId}' has no room membership reference.`);
    }
    return actorId;
  }

  private async notifyActor(actorId: string, payload: ConstructorParameters<typeof DeliverPlayNotification>[0]): Promise<void> {
    const actor = this.actors.get(actorId);
    if (actor === undefined) {
      throw new Error(`TicTacToe actor '${actorId}' has no room membership reference.`);
    }
    await actor.push(payload);
  }

  private requireRoomId(): string {
    return this.roomId;
  }

  private requireMatch(): TicTacToeMatchType<GameParticipant> {
    return this.match;
  }
}

function isTerminal(status: GameStatus): boolean {
  return status === GameStatus.Won
    || status === GameStatus.Draw
    || status === GameStatus.TurnTimedOut;
}

export { TicTacToeGameSpot };
