import { performance } from 'node:perf_hooks';
import { Game, Rejection } from '../../shared/constants';
import { ActionRejected, MoveRequest, PlayerPosition } from '../../shared/messages';

interface Position { x: number; sequence: bigint; budget: number; updatedAt: number }
export interface PositionSnapshot { playerId: string; x: number; sequence: bigint; remainingMs: number }

export class Positions {
  private players = new Map<string, Position>();
  constructor(private readonly nowMs: () => number = () => performance.now()) {}
  get size(): number { return this.players.size; }
  has(id: string): boolean { return this.players.has(id); }
  add(id: string): void {
    if (!this.players.has(id)) this.players.set(id, { x: Game.initialX, sequence: Game.initialSequence, budget: Game.maxStep, updatedAt: this.nowMs() });
  }
  remove(id: string): void { this.players.delete(id); }
  ids(): IterableIterator<string> { return this.players.keys(); }
  move(id: string, request: MoveRequest): ActionRejected | null {
    const position = this.players.get(id);
    if (!position) throw new Error('Player is not in this region');
    if (request.sequence > Game.maxSafeInteger) return new ActionRejected(Rejection.integerOutOfRange);
    if (request.sequence <= position.sequence) return new ActionRejected(Rejection.staleSequence);
    if (!Number.isFinite(request.delta)) return new ActionRejected(Rejection.nonFinite);
    const distance = Math.abs(request.delta);
    if (distance > Game.maxStep) return new ActionRejected(Rejection.stepTooLarge);
    const x = position.x + request.delta;
    if (x < Game.regionMinX || x > Game.regionMaxX) return new ActionRejected(Rejection.outsideRegion);
    const now = this.nowMs(), elapsed = Math.max(0, now - position.updatedAt);
    const budget = Math.min(Game.maxStep, position.budget + (Game.maxSpeed * elapsed) / 1000);
    if (distance > budget) return new ActionRejected(Rejection.moveBudget, Math.max(1, Math.ceil(((distance - budget) * 1000) / Game.maxSpeed)));
    position.x = x; position.sequence = request.sequence; position.budget = budget - distance; position.updatedAt = now;
    return null;
  }
  view(): PlayerPosition[] {
    return [...this.players].sort(([a], [b]) => a.localeCompare(b)).map(([playerId, p]) => new PlayerPosition(playerId, p.x, p.sequence));
  }
  capture(): PositionSnapshot[] {
    const now = this.nowMs();
    return [...this.players].map(([playerId, p]) => {
      const budget = Math.min(Game.maxStep, p.budget + (Game.maxSpeed * Math.max(0, now - p.updatedAt)) / 1000);
      return { playerId, x: p.x, sequence: p.sequence, remainingMs: Math.max(0, Math.ceil(((Game.maxStep - budget) * 1000) / Game.maxSpeed)) };
    });
  }
  restore(snapshot: PositionSnapshot[]): void {
    const now = this.nowMs(), restored = new Map<string, Position>();
    for (const p of snapshot) {
      if (!p.playerId || !Number.isFinite(p.x) || p.x < Game.regionMinX || p.x > Game.regionMaxX || p.sequence < 0n || p.sequence > Game.maxSafeInteger || !Number.isSafeInteger(p.remainingMs) || p.remainingMs < 0 || p.remainingMs > (Game.maxStep * 1000) / Game.maxSpeed || restored.has(p.playerId)) throw new Error('Invalid Region position snapshot');
      restored.set(p.playerId, { x: p.x, sequence: p.sequence, budget: Game.maxStep - (p.remainingMs * Game.maxSpeed) / 1000, updatedAt: now });
    }
    this.players = restored;
  }
}
