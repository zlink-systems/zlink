import { Game, Rejection } from '../shared/constants';
import { PlayerView, type MoveRequest, type ShootRequest } from '../shared/messages';
export interface Combatant {
  playerId: string;
  x: number;
  y: number;
  hp: number;
  sequence: number;
}
export class RoomState {
  players: Combatant[] = [];
  winner: string | null = null;
  admit(playerId: string): boolean {
    if (this.players.some((p) => p.playerId === playerId)) return true;
    if (this.players.length >= Game.playersPerRoom) return false;
    this.players.push({ playerId, x: 0, y: 0, hp: Game.initialHp, sequence: 0 });
    return true;
  }
  private player(id: string): Combatant {
    const player = this.players.find((p) => p.playerId === id);
    if (!player) throw new Error('Unknown room player');
    return player;
  }
  private validate(player: Combatant, sequence: number): string | null {
    if (!Number.isSafeInteger(sequence) || sequence <= player.sequence)
      return Rejection.staleSequence;
    player.sequence = sequence;
    return this.winner === null ? null : Rejection.matchFinished;
  }
  move(id: string, move: MoveRequest): string | null {
    const player = this.player(id),
      reason = this.validate(player, move.sequence);
    if (reason) return reason;
    if (!Number.isFinite(move.x) || !Number.isFinite(move.y)) return Rejection.invalidMove;
    player.x = move.x;
    player.y = move.y;
    return null;
  }
  shoot(id: string, shot: ShootRequest): string | null {
    const shooter = this.player(id),
      reason = this.validate(shooter, shot.sequence);
    if (reason) return reason;
    const target = this.players.find(
      (p) => p.playerId === shot.targetId && p.playerId !== id && p.hp > 0
    );
    if (!target) return Rejection.noTarget;
    const dx = target.x - shooter.x,
      dy = target.y - shooter.y;
    if (dx * dx + dy * dy > Game.shotRange * Game.shotRange) return Rejection.outOfRange;
    if (
      Game.walls.some((wall) => {
        // Touching the closed wall segment blocks the shot.
        if (shooter.x === target.x)
          return (
            shooter.x === wall.x &&
            Math.max(shooter.y, target.y) >= wall.minY &&
            Math.min(shooter.y, target.y) <= wall.maxY
          );
        const t = (wall.x - shooter.x) / dx,
          y = shooter.y + t * dy;
        return t >= 0 && t <= 1 && y >= wall.minY && y <= wall.maxY;
      })
    )
      return Rejection.blockedByWall;
    target.hp = Math.max(0, target.hp - Game.shotDamage);
    if (target.hp === 0) this.winner = id;
    return null;
  }
  view(): PlayerView[] {
    return this.players.map((p) => new PlayerView(p.playerId, p.x, p.y, p.hp));
  }
}
