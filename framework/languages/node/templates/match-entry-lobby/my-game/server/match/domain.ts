import { performance } from 'node:perf_hooks';
import { Game, Names, Rejection } from '../shared/constants';
import {
  ActionRejected,
  PlayerView,
  toRetryAfterMs,
  type MoveRequest,
  type ShootRequest,
  type QueueRequest
} from '../shared/messages';

export class MatchmakingState {
  waiting: [string, QueueRequest[]][] = [];
  roomSequence = 0;

  enqueue(request: QueueRequest): void {
    if (!request.mode.trim()) throw new Error('Mode required');
    if (this.waiting.some(([, queue]) => queue.some((p) => p.playerId === request.playerId)))
      return;
    let entry = this.waiting.find(([mode]) => mode === request.mode);
    if (!entry) {
      entry = [request.mode, []];
      this.waiting.push(entry);
    }
    entry[1].push(request);
  }

  pair(): QueueRequest[] {
    return (
      this.waiting
        .find(([, queue]) => queue.length >= Game.playersPerRoom)?.[1]
        .slice(0, Game.playersPerRoom) ?? []
    );
  }

  allocateRoom(): string | null {
    if (this.roomSequence >= Game.maxSafeInteger) return null;
    this.roomSequence++;
    return Names.roomPrefix + this.roomSequence;
  }

  remove(pair: QueueRequest[]): void {
    const index = this.waiting.findIndex(([mode]) => mode === pair[0].mode);
    const queue = this.waiting[index][1];
    queue.splice(0, Game.playersPerRoom);
    if (!queue.length) this.waiting.splice(index, 1);
  }
}

export interface Combatant {
  playerId: string;
  x: number;
  y: number;
  hp: number;
  lastSequence: bigint;
  lastMoveAtMs: number | null;
  lastShotAtMs: number | null;
}

export interface RoomClock {
  now(): number;
}

interface CombatantSnapshot {
  PlayerId: string;
  X: number;
  Y: number;
  Hp: number;
  LastSequence: string;
  MoveBudgetRemainingMs: number | null;
  FireCooldownRemainingMs: number | null;
}

interface RoomSnapshot {
  Phase: string;
  Winner: string | null;
  Players: CombatantSnapshot[];
}

const systemClock: RoomClock = { now: () => performance.now() };
const moveBudgetDurationMs = (Game.maxStep / Game.maxSpeed) * 1000;

export class RoomState {
  private readonly combatants: Combatant[] = [];
  private winner: string | null = null;

  constructor(private readonly clock: RoomClock = systemClock) {}

  get players(): readonly Combatant[] {
    return this.combatants;
  }

  get Winner(): string | null {
    return this.winner;
  }

  get Phase(): string {
    return this.winner === null ? Game.playingPhase : Game.finishedPhase;
  }

  admit(playerId: string): boolean {
    if (this.combatants.some((player) => player.playerId === playerId)) return true;
    if (this.combatants.length >= Game.playersPerRoom) return false;
    const spawn =
      this.combatants.length === 0
        ? [Game.firstSpawnX, Game.firstSpawnY]
        : [Game.secondSpawnX, Game.secondSpawnY];
    this.combatants.push({
      playerId,
      x: spawn[0],
      y: spawn[1],
      hp: Game.initialHp,
      lastSequence: 0n,
      lastMoveAtMs: null,
      lastShotAtMs: null
    });
    return true;
  }

  private player(id: string): Combatant {
    const player = this.combatants.find((candidate) => candidate.playerId === id);
    if (!player) throw new Error('Unknown room player');
    return player;
  }

  private validateAction(player: Combatant, sequence: bigint): ActionRejected | null {
    if (sequence > BigInt(Game.maxSafeInteger))
      return new ActionRejected(Rejection.integerOutOfRange);
    if (sequence <= player.lastSequence) return new ActionRejected(Rejection.staleSequence);
    return this.winner === null ? null : new ActionRejected(Rejection.matchFinished);
  }

  private static commitAcceptedAction(player: Combatant, sequence: bigint): void {
    player.lastSequence = sequence;
  }

  private elapsedMilliseconds(start: number, end: number): number {
    return Math.max(0, end - start);
  }

  private static intersectsWall(ax: number, ay: number, bx: number, by: number): boolean {
    if (ax === bx)
      return (
        ax === Game.wallX && Math.max(ay, by) >= Game.wallMinY && Math.min(ay, by) <= Game.wallMaxY
      );
    const t = (Game.wallX - ax) / (bx - ax);
    const y = ay + t * (by - ay);
    return t >= 0 && t <= 1 && y >= Game.wallMinY && y <= Game.wallMaxY;
  }

