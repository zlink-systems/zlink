import { restoreNotification, type GameNotification } from './messages';
import { Inject, Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from './config';
import { fieldId } from './constants';
import { EnterField, WorldState } from './messages';
export class Player implements ZLinkActor {
  x = 0;
  sequence = 0;
  constructor(
    readonly context: ZLinkActorContext,
    private readonly config: NodeConfig
  ) {}
  apply(position: EnterField): void {
    this.x = position.x;
    this.sequence = position.sequence;
  }
  move(position: EnterField): void {
    this.context.joinSpot(fieldId(position.x), position).defer();
  }
  async sendState(): Promise<void> {
    await this.context.boundSession
      .send(new WorldState(fieldId(this.x), this.config.id, this.x, this.sequence))
      .submit();
  }
  async deliver(
    sourceSpotId: string,
    packetName: string,
    message: GameNotification,
    visible: boolean
  ): Promise<void> {
    if (this.context.spotId !== sourceSpotId || !visible) return;
    await this.context.boundSession.send(restoreNotification(packetName, message)).submit();
  }
  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status !== 'accepted') throw new Error(`Spot join failed: ${completion.status}`);
    if (String(this.context.spotId) === fieldId(this.x)) await this.sendState();
  }
}
@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  constructor(@Inject(NODE_CONFIG) private readonly config: NodeConfig) {}
  async create(context: ZLinkActorContext): Promise<Player> {
    return new Player(context, this.config);
  }
}
// Serialize application state only; Framework owns identity, binding and membership.
@Injectable()
export class PlayerRelocation implements ZLinkActorRelocationAdapter<Player> {
  async capture(actor: Player): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify({ x: actor.x, sequence: actor.sequence }));
  }
  async restore(actor: Player, payload: Uint8Array): Promise<void> {
    actor.apply(JSON.parse(Buffer.from(payload).toString('utf8')) as EnterField);
  }
}
