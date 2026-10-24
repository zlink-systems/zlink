import { Inject, Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Control } from '../shared/constants';
import { PlayerData } from './data';
import { GoldBalance } from './domain';
export class Player implements ZLinkActor {
  readonly gold: GoldBalance;
  constructor(
    readonly context: ZLinkActorContext,
    private readonly data: PlayerData,
    gold: number
  ) {
    this.gold = new GoldBalance(gold);
  }
  async earn(amount: number): Promise<void> {
    const next = this.gold.afterEarning(amount);
    await this.data.save(this.context.actorId, next);
    this.gold.restore(next);
  }
  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status !== 'accepted') throw new Error(`Zone join failed: ${completion.status}`);
  }
}
@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  constructor(
    private readonly data: PlayerData,
    @Inject(NODE_CONFIG) private readonly config: NodeConfig
  ) {}
  async create(context: ZLinkActorContext): Promise<Player> {
    const player = new Player(context, this.data, await this.data.load(context.actorId));
    console.log(
      Control.playerCreated + JSON.stringify({ actorId: context.actorId, nodeId: this.config.id })
    );
    return player;
  }
}
@Injectable()
export class PlayerRelocation implements ZLinkActorRelocationAdapter<Player> {
  async capture(actor: Player): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify(actor.gold.value));
  }
  async restore(actor: Player, payload: Uint8Array): Promise<void> {
    actor.gold.restore(JSON.parse(Buffer.from(payload).toString('utf8')));
  }
}
