import { Injectable } from '@nestjs/common';
import type { ZLinkActor, ZLinkActorContext, ZLinkActorFactory, ZLinkActorJoinCompletion } from '@zlink-systems/framework';
import { zoneId } from '../shared/constants';
import { ActionRejected, type EnterZone, WorldState } from '../shared/messages';
export class Player implements ZLinkActor {
  constructor(readonly context: ZLinkActorContext) {}
  move(position: EnterZone): void {
    this.context.joinSpot(zoneId(position.position.x), position).defer();
  }
  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    if (completion.status === 'accepted' && completion.reply !== undefined)
      await this.context.boundSession.send(completion.reply.decode(WorldState)).submit();
    else if (completion.status === 'rejected' && completion.reply !== undefined)
      await this.context.boundSession.send(completion.reply.decode(ActionRejected)).submit();
    else if (completion.status !== 'accepted') throw new Error(`Zone join failed: ${completion.status}`);
  }
}
@Injectable()
export class PlayerFactory implements ZLinkActorFactory<Player> {
  async create(context: ZLinkActorContext): Promise<Player> {
    return new Player(context);
  }
}
