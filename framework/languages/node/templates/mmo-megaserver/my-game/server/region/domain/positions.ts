import { Game } from '../../shared/constants';
export interface Position {
  x: number;
  sequence: number;
}
export class Positions {
  private players = new Map<string, Position>();
  admit(id: string, capacity: number): boolean {
    if (this.players.has(id)) return true;
    if (this.players.size >= capacity) return false;
    this.players.set(id, { x: Game.initialX, sequence: Game.initialSequence });
    return true;
  }
  remove(id: string): void {
    this.players.delete(id);
  }
  move(id: string, x: number, sequence: number): void {
    const position = this.players.get(id);
    if (!position) throw new Error('Player is not in this region');
    if (
      !Number.isSafeInteger(x) ||
      !Number.isSafeInteger(sequence) ||
      sequence <= position.sequence
    )
      throw new Error('Move sequence must increase and position must be an integer');
    position.x = x;
    position.sequence = sequence;
  }
  ids(): IterableIterator<string> {
    return this.players.keys();
  }
  view(): { playerId: string; x: number }[] {
    return [...this.players]
      .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
      .map(([playerId, position]) => ({ playerId, x: position.x }));
  }
  snapshot(): [string, Position][] {
    return [...this.players];
  }
  restore(snapshot: [string, Position][]): void {
    this.players = new Map(snapshot);
  }
}
