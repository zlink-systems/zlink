import { TicTacToeBoard } from './tictactoe-board';
import type { TicTacToeBoard as TicTacToeBoardType } from './tictactoe-board';
import { GameMarks, GameStatus } from '../../../../Shared/Contracts/messages';
import type { GameState } from '../../../../Shared/Contracts/messages';

type TicTacToePlayer = {
  actorId: string;
};

type JoinedPlayer<TPlayer extends TicTacToePlayer = TicTacToePlayer> = {
  actorId: string;
  mark: string;
  player: TPlayer;
};

type TicTacToeJoinChange<TPlayer extends TicTacToePlayer = TicTacToePlayer> = {
  state: GameState;
  joined: JoinedPlayer<TPlayer>;
  newlyJoined: boolean;
};

type TicTacToeMoveChange = {
  state: GameState;
};

type TicTacToeTickChange = {
  state: GameState;
  changed: boolean;
};

const defaultTurnTimeoutMs = 15_000;

class TicTacToeMatch<TPlayer extends TicTacToePlayer = TicTacToePlayer> {
  readonly roomId: string;
  readonly players: Map<string, JoinedPlayer<TPlayer>>;
  private readonly board: TicTacToeBoardType;
  private readonly turnTimeoutMs: number;
  private status: GameStatus;
  private winner: string | null;
  private nextTurn: string;
  private lastMoveActorId: string | null;
  private lastMoveCell: number | null;
  private turnDeadline: number | null;

  constructor(roomId: string, turnTimeoutMs: number = defaultTurnTimeoutMs) {
    this.roomId = roomId;
    this.turnTimeoutMs = turnTimeoutMs;
    this.players = new Map();
    this.board = new TicTacToeBoard();
    this.status = GameStatus.WaitingForPlayers;
    this.winner = null;
    this.nextTurn = '';
    this.lastMoveActorId = null;
    this.lastMoveCell = null;
    this.turnDeadline = null;
  }

  joinPlayer(player: TPlayer): TicTacToeJoinChange<TPlayer> {
    const existing = this.players.get(player.actorId);
    if (existing !== undefined) {
      return { state: this.snapshot(), joined: existing, newlyJoined: false };
    }
    if (this.players.size >= 2) {
      throw new Error(`Room '${this.roomId}' is full.`);
    }
    const mark = this.players.size === 0 ? GameMarks.x : GameMarks.o;
    const joined = { actorId: player.actorId, mark, player };
    this.players.set(player.actorId, joined);
    if (this.players.size === 2) {
      this.status = GameStatus.InProgress;
      this.nextTurn = GameMarks.x;
      this.resetTurnDeadline();
    }
    return { state: this.snapshot(), joined, newlyJoined: true };
  }

  placeMark(actorId: string, cell: number): TicTacToeMoveChange {
    const player = this.players.get(actorId);
    if (player === undefined) {
      throw new Error(`Actor '${actorId}' has not joined room '${this.roomId}'.`);
    }
    if (this.status !== GameStatus.InProgress) {
      throw new Error(`Room '${this.roomId}' is not in progress.`);
    }
    if (this.nextTurn !== player.mark) {
      throw new Error(`Actor '${actorId}' cannot move out of turn.`);
    }
    this.board.place(player.mark, cell);
    this.lastMoveActorId = actorId;
    this.lastMoveCell = cell;
    this.advanceAfterMove(actorId, player.mark);
    return { state: this.snapshot() };
  }

  tick(now: number = Date.now()): TicTacToeTickChange {
    if (this.status !== GameStatus.InProgress || this.turnDeadline === null || now < this.turnDeadline) {
      return { state: this.snapshot(), changed: false };
    }
    this.status = GameStatus.TurnTimedOut;
    this.winner = null;
    this.nextTurn = '';
    this.turnDeadline = null;
    return { state: this.snapshot(), changed: true };
  }

  snapshot(): GameState {
    return {
      roomId: this.roomId,
      board: this.board.snapshot(),
      status: this.status,
      winner: this.winner,
      nextTurn: this.nextTurn,
      xActorId: this.xActorId(),
      oActorId: this.oActorId(),
      lastMoveActorId: this.lastMoveActorId,
      lastMoveCell: this.lastMoveCell
    };
  }

  xActorId(): string | null {
    return [...this.players.values()].find((player) => player.mark === GameMarks.x)?.actorId ?? null;
  }

  oActorId(): string | null {
    return [...this.players.values()].find((player) => player.mark === GameMarks.o)?.actorId ?? null;
  }

  advanceAfterMove(actorId: string, mark: string): void {
    if (this.board.hasWon(mark)) {
      this.status = GameStatus.Won;
      this.winner = actorId;
      this.nextTurn = '';
      this.turnDeadline = null;
      return;
    }
    if (this.board.isFull()) {
      this.status = GameStatus.Draw;
      this.winner = null;
      this.nextTurn = '';
      this.turnDeadline = null;
      return;
    }
    const nextPlayer = [...this.players.values()].find((player) => player.actorId !== actorId);
    if (nextPlayer === undefined) {
      throw new Error(`Room '${this.roomId}' cannot advance without another player.`);
    }
    this.nextTurn = nextPlayer.mark;
    this.resetTurnDeadline();
  }

  resetTurnDeadline(): void {
    this.turnDeadline = Date.now() + this.turnTimeoutMs;
  }
}

export { TicTacToeMatch };
export type { JoinedPlayer, TicTacToeJoinChange, TicTacToeMoveChange, TicTacToePlayer, TicTacToeTickChange };
