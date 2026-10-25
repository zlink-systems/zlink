import { Inject, Injectable } from '@nestjs/common';
import { createClient } from 'redis';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Names } from '../shared/constants';
@Injectable()
export class PlayerData {
  private readonly client;
  constructor(@Inject(NODE_CONFIG) private readonly config: NodeConfig) {
    this.client = createClient({ url: `redis://${config.redisEndpoint}` });
    this.client.on('error', (error) => console.error('Player data Redis error', error));
  }
  async onModuleInit(): Promise<void> {
    await this.client.connect();
  }
  async onModuleDestroy(): Promise<void> {
    await this.client.quit();
  }
  private key(id: string): string {
    return this.config.keyPrefix + Names.playerKey + id;
  }
  async load(id: string): Promise<bigint> {
    const value = await this.client.get(this.key(id));
    return value === null ? 0n : BigInt(value);
  }
  async save(id: string, gold: bigint): Promise<void> {
    await this.client.set(this.key(id), gold.toString());
  }
}
