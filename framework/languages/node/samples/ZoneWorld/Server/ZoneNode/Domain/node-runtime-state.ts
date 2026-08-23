import { Inject, Injectable } from '@nestjs/common';
import { ZONEWORLD_CONFIG } from '../../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../../Configuration/configuration';

@Injectable()
class NodeRuntimeState {
  private readonly maintenance = new Map<string, boolean>();
  private readonly playerZones = new Map<string, Set<string>>();
  private readonly hostedZones = new Set<string>();
  private botTicksEnabled = false;
  readonly nodeId: string;

  constructor(@Inject(ZONEWORLD_CONFIG) config: ZoneWorldConfiguration) {
    if (config.zoneNode === undefined) throw new Error('ZoneNode configuration is required.');
    this.nodeId = config.zoneNode.nodeId;
  }

  restore(values: ReadonlyMap<string, boolean>): void {
    for (const [nodeId, enabled] of values) this.maintenance.set(nodeId, enabled);
  }

  setMaintenance(nodeId: string, enabled: boolean): void {
    this.maintenance.set(nodeId, enabled);
  }

  ownMaintenance(): boolean {
    return this.maintenance.get(this.nodeId) ?? false;
  }

  isUnderMaintenance(nodeId: string): boolean {
    return this.maintenance.get(nodeId) ?? false;
  }

  rejectsArrival(): boolean {
    return this.ownMaintenance();
  }

  hostZone(zoneId: string): void {
    this.hostedZones.add(zoneId);
  }

  releaseZone(zoneId: string): void {
    this.hostedZones.delete(zoneId);
  }

  zones(): readonly string[] {
    return [...this.hostedZones].sort((left, right) => Buffer.from(left).compare(Buffer.from(right)));
  }

  joined(playerId: string, zoneId: string): void {
    const zones = this.playerZones.get(playerId) ?? new Set<string>();
    zones.add(zoneId);
    this.playerZones.set(playerId, zones);
  }

  left(playerId: string, zoneId: string): void {
    const zones = this.playerZones.get(playerId);
    if (zones === undefined) return;
    zones.delete(zoneId);
    if (zones.size === 0) this.playerZones.delete(playerId);
  }

  playerCount(): number {
    return this.playerZones.size;
  }

  enableBotTicks(): void {
    this.botTicksEnabled = true;
  }

  canTickBots(): boolean {
    return this.botTicksEnabled;
  }
}

export { NodeRuntimeState };
