import { Inject, Injectable } from '@nestjs/common';
import { ZONEWORLD_CONFIG } from '../../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../../Configuration/configuration';
import { nodeOf } from '../../../Shared/spec';

@Injectable()
class NodeRuntimeState {
  private readonly maintenance = new Map<string, boolean>();
  private readonly playerZones = new Map<string, Set<string>>();
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

  rejectsArrival(zoneId: string, sourceNodeId: string | null): boolean {
    return nodeOf(zoneId as never) === this.nodeId
      && sourceNodeId !== this.nodeId
      && this.ownMaintenance();
  }

  targetUnavailable(zoneId: string): boolean {
    const targetNodeId = nodeOf(zoneId as never);
    return targetNodeId !== this.nodeId && (this.maintenance.get(targetNodeId) ?? false);
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
