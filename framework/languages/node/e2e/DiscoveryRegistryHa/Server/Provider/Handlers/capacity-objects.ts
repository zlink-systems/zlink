import { Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotContext
} from '@zlink-systems/framework';

export const Config6ActorType = 'Config6Actor';
export const Config6UserSpotType = 'Config6UserSpot';

export class Config6Actor implements ZLinkActor {
  readonly context!: ZLinkActorContext;

  constructor(context: ZLinkActorContext) {
    Object.defineProperty(this, 'context', { value: context });
  }
}

@Injectable()
export class Config6ActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<Config6Actor> {
    if (String(context.actorId).includes('factory-fail')) {
      throw new Error('injected Config 6 Actor factory failure');
    }
    return new Config6Actor(context);
  }
}

@Injectable()
export class Config6EntrySpot implements ZLinkEntrySpot<Config6Actor> {
  readonly context!: ZLinkEntrySpotContext<Config6Actor>;

  async onCreateActor(_actor: Config6Actor, _request: ZLinkMessage): Promise<{ accepted: boolean }> {
    return { accepted: true };
  }

  async onJoinedActor(_actor: Config6Actor): Promise<void> {}
  async onLeaveActor(_actor: Config6Actor): Promise<void> {}
  async onDisconnectActor(_actor: Config6Actor): Promise<void> {}
}

@Injectable()
export class Config6UserSpot implements ZLinkSpot<Config6Actor> {
  readonly context!: ZLinkSpotContext<Config6Actor>;

  async onCreate(request: ZLinkMessage): Promise<{ accepted: boolean }> {
    const value = request.decode<{ readonly failFactory?: boolean }>(Object as never);
    if (value.failFactory === true) {
      throw new Error('injected Config 6 User Spot factory failure');
    }
    return { accepted: true };
  }

  async onActorJoin(_actorId: string, _request: ZLinkMessage): Promise<{ accepted: boolean }> {
    return { accepted: true };
  }

  async onJoinedActor(_actor: Config6Actor): Promise<void> {}
  async onLeaveActor(_actor: Config6Actor): Promise<void> {}
  async onDisconnectActor(_actor: Config6Actor): Promise<void> {}
}
