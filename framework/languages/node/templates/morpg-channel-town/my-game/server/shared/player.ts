import { Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter
} from '@zlink-systems/framework';
import { ActionRejected } from './messages';
import { Rejection } from './constants';
export class Player implements ZLinkActor {
  constructor(readonly context: ZLinkActorContext) {}
  assertIdentity(playerId: string): void {
    if (playerId !== this.context.actorId) throw new Error('Player identity mismatch');
  }
  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status === 'rejected')
      await this.context.boundSession.send(new ActionRejected(Rejection.joinRejected)).submit();
    else if (completion.status !== 'accepted')
      throw new Error(`Actor join failed: ${completion.status}`);
  }
}
@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  async create(context: ZLinkActorContext): Promise<Player> {
    return new Player(context);
  }
}
// Identity, membership and binding are Framework-owned; Player has no application state.
@Injectable()
export class PlayerRelocation implements ZLinkActorRelocationAdapter<Player> {
  async capture(_actor: Player): Promise<Uint8Array> {
    return new Uint8Array();
  }
  async restore(_actor: Player, _payload: Uint8Array): Promise<void> {}
}
