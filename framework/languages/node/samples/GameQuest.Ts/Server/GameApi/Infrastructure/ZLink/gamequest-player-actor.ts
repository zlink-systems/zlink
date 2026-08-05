import type { ZLinkActor, ZLinkActorContext } from '@zlink-systems/framework';

class GameQuestPlayerActor implements ZLinkActor {
  constructor(
    readonly actorId: string,
    readonly context: ZLinkActorContext
  ) {}

  push(message: unknown): void {
    this.context.boundSession.send(message).submit();
  }
}

export { GameQuestPlayerActor };
