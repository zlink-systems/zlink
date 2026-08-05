import { Inject, Injectable, type OnModuleDestroy } from '@nestjs/common';
import { createClient } from 'redis';
import { ZONEWORLD_CONFIG } from './configuration';
import type { ZoneWorldConfiguration } from './configuration';

@Injectable()
class MaintenanceStore implements OnModuleDestroy {
  private readonly client;
  private readonly key: string;
  private connected?: Promise<void>;

  constructor(@Inject(ZONEWORLD_CONFIG) config: ZoneWorldConfiguration) {
    this.client = createClient({ url: `redis://${config.shared.redisEndpoint}` });
    this.key = `${config.shared.redisKeyPrefix}maintenance`;
  }

  async readAll(): Promise<ReadonlyMap<string, boolean>> {
    await this.connect();
    const values = await this.client.hGetAll(this.key);
    return new Map(Object.entries(values).map(([nodeId, value]) => [nodeId, value === '1']));
  }

  async write(nodeId: string, enabled: boolean): Promise<void> {
    await this.connect();
    await this.client.hSet(this.key, nodeId, enabled ? '1' : '0');
  }

  async onModuleDestroy(): Promise<void> {
    if (this.client.isOpen) await this.client.quit();
  }

  private async connect(): Promise<void> {
    this.connected ??= this.client.connect().then(() => undefined);
    await this.connected;
  }
}

export { MaintenanceStore };
