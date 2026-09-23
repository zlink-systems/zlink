export interface ZLinkSessionLocalActor {
  readonly actorId: string;
}

export class ZLinkSessionLocalActorBindings<TActor extends ZLinkSessionLocalActor> {
  private readonly actors = new Map<string, { actor: TActor; token: string; actorSlot?: number }>();
  private readonly actorIdsBySlot = new Map<number, string>();

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

  findBySlot(actorSlot: number): TActor | undefined {
    const actorId = this.actorIdsBySlot.get(actorSlot);
    return actorId === undefined ? undefined : this.actors.get(actorId)?.actor;
  }

  bind(actor: TActor, token: string, actorSlot?: number): void {
    const previous = this.actors.get(actor.actorId);
    if (previous?.actorSlot !== undefined && previous.actorSlot !== actorSlot) {
      this.actorIdsBySlot.delete(previous.actorSlot);
    }
    this.actors.set(actor.actorId, { actor, token, actorSlot });
    if (actorSlot !== undefined) {
      this.actorIdsBySlot.set(actorSlot, actor.actorId);
    }
  }

  unbind(actorId: string, token: string): void {
    const current = this.actors.get(actorId);
    if (current?.token === token) {
      this.actors.delete(actorId);
      if (current.actorSlot !== undefined) {
        this.actorIdsBySlot.delete(current.actorSlot);
      }
    }
  }
}
