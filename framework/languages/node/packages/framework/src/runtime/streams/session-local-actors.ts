export interface ZLinkSessionLocalActor {
  readonly actorId: string;
}

export class ZLinkSessionLocalActorBindings<TActor extends ZLinkSessionLocalActor> {
  private readonly actors = new Map<string, { actor: TActor; token: string }>();

  snapshot(): readonly TActor[] {
    const snapshot: TActor[] = [];
    for (const entry of this.actors.values()) {
      snapshot.push(entry.actor);
    }
    return snapshot;
  }

  find(actorId: string): TActor | undefined {
    return this.actors.get(actorId)?.actor;
  }

  bind(actor: TActor, token: string): void {
    this.actors.set(actor.actorId, { actor, token });
  }

  unbind(actorId: string, token: string): void {
    const current = this.actors.get(actorId);
    if (current?.token === token) {
      this.actors.delete(actorId);
    }
  }
}
