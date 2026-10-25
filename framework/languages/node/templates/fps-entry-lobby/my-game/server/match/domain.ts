import { performance } from 'node:perf_hooks';
import { Game, MatchPhase, Rejection, ShotOutcome } from '../shared/constants';
import {
  ActionRejected,
  PlayerView,
  ShotRejected,
  ShotResult,
  type MoveRequest,
  type ShootRequest
} from '../shared/messages';
import {
  addVec,
  ArenaMap,
  isFiniteVec,
  normalizeVec,
  Vec3,
  vecLength,
  type Vec3Like
} from '../shared/geometry';

export interface Combatant {
  playerId: string;
  position: Vec3Like;
  hp: number;
  sequence: number;
  lastMoveMs: number | null;
  nextFireMs: number | null;
}

export interface CombatantSnapshot {
  playerId: string;
  position: Vec3;
  hp: number;
  sequence: number;
  fireRemainingMs: number;
  moveRemainingMs: number;
}

export interface RoomSnapshot {
  players: CombatantSnapshot[];
  phase: MatchPhase;
  winner: string | null;
}

export interface ShotDecision {
  rejected: ShotRejected | null;
  result: ShotResult | null;
}

const MovementWindowMs = (Game.maxStep * 1000) / Game.maxSpeed;

export class RoomState {
  players: Combatant[] = [];
  private currentPhase = MatchPhase.Waiting;
  private currentWinner: string | null = null;

  constructor(private readonly nowMs: () => number = () => performance.now()) {}

  get phase(): MatchPhase {
    return this.currentPhase;
  }

  get winner(): string | null {
    return this.currentWinner;
  }

  admit(playerId: string): boolean {
    if (this.players.some((player) => player.playerId === playerId)) return true;
    if (this.players.length >= Game.playersPerRoom) return false;
    this.players.push({
      playerId,
      position: ArenaMap.spawns[this.players.length],
      hp: Game.initialHp,
      sequence: 0,
      lastMoveMs: null,
      nextFireMs: null
    });
    if (this.players.length === Game.playersPerRoom) this.currentPhase = MatchPhase.Playing;
    return true;
  }

  private player(id: string): Combatant {
    const player = this.players.find((candidate) => candidate.playerId === id);
    if (!player) throw new Error('Unknown room player');
    return player;
  }

  private validateAction(player: Combatant, sequence: number): string | null {
    if (!Number.isSafeInteger(sequence) || sequence <= player.sequence) return Rejection.staleSequence;
    if (this.currentPhase === MatchPhase.Finished) return Rejection.matchFinished;
    return null;
  }

  move(id: string, move: MoveRequest): ActionRejected | null {
    const player = this.player(id),
      reason = this.validateAction(player, move.sequence);
    if (reason) return new ActionRejected(reason);
    if (!isFiniteVec(move.delta)) return new ActionRejected(Rejection.nonFinite);
    if (move.delta.y !== 0) return new ActionRejected(Rejection.verticalMove);
    const distance = vecLength(move.delta);
    if (distance > Game.maxStep) return new ActionRejected(Rejection.stepTooLarge);
    const next = addVec(player.position, move.delta);
    if (!ArenaMap.bounds.contains(next)) return new ActionRejected(Rejection.outsideMap);
    if (ArenaMap.walls.some((wall) => wall.intersect(player.position, move.delta, 1) !== null))
      return new ActionRejected(Rejection.moveBlockedByWall);
    const now = this.nowMs();
    if (player.lastMoveMs !== null) {
      const elapsed = Math.max(0, now - player.lastMoveMs),
        budget = Math.min(Game.maxStep, (Game.maxSpeed * elapsed) / 1000);
      if (distance > budget)
        return new ActionRejected(
          Rejection.moveBudget,
          Math.max(1, Math.ceil((distance * 1000) / Game.maxSpeed - elapsed))
        );
    }
    player.position = next;
    player.sequence = move.sequence;
    player.lastMoveMs = now;
    return null;
  }

  shoot(id: string, shot: ShootRequest): ShotDecision {
    const shooter = this.player(id),
      reason = this.validateAction(shooter, shot.sequence);
    if (reason) return { rejected: new ShotRejected(reason), result: null };
    const direction = normalizeVec(shot.aimDirection);
    if (!direction) return { rejected: new ShotRejected(Rejection.invalidAim), result: null };
    const now = this.nowMs();
    if (shooter.nextFireMs !== null && now < shooter.nextFireMs)
      return {
        rejected: new ShotRejected(
          Rejection.fireTooSoon,
          Math.max(1, Math.ceil(shooter.nextFireMs - now))
        ),
        result: null
      };
    const origin = addVec(shooter.position, new Vec3(0, Game.eyeHeight, 0));
    let wallDistance = Number.POSITIVE_INFINITY;
    for (const wall of ArenaMap.walls) {
      const distance = wall.intersect(origin, direction, Game.shotRange);
      if (distance !== null) wallDistance = Math.min(wallDistance, distance);
    }
    let target: Combatant | null = null,
      targetDistance = Number.POSITIVE_INFINITY;
    for (const candidate of this.players) {
      if (candidate === shooter || candidate.hp === 0) continue;
      const distance = ArenaMap.playerBox(candidate.position).intersect(
        origin,
        direction,
        Game.shotRange
      );
      if (distance !== null && distance < targetDistance) {
        target = candidate;
        targetDistance = distance;
      }
    }
    let outcome: string = ShotOutcome.miss;
    if (wallDistance <= targetDistance && Number.isFinite(wallDistance)) {
      outcome = ShotOutcome.blockedByWall;
    } else if (target) {
      outcome = ShotOutcome.hit;
      target.hp = Math.max(0, target.hp - Game.shotDamage);
      if (target.hp === 0) {
        this.currentPhase = MatchPhase.Finished;
        this.currentWinner = id;
      }
    }
    shooter.sequence = shot.sequence;
    shooter.nextFireMs = now + Game.fireIntervalMs;
    return {
      rejected: null,
      result: new ShotResult(outcome, outcome === ShotOutcome.hit ? target!.playerId : null, shot.sequence)
    };
  }

  view(): PlayerView[] {
    return this.players.map(
      (player) => new PlayerView(player.playerId, new Vec3(player.position.x, player.position.y, player.position.z), player.hp, player.sequence)
    );
  }

  capture(): RoomSnapshot {
    const now = this.nowMs();
    return {
      players: this.players.map((player) => ({
        playerId: player.playerId,
        position: new Vec3(player.position.x, player.position.y, player.position.z),
        hp: player.hp,
        sequence: player.sequence,
        fireRemainingMs: player.nextFireMs === null ? 0 : Math.max(0, player.nextFireMs - now),
        moveRemainingMs:
          player.lastMoveMs === null
            ? 0
            : Math.max(0, MovementWindowMs - (now - player.lastMoveMs))
      })),
      phase: this.currentPhase,
      winner: this.currentWinner
    };
  }

  restore(snapshot: RoomSnapshot): void {
    const now = this.nowMs();
    this.players = snapshot.players.map((player) => ({
      playerId: player.playerId,
      position: new Vec3(player.position.x, player.position.y, player.position.z),
      hp: player.hp,
      sequence: player.sequence,
      nextFireMs: now + player.fireRemainingMs,
      lastMoveMs:
        player.moveRemainingMs > 0 ? now - (MovementWindowMs - player.moveRemainingMs) : null
    }));
    this.currentPhase = snapshot.phase;
    this.currentWinner = snapshot.winner;
  }
}
