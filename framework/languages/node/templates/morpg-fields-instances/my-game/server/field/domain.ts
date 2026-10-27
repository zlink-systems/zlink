import { Game, fieldId } from '../shared/constants';
import { ActionRejected, EnterField, MaxSafeInteger, MoveRequest } from '../shared/messages';
interface PositionState { x: number; sequence: bigint; acceptedAt: number; connected: boolean }
export class FieldState {
  static readonly maxStep = 10;
  static readonly maxSpeed = FieldState.maxStep * 5;
  static readonly fullBudgetMs = 200;
  private players: Record<string, PositionState> = Object.create(null);
  private arrivals: Record<string, EnterField> = Object.create(null);
  constructor(private readonly clock: () => number = () => Math.floor(performance.now())) {}
  get visible(): Record<string, EnterField> {
    return Object.fromEntries(Object.entries(this.players).filter(([, p]) => p.connected)
      .map(([id, p]) => [id, new EnterField(p.x, p.sequence, this.remaining(p))]));
  }
  private remaining(p: PositionState): number {
    return Math.max(0, Math.min(FieldState.fullBudgetMs,
      Math.ceil(FieldState.fullBudgetMs - Math.max(0, this.clock() - p.acceptedAt))));
  }
  admit(id: string, p: EnterField, target: string): boolean {
    if (!Number.isFinite(p.x) || p.x < 0 || p.x >= Game.fieldWidth * Game.fieldCount
      || fieldId(p.x) !== target || p.sequence < 0n || p.sequence > MaxSafeInteger
      || p.remainingMs < 0 || p.remainingMs > FieldState.fullBudgetMs) return false;
    this.arrivals[id] = p;
    return true;
  }
  arrive(id: string): EnterField {
    const p = this.arrivals[id];
    if (!p) throw new Error('Field admission has no movement intent');
    delete this.arrivals[id];
    this.players[id] = { x: p.x, sequence: p.sequence, connected: true,
      acceptedAt: this.clock() - (FieldState.fullBudgetMs - p.remainingMs) };
    return p;
  }
  leave(id: string): void { delete this.players[id]; }
  connected(id: string, value: boolean): void { if (this.players[id]) this.players[id].connected = value; }
  move(id: string, request: MoveRequest): EnterField | ActionRejected {
    const p = this.players[id];
    if (!p) return new ActionRejected('stale-sequence');
    if (!Number.isFinite(request.delta)) return new ActionRejected('non-finite');
    if (Math.abs(request.delta) > FieldState.maxStep) return new ActionRejected('step-too-large');
    if (request.sequence > MaxSafeInteger) return new ActionRejected('integer-out-of-range');
    if (request.sequence < 0n || request.sequence <= p.sequence || p.sequence >= MaxSafeInteger)
      return new ActionRejected('stale-sequence');
    const next = p.x + request.delta;
    if (next < 0 || next >= Game.fieldWidth * Game.fieldCount) return new ActionRejected('outside-world');
    const now = this.clock(), budget = Math.min(FieldState.maxStep,
      FieldState.maxSpeed * Math.max(0, now - p.acceptedAt) / 1000);
    if (Math.abs(request.delta) > budget)
      return new ActionRejected('move-budget', Math.max(1,
        Math.ceil((Math.abs(request.delta) - budget) * 1000 / FieldState.maxSpeed)));
    const result = new EnterField(next, request.sequence, FieldState.fullBudgetMs);
    if (fieldId(next) === fieldId(p.x)) {
      p.x = next; p.sequence = request.sequence; p.acceptedAt = now;
    }
    return result;
  }
  capture(): object {
    return { players: Object.fromEntries(Object.entries(this.players).map(([id, p]) =>
      [id, { x: p.x, sequence: p.sequence.toString(), remainingMs: this.remaining(p), connected: p.connected }])),
      arrivals: this.arrivals };
  }
  restore(snapshot: { players: Record<string, { x: number; sequence: string; remainingMs: number; connected: boolean }>;
    arrivals: Record<string, EnterField> }): void {
    this.players = Object.fromEntries(Object.entries(snapshot.players).map(([id, p]) => [id,
      { x: p.x, sequence: BigInt(p.sequence), connected: p.connected,
        acceptedAt: this.clock() - (FieldState.fullBudgetMs - p.remainingMs) }]));
    this.arrivals = Object.fromEntries(Object.entries(snapshot.arrivals).map(([id, p]) =>
      [id, new EnterField(p.x, BigInt(p.sequence), p.remainingMs)]));
  }
}
