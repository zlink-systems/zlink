import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_CHANNEL_CLIENT } from '@zlink-systems/nestjs';
import type { ZLinkChannelClient } from '@zlink-systems/framework';
import { Names } from '../../shared/constants';
import {
  CommitRound,
  LoadPlayer,
  type PlayerRecord,
  type RoundCommitted
} from '../../shared/messages';
import { PlayerRecords } from '../application/player-records';

@Injectable()
export class PlayerRecordsChannel implements PlayerRecords {
  constructor(@Inject(ZLINK_CHANNEL_CLIENT) private readonly channel: ZLinkChannelClient) {}

  async loadPlayer(playerId: string): Promise<bigint> {
    const reply = await this.channel
      .requestToChannel(Names.playerRecords, new LoadPlayer(playerId))
      .submit<PlayerRecord>();
    return reply.chips;
  }

  async commitRound(
    tableId: string,
    handNumber: bigint,
    results: readonly { playerId: string; chips: bigint }[]
  ): Promise<void> {
    await this.channel
      .requestToChannel(Names.playerRecords, new CommitRound(tableId, handNumber, results))
      .submit<RoundCommitted>();
  }
}
