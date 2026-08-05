import type { SupportRole } from '../../../../../Shared/Contracts/messages';

interface SupportActorIdentity {
  readonly actorId: string;
  readonly displayName: string;
  readonly role: SupportRole;
  readonly participantId: string;
}

class SupportActorDirectory {
  private readonly actors = new Map<string, SupportActorIdentity>();

  bind(actorId: string, identity: Omit<SupportActorIdentity, 'actorId'>): void {
    this.actors.set(actorId, {
      actorId,
      ...identity
    });
  }

  get(actorId: string): SupportActorIdentity | undefined {
    return this.actors.get(actorId);
  }

  remove(actorId: string): void {
    this.actors.delete(actorId);
  }
}

export { SupportActorDirectory };
export type { SupportActorIdentity };
