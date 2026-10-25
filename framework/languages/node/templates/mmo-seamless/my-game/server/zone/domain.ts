import { Game, zoneId } from '../shared/constants';
import { ActionRejected, type EnterZone, type MoveRequest, Position } from '../shared/messages';

export interface PlayerView {
  position: Position;
  sequence: bigint;
  remainingMs: number;
  connected: boolean;
}
export interface Movement {
  accepted: boolean;
  position: Position;
  sequence: bigint;
  remainingMs: number;
  zoneId: string;
  relocates: boolean;
  rejection?: ActionRejected;
}
interface PlayerState {
  position: Position;
  sequence: bigint;
  acceptedAt: number;
  connected: boolean;
}
export interface ZoneSnapshot {
  zoneId: string;
  players: [string, Omit<PlayerView, 'remainingMs'> & { remainingMs: number }][];
  arrivals: [string, EnterZone][];
}

export class ZonePositionDomain {
  readonly maxStep = Game.maxStep;
  readonly maxSpeed = Game.maxSpeed;
  readonly fullBudgetAfterMs = Game.fullBudgetAfterMs;
  private readonly players = new Map<string, PlayerState>();
  private readonly arrivals = new Map<string, EnterZone>();
  constructor(
    readonly zoneId: string,
    private readonly clock: () => number = () => Math.floor(performance.now())
  ) {}
  static resolveLocation(x: number): string | ActionRejected {
    if (!Number.isFinite(x)) return new ActionRejected('non-finite', 0);
    if (x < 0 || x >= Game.zoneWidth * Game.zoneCount) return new ActionRejected('outside-world', 0);
    return zoneId(x);
  }
  private remainingMs(state: PlayerState, now = this.clock()): number {
    return Math.max(0, Math.min(this.fullBudgetAfterMs, this.fullBudgetAfterMs - Math.max(0, now - state.acceptedAt)));
  }
  private view(state: PlayerState): PlayerView {
    return { position: state.position, sequence: state.sequence, remainingMs: this.remainingMs(state), connected: state.connected };
  }
  queueArrival(id: string, arrival: EnterZone): ActionRejected | undefined {
    const location = ZonePositionDomain.resolveLocation(arrival.position.x);
    if (location instanceof ActionRejected) return location;
    if (arrival.sequence > BigInt(Game.maxSafeInteger) || arrival.remainingMs < 0 || arrival.remainingMs > this.fullBudgetAfterMs)
      return new ActionRejected('integer-out-of-range', 0);
    if (arrival.sequence < 0n) return new ActionRejected('stale-sequence', 0);
    if (location !== this.zoneId) return new ActionRejected('outside-world', 0);
    if (this.arrivals.has(id)) throw new Error('Duplicate zone admission');
    this.arrivals.set(id, arrival);
    return undefined;
  }
  takeArrival(id: string): EnterZone | undefined {
    const arrival = this.arrivals.get(id);
    this.arrivals.delete(id);
    return arrival;
  }
  addPlayer(id: string, arrival: EnterZone, connected = true): void {
    this.players.set(id, {
      position: arrival.position,
      sequence: arrival.sequence,
      acceptedAt: this.clock() - (this.fullBudgetAfterMs - arrival.remainingMs),
      connected
    });
  }
  move(id: string, request: MoveRequest): Movement {
    const state = this.players.get(id);
    if (!state) return { accepted: false, position: new Position(0), sequence: 0n, remainingMs: 0, zoneId: this.zoneId, relocates: false, rejection: new ActionRejected('stale-sequence', 0) };
    const reject = (reason: string, retryAfterMs = 0): Movement => ({ accepted: false, position: state.position, sequence: state.sequence, remainingMs: this.remainingMs(state), zoneId: this.zoneId, relocates: false, rejection: new ActionRejected(reason, retryAfterMs) });
    if (!Number.isFinite(request.delta)) return reject('non-finite');
    if (Math.abs(request.delta) > this.maxStep) return reject('step-too-large');
    if (request.sequence > BigInt(Game.maxSafeInteger)) return reject('integer-out-of-range');
    if (request.sequence < 0n || state.sequence >= BigInt(Game.maxSafeInteger) || request.sequence <= state.sequence) return reject('stale-sequence');
    const position = new Position(state.position.x + request.delta);
    const location = ZonePositionDomain.resolveLocation(position.x);
    if (location instanceof ActionRejected) return reject(location.reason);
    const now = this.clock();
    const budget = Math.min(this.maxStep, (this.maxSpeed * Math.max(0, now - state.acceptedAt)) / 1000);
    const distance = Math.abs(request.delta);
    if (distance > budget) return reject('move-budget', Math.max(1, Math.ceil(((distance - budget) * 1000) / this.maxSpeed)));
    const relocates = location !== this.zoneId;
    if (!relocates) this.players.set(id, { position, sequence: request.sequence, acceptedAt: now, connected: state.connected });
    return { accepted: true, position, sequence: request.sequence, remainingMs: this.fullBudgetAfterMs, zoneId: location, relocates };
  }
  state(id: string): PlayerView | undefined {
    const state = this.players.get(id);
    return state ? this.view(state) : undefined;
  }
  *connectedPlayers(): Iterable<[string, PlayerView]> {
    for (const [id, state] of this.players) if (state.connected) yield [id, this.view(state)];
  }
  setConnected(id: string, connected: boolean): PlayerView | undefined {
    const state = this.players.get(id);
    if (!state) return undefined;
    state.connected = connected;
    return this.view(state);
  }
  removePlayer(id: string): PlayerView | undefined {
    const state = this.players.get(id);
    if (!state) return undefined;
    this.players.delete(id);
    return this.view(state);
  }
  capture(): ZoneSnapshot {
    return {
      zoneId: this.zoneId,
      players: [...this.players].map(([id, state]) => [id, this.view(state)]),
      arrivals: [...this.arrivals]
    };
  }
  restore(snapshot: ZoneSnapshot): void {
    if (snapshot.zoneId !== this.zoneId) throw new Error('Zone relocation state belongs to a different Zone');
    this.players.clear();
    this.arrivals.clear();
    for (const [id, state] of snapshot.players)
      this.players.set(id, { position: state.position, sequence: BigInt(state.sequence), acceptedAt: this.clock() - (this.fullBudgetAfterMs - state.remainingMs), connected: state.connected });
    for (const [id, arrival] of snapshot.arrivals)
      this.arrivals.set(id, {
        position: arrival.position,
        sequence: BigInt(arrival.sequence),
        remainingMs: arrival.remainingMs
      });
  }
}
