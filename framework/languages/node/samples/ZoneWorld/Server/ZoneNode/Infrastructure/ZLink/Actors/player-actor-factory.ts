import { PlayerActor } from './player-actor';
import type { ZLinkActorContext, ZLinkActorFactory } from '@zlink-systems/framework';

class PlayerActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<PlayerActor> {
    const actor = new PlayerActor(context.actorId);
    Object.defineProperty(actor, 'context', {
      configurable: true,
      enumerable: false,
      value: context
    });
    return actor;
  }
}

export { PlayerActorFactory };
