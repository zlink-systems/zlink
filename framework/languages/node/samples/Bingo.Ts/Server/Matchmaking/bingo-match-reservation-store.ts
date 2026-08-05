import { Injectable, type OnModuleDestroy } from '@nestjs/common';
import { randomUUID } from 'node:crypto';
import { createClient, type RedisClientType } from 'redis';
import { BINGO_SAMPLE_CONFIG } from '../Configuration/sample-config';
import { BingoRoomSettingsPayload, ReserveBingoRoomRes } from '../../Shared/Contracts/bingo-messages.generated';
import type { BingoSampleConfig } from '../Configuration/sample-config';
import type { ReserveBingoRoomReq } from '../../Shared/Contracts/bingo-messages.generated';
import { Inject } from '@nestjs/common';

@Injectable()
class BingoMatchReservationStore implements OnModuleDestroy {
  private readonly client: RedisClientType;
  private connecting?: Promise<unknown>;

  constructor(@Inject(BINGO_SAMPLE_CONFIG) private readonly config: BingoSampleConfig) {
    this.client = createClient({ url: `redis://${config.redisEndpoint}` });
  }

  async reserve(request: ReserveBingoRoomReq): Promise<ReserveBingoRoomRes> {
    await this.ensureConnected();
    const key = `${this.config.redisKeyPrefix}match:${request.levelBucket}:${request.mode}`;
    let roomId = await this.client.get(key) as string | null;
    if (roomId === null) {
      const candidate = `room-${randomUUID()}`;
      const stored = await this.client.set(key, candidate, { NX: true, EX: 60 });
      roomId = stored === 'OK' ? candidate : await this.client.get(key) as string | null;
    }
    if (roomId === null) throw new Error('Match reservation could not select a room.');
    await this.client.hSet(`${key}:actors`, request.actorId, Date.now().toString());
    await this.client.expire(`${key}:actors`, 60);
    return new ReserveBingoRoomRes({
      roomId,
      settings: new BingoRoomSettingsPayload({
        roomName: roomId,
        mode: request.mode,
        requiredPlayers: 2,
        maxDrawNumber: 15,
        purpose: 'play',
        observedRoomId: null
      })
    });
  }

  async onModuleDestroy(): Promise<void> {
    if (this.client.isOpen) await this.client.quit();
  }

  private async ensureConnected(): Promise<void> {
    if (this.client.isOpen) return;
    this.connecting ??= this.client.connect();
    await this.connecting;
  }
}

export { BingoMatchReservationStore };