  move(id: string, move: MoveRequest): ActionRejected | null {
    const player = this.player(id);
    const rejection = this.validateAction(player, move.sequence);
    if (rejection) return rejection;
    if (!Number.isFinite(move.dx) || !Number.isFinite(move.dy))
      return new ActionRejected(Rejection.invalidMove);

    const distance = Math.sqrt(move.dx * move.dx + move.dy * move.dy);
    if (distance > Game.maxStep) return new ActionRejected(Rejection.stepTooLarge);

    const targetX = player.x + move.dx;
    const targetY = player.y + move.dy;
    if (
      targetX < Game.mapMinX ||
      targetX > Game.mapMaxX ||
      targetY < Game.mapMinY ||
      targetY > Game.mapMaxY
    )
      return new ActionRejected(Rejection.outsideMap);

    if (RoomState.intersectsWall(player.x, player.y, targetX, targetY))
      return new ActionRejected(Rejection.moveBlockedByWall);

    const now = this.clock.now();
    if (player.lastMoveAtMs !== null) {
      const elapsedMs = this.elapsedMilliseconds(player.lastMoveAtMs, now);
      const budget = Math.min(Game.maxStep, (Game.maxSpeed * elapsedMs) / 1000);
      if (distance > budget) {
        const remainingDistance = distance - budget;
        const retryAfterMs = toRetryAfterMs(
          Math.max(1, (remainingDistance * 1000) / Game.maxSpeed)
        );
        return new ActionRejected(Rejection.moveBudget, retryAfterMs);
      }
    }

    player.x = targetX;
    player.y = targetY;
    RoomState.commitAcceptedAction(player, move.sequence);
    player.lastMoveAtMs = now;
    return null;
  }

  shoot(id: string, shot: ShootRequest): ActionRejected | null {
    const shooter = this.player(id);
    const rejection = this.validateAction(shooter, shot.sequence);
    if (rejection) return rejection;

    const target = this.combatants.find(
      (player) => player.playerId === shot.targetId && player.playerId !== id && player.hp > 0
    );
    if (!target) return new ActionRejected(Rejection.noTarget);

    const dx = target.x - shooter.x;
    const dy = target.y - shooter.y;
    if (dx * dx + dy * dy > Game.shotRange * Game.shotRange)
      return new ActionRejected(Rejection.outOfRange);
    if (RoomState.intersectsWall(shooter.x, shooter.y, target.x, target.y))
      return new ActionRejected(Rejection.blockedByWall);

    const now = this.clock.now();
    if (shooter.lastShotAtMs !== null) {
      const retryAfterMs = toRetryAfterMs(
        Game.fireIntervalMs - this.elapsedMilliseconds(shooter.lastShotAtMs, now)
      );
      if (retryAfterMs > 0)
        return new ActionRejected(Rejection.fireTooSoon, retryAfterMs);
    }

    target.hp = Math.max(0, target.hp - Game.shotDamage);
    if (target.hp === 0) this.winner = id;
    RoomState.commitAcceptedAction(shooter, shot.sequence);
    shooter.lastShotAtMs = now;
    return null;
  }

  view(): PlayerView[] {
    return this.combatants.map(
      (player) =>
        new PlayerView(player.playerId, player.x, player.y, player.hp, player.lastSequence)
    );
  }

  capture(): Uint8Array {
    const now = this.clock.now();
    const snapshot: RoomSnapshot = {
      Phase: this.Phase,
      Winner: this.winner,
      Players: this.combatants.map((player) => ({
        PlayerId: player.playerId,
        X: player.x,
        Y: player.y,
        Hp: player.hp,
        LastSequence: player.lastSequence.toString(),
        MoveBudgetRemainingMs: this.remainingMilliseconds(
          player.lastMoveAtMs,
          moveBudgetDurationMs,
          now
        ),
        FireCooldownRemainingMs: this.remainingMilliseconds(
          player.lastShotAtMs,
          Game.fireIntervalMs,
          now
        )
      }))
    };
    return new TextEncoder().encode(JSON.stringify(snapshot));
  }

  restore(payload: Uint8Array): void {
    const snapshot = JSON.parse(new TextDecoder().decode(payload)) as RoomSnapshot;
    const expectedPhase = snapshot.Winner === null ? Game.playingPhase : Game.finishedPhase;
    if (snapshot.Phase !== expectedPhase || snapshot.Players.length > Game.playersPerRoom)
      throw new Error('Invalid room state snapshot');

    const now = this.clock.now();
    this.combatants.splice(
      0,
      this.combatants.length,
      ...snapshot.Players.map((player) => ({
        playerId: player.PlayerId,
        x: player.X,
        y: player.Y,
        hp: player.Hp,
        lastSequence: BigInt(player.LastSequence),
        lastMoveAtMs: this.restoreTimestamp(
          player.MoveBudgetRemainingMs,
          moveBudgetDurationMs,
          now
        ),
        lastShotAtMs: this.restoreTimestamp(
          player.FireCooldownRemainingMs,
          Game.fireIntervalMs,
          now
        )
      }))
    );
    this.winner = snapshot.Winner;
  }

  private remainingMilliseconds(
    lastAtMs: number | null,
    durationMs: number,
    now: number
  ): number | null {
    if (lastAtMs === null) return null;
    return Math.max(0, Math.ceil(durationMs - this.elapsedMilliseconds(lastAtMs, now)));
  }

  private restoreTimestamp(
    remainingMs: number | null,
    durationMs: number,
    now: number
  ): number | null {
    if (remainingMs === null) return null;
    const boundedRemaining = Math.max(0, Math.min(remainingMs, durationMs));
    return now - (durationMs - boundedRemaining);
  }
}
