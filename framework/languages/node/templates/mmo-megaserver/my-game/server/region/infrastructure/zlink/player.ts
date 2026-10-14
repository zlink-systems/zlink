import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorClient,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter
} from '@zlink-systems/framework';
import { RegionFull } from '../../../shared/messages';
export class Player implements ZLinkActor {
  constructor(
    readonly context: ZLinkActorContext,
    private readonly actors: ZLinkActorClient
  ) {}
  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status === 'rejected') {
      if (!completion.reply) throw new Error('Region rejection omitted application reply');
      // Selection remains in Entry after a rejected deferred join.
      await this.actors
        .sendToActor(this.context.actorId, completion.reply.decode(RegionFull))
        .submit();
    } else if (completion.status === 'failed')
      throw new Error(`Region join failed: ${completion.kind}`);
  }
}
@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}
  async create(context: ZLinkActorContext): Promise<Player> {
    return new Player(context, this.actors);
  }
}
// Identity, membership and binding are Framework-owned.
@Injectable()
export class PlayerRelocation implements ZLinkActorRelocationAdapter<Player> {
  async capture(_actor: Player): Promise<Uint8Array> {
    return new Uint8Array();
  }
  async restore(_actor: Player, _payload: Uint8Array): Promise<void> {}
}
