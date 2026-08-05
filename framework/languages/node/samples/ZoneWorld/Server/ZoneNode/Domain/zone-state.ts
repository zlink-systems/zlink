import { ZoneWorldSpec } from '../../../Shared/spec';
import { inBorderBand } from './world';
import type { PlayerView } from '../../../Shared/contracts';
import type { ZoneId } from '../../../Shared/spec';

type ResidentPlayer = { playerId: string; x: number; y: number; isBot: boolean };
type BorderSnapshot = { tick: number; receivedAtTick: number; players: readonly PlayerView[] };

class ZoneState {
  private readonly residents = new Map<string, ResidentPlayer>();
  private readonly adjacent = new Map<string, BorderSnapshot>();
  private readonly borderHighWater = new Map<string, number>();
  private currentTick = 0;

  constructor(readonly zoneId: ZoneId) {}

  get tick(): number { return this.currentTick; }
  get playerCount(): number { return this.residents.size; }

  enter(playerId: string, x: number, y: number, isBot: boolean): void {
    this.residents.set(playerId, { playerId, x, y, isBot });
  }

  updatePosition(playerId: string, x: number, y: number): void {
    const resident = this.residents.get(playerId);
    if (resident !== undefined) this.residents.set(playerId, { ...resident, x, y });
  }

  leave(playerId: string): void { this.residents.delete(playerId); }
  nextTick(): number { return ++this.currentTick; }

  applyBorderSnapshot(fromZoneId: string, tick: number, players: readonly PlayerView[]): void {
    if (tick <= (this.borderHighWater.get(fromZoneId) ?? -1)) return;
    this.borderHighWater.set(fromZoneId, tick);
    this.adjacent.set(fromZoneId, { tick, receivedAtTick: this.currentTick, players });
  }

  expireStaleSnapshots(): string[] {
    const expired: string[] = [];
    for (const [zoneId, snapshot] of this.adjacent) {
      if (this.currentTick - snapshot.receivedAtTick >= ZoneWorldSpec.borderSnapshotExpiryTicks) {
        this.adjacent.delete(zoneId);
        expired.push(zoneId);
      }
    }
    return expired;
  }

  visiblePlayers(): PlayerView[] {
    const merged = new Map<string, PlayerView>();
    for (const snapshot of this.adjacent.values()) {
      for (const player of snapshot.players) merged.set(player.playerId, player);
    }
    for (const resident of this.residents.values()) {
      merged.set(resident.playerId, { ...resident, zoneId: this.zoneId });
    }
    return [...merged.values()].sort((left, right) => Buffer.from(left.playerId).compare(Buffer.from(right.playerId)));
  }

  borderBandFor(toZoneId: ZoneId): PlayerView[] {
    return [...this.residents.values()]
      .filter((resident) => inBorderBand(resident.x, resident.y, this.zoneId, toZoneId))
      .map((resident) => ({ ...resident, zoneId: this.zoneId }))
      .sort((left, right) => Buffer.from(left.playerId).compare(Buffer.from(right.playerId)));
  }
}

export { ZoneState };
