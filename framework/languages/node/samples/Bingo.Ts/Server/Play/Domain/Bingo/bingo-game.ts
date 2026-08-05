import { BingoCard } from './bingo-card';
import type { BingoCard as BingoCardType } from './bingo-card';

type BingoPlayerGameState = {
  actorId: string;
  card: BingoCardType | null;
};

class BingoGame {
  readonly drawnNumbers: number[];
  readonly winners: string[];
  private readonly drawDeck: number[];

  constructor(drawDeck: number[]) {
    this.drawDeck = [...drawDeck];
    this.drawnNumbers = [];
    this.winners = [];
  }

  submitCard(player: BingoPlayerGameState, cardNumbers: number[]): BingoCardType {
    if (player.card !== null) {
      throw new Error(`Actor '${player.actorId}' already submitted a Bingo card.`);
    }
    return new BingoCard(cardNumbers);
  }

  canDraw(players: BingoPlayerGameState[], requiredPlayers: number): boolean {
    return players.length === requiredPlayers
      && players.every((player) => player.card !== null)
      && this.winners.length === 0
      && this.drawDeck.length > 0;
  }

  drawNext(players: BingoPlayerGameState[]): { number: number; drawSeq: number; finished: boolean } | null {
    if (this.drawDeck.length === 0 || this.winners.length > 0) {
      return null;
    }
    const number = this.drawDeck.shift();
    if (number === undefined) {
      return null;
    }
    this.drawnNumbers.push(number);
    for (const player of players) {
      const card = player.card;
      if (card === null) {
        throw new Error(
          `Actor '${player.actorId}' has no Bingo card for the current round.`
        );
      }
      const lines = card.mark(number);
      if (lines > 0 && !this.winners.includes(player.actorId)) {
        this.winners.push(player.actorId);
      }
    }
    return {
      number,
      drawSeq: this.drawnNumbers.length,
      finished: this.winners.length > 0
    };
  }

  lastDrawnNumber(): number | null {
    return this.drawnNumbers.at(-1) ?? null;
  }
}

export { BingoGame };
export type { BingoPlayerGameState };
