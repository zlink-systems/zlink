import { Inject, Injectable, type OnModuleDestroy, type OnModuleInit } from '@nestjs/common';
import type { ZLinkRequestHandler } from '@zlink-systems/framework';
import { zlinkRequestHandler } from '@zlink-systems/nestjs';
import { createClient } from 'redis';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Game, Names, PacketNames } from '../shared/constants';
import { CommitRound, LoadPlayer, PlayerRecord, RoundCommitted } from '../shared/messages';

// One MSET writes the marker and all player records together. Redis runs the
// marker check and the write atomically, including across API processes.
const commitRound = `
if redis.call('EXISTS', KEYS[1]) == 0 then
  local entries = { KEYS[1], '1' }
  for i = 2, #KEYS do
    entries[#entries + 1] = KEYS[i]
    entries[#entries + 1] = ARGV[i - 1]
  end
  redis.call('MSET', unpack(entries))
end
return 1
`;

@Injectable()
export class RedisPlayerRecords implements OnModuleInit, OnModuleDestroy {
  private readonly redis: ReturnType<typeof createClient>;
  private readonly prefix: string;

  constructor(@Inject(NODE_CONFIG) config: NodeConfig) {
    this.prefix = config.keyPrefix;
    this.redis = createClient({ url: `redis://${config.redisEndpoint}` });
    this.redis.on('error', (error) => console.error('Player records Redis error', error));
  }

  async onModuleInit(): Promise<void> {
    await this.redis.connect();
  }

  async onModuleDestroy(): Promise<void> {
    await this.redis.close();
  }

  private playerKey(playerId: string): string {
    return `${this.prefix}players:${playerId}`;
  }

  async loadPlayer(playerId: string): Promise<bigint> {
    const key = this.playerKey(playerId);
    const [, chips] = await this.redis
      .multi()
      .set(key, Game.initialChips.toString(), { condition: 'NX' })
      .get(key)
      .execTyped();
    if (chips === null) throw new Error(`Missing chips for player ${playerId}`);
    return BigInt(chips);
  }

  async commitRound(request: CommitRound): Promise<void> {
    const roundKey = `${this.prefix}rounds:${request.tableId}:${request.handNumber}`;
    const keys = [roundKey, ...request.results.map((result) => this.playerKey(result.playerId))];
    const chips = request.results.map((result) => result.chips.toString());
    await this.redis.eval(commitRound, { keys, arguments: chips });
  }
}

@zlinkRequestHandler(Names.playerRecords, PacketNames.loadPlayer)
export class LoadPlayerHandler implements ZLinkRequestHandler<LoadPlayer, PlayerRecord> {
  constructor(private readonly records: RedisPlayerRecords) {}

  async handle(request: LoadPlayer): Promise<PlayerRecord> {
    return new PlayerRecord(request.playerId, await this.records.loadPlayer(request.playerId));
  }
}

@zlinkRequestHandler(Names.playerRecords, PacketNames.commitRound)
export class CommitRoundHandler implements ZLinkRequestHandler<CommitRound, RoundCommitted> {
  constructor(private readonly records: RedisPlayerRecords) {}

  async handle(request: CommitRound): Promise<RoundCommitted> {
    await this.records.commitRound(request);
    return new RoundCommitted(request.tableId, request.handNumber);
  }
}
