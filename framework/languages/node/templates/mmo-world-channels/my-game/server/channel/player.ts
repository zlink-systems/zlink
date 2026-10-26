import { Inject, Injectable } from '@nestjs/common';
import { deserialize, serialize } from 'node:v8';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { zoneId, Control } from '../shared/constants';
import { PlayerData } from './data';
import { GoldBalance, type GoldSnapshot } from './domain';
import { PlayerView, restoreNotification, type Notification, ActionRejected } from '../shared/messages';
export class Player implements ZLinkActor {
  async deliver(packetName: string, payload: Notification, visible = true): Promise<void> {
    const message = restoreNotification(packetName, payload);
    const sourceSpotId = zoneId(
      message instanceof PlayerView ? message.channel : message.channelId
    );
    if (sourceSpotId !== null && (String(this.context.spotId) !== sourceSpotId || !visible)) return;
    await this.context.boundSession.send(message).submit();
  }
  readonly gold: GoldBalance;
  constructor(
    readonly context: ZLinkActorContext,
    private readonly data: PlayerData,
    gold: bigint
  ) {
    this.gold = new GoldBalance(gold);
  }
  async earn(): Promise<ActionRejected | undefined> {
    const decision = this.gold.previewEarn();
    if (!decision.accepted) return decision.rejection;
    await this.data.save(this.context.actorId, decision.gold);
    this.gold.commitEarn(decision);
    return undefined;
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
    return serialize(actor.gold.capture());
  }
  async restore(actor: Player, payload: Uint8Array): Promise<void> {
    actor.gold.restore(deserialize(payload) as GoldSnapshot);
  }
}
