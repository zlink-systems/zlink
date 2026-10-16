import { Inject, Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { zoneId } from '../shared/constants';
import { EnterZone, WorldState } from '../shared/messages';
export class Player implements ZLinkActor {
  x = 0;
  sequence = 0;
  constructor(
    readonly context: ZLinkActorContext,
    private readonly config: NodeConfig
  ) {}
  apply(position: EnterZone): void {
    this.x = position.x;
    this.sequence = position.sequence;
  }
  move(position: EnterZone): void {
    this.context.joinSpot(zoneId(position.x), position).defer();
  }
  async sendState(): Promise<void> {
    await this.context.boundSession
      .send(new WorldState(zoneId(this.x), this.config.id, this.x, this.sequence))
      .submit();
  }
  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status !== 'accepted') throw new Error(`Zone join failed: ${completion.status}`);
    await this.sendState();
  }
}
@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  constructor(@Inject(NODE_CONFIG) private readonly config: NodeConfig) {}
  async create(context: ZLinkActorContext): Promise<Player> {
    return new Player(context, this.config);
  }
}
@Injectable()
export class PlayerRelocation implements ZLinkActorRelocationAdapter<Player> {
  async capture(actor: Player): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify({ x: actor.x, sequence: actor.sequence }));
  }
  async restore(actor: Player, payload: Uint8Array): Promise<void> {
    actor.apply(JSON.parse(Buffer.from(payload).toString('utf8')) as EnterZone);
  }
}
