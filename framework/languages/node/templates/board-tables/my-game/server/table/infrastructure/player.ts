import { Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter
} from '@zlink-systems/framework';
import { Reasons } from '../../shared/constants';
import { ActionRejected } from '../../shared/messages';
export class Player implements ZLinkActor {
  constructor(readonly context: ZLinkActorContext) {}
  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status === 'failed') throw new Error(`Actor join failed: ${completion.kind}`);
    if (completion.status === 'rejected')
      await this.context.boundSession.send(new ActionRejected(Reasons.full)).submit();
  }
}
@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  async create(context: ZLinkActorContext): Promise<Player> {
    return new Player(context);
  }
}
@Injectable()
export class PlayerRelocation implements ZLinkActorRelocationAdapter<Player> {
  // Player owns no mutable application state. Framework transfers identity and binding.
  async capture(_actor: Player): Promise<Uint8Array> {
    return new Uint8Array();
  }
  async restore(_actor: Player, _payload: Uint8Array): Promise<void> {}
}
