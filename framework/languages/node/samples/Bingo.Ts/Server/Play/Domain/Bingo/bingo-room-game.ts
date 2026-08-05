import { BingoGame } from './bingo-game';
import type { BingoGame as BingoGameType } from './bingo-game';
import type { BingoCard } from './bingo-card';

enum BingoRoomStatus {
  WaitingForPlayers = 'WaitingForPlayers',
  Running = 'Running',
  Finished = 'Finished'
}

type BingoActor = {
  actorId: string;
  displayName: string;
};

type BingoRoomSettings = {
  requiredPlayers: number;
  drawDeck: number[];
};

type BingoPlayerSeat = {
  actor: BingoActor;
  seat: number;
  card: BingoCard | null;
  isHost: boolean;
  wins: number;
  losses: number;
};

type BingoRoomSnapshot = {
  roomId: string;
  status: BingoRoomStatus;
  hostActorId: string;
  canStart: boolean;
  drawSeq: number;
  lastDrawnNumber: number | null;
  drawnNumbers: number[];
  players: {
    actorId: string;
    displayName: string;
    seat: number;
    isHost: boolean;
    card: number[];
    marks: boolean[];
    completedLines: number;
    wins: number;
    losses: number;
  }[];
  winners: string[];
};

class BingoRoomGame {
  readonly roomId: string;
  readonly players: BingoPlayerSeat[];
  private readonly settings: BingoRoomSettings;
  private readonly game: BingoGameType;
  private status: BingoRoomStatus;

  constructor(roomId: string, settings: BingoRoomSettings) {
    this.roomId = roomId;
    this.settings = {
      requiredPlayers: settings.requiredPlayers,
      drawDeck: [...settings.drawDeck]
    };
    this.status = BingoRoomStatus.WaitingForPlayers;
    this.players = [];
    this.game = new BingoGame(this.settings.drawDeck);
  }

  join(actor: BingoActor): { joined: boolean; player: BingoPlayerSeat; started: boolean } {
    const existing = this.players.find((player) => player.actor.actorId === actor.actorId);
    if (existing !== undefined) {
      return { joined: false, player: existing, started: false };
    }
    if (this.status !== BingoRoomStatus.WaitingForPlayers || this.players.length >= this.settings.requiredPlayers) {
      throw new Error(`Room ${this.roomId} cannot accept more players.`);
    }
    const player = {
      actor,
      seat: this.players.length,
      card: null,
      isHost: this.players.length === 0,
      wins: 0,
      losses: 0
    };
    this.players.push(player);
    let started = false;
    if (this.players.length === this.settings.requiredPlayers) {
      this.status = BingoRoomStatus.Running;
      started = true;
    }
    return { joined: true, player, started };
  }

  submitCard(actorId: string, cardNumbers: number[]): void {
    if (this.status !== BingoRoomStatus.Running) {
      throw new Error(`Room ${this.roomId} is not running.`);
    }
    const player = this.requirePlayer(actorId);
    player.card = this.game.submitCard({ actorId, card: player.card }, cardNumbers);
  }

  canDraw(): boolean {
    return this.status === BingoRoomStatus.Running
      && this.game.canDraw(this.players.map((player) => ({ actorId: player.actor.actorId, card: player.card })), this.settings.requiredPlayers);
  }

  setPlayerRecord(actorId: string, wins: number, losses: number): void {
    const player = this.requirePlayer(actorId);
    player.wins = wins;
    player.losses = losses;
  }

  drawNext(): { number: number; drawSeq: number; finished: boolean } | null {
    if (!this.canDraw()) {
      return null;
    }
    const drawn = this.game.drawNext(this.players.map((player) => ({ actorId: player.actor.actorId, card: player.card })));
    if (drawn !== null && drawn.finished) {
      this.status = BingoRoomStatus.Finished;
    }
    return drawn;
  }

  snapshot(): BingoRoomSnapshot {
    return {
      roomId: this.roomId,
      status: this.status,
      hostActorId: this.players[0]?.actor.actorId ?? '',
      canStart: false,
      drawSeq: this.game.drawnNumbers.length,
      lastDrawnNumber: this.game.lastDrawnNumber(),
      drawnNumbers: [...this.game.drawnNumbers],
      players: this.players.map((player) => ({
        actorId: player.actor.actorId,
        displayName: player.actor.displayName,
        seat: player.seat,
        isHost: player.isHost,
        card: player.card === null ? [] : [...player.card.numbers],
        marks: player.card === null ? [] : [...player.card.marks],
        completedLines: player.card === null ? 0 : player.card.completedLines(),
        wins: player.wins,
        losses: player.losses
      })),
      winners: [...this.game.winners]
    };
  }

  requirePlayer(actorId: string): BingoPlayerSeat {
    const player = this.players.find((entry) => entry.actor.actorId === actorId);
    if (player === undefined) {
      throw new Error(`Actor '${actorId}' has not joined room '${this.roomId}'.`);
    }
    return player;
  }
}

export { BingoRoomGame, BingoRoomStatus };
export type { BingoActor, BingoPlayerSeat, BingoRoomSettings, BingoRoomSnapshot };
