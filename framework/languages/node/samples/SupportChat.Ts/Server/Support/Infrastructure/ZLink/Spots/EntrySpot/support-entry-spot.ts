import { Injectable } from '@nestjs/common';
import { AgentAvailabilityDirectory } from '../../../../Application/ConversationAssignment/agent-availability-directory';
import { SupportActorDirectory } from '../../Actors/support-actor-directory';
import { SupportUserActor } from '../../Actors/support-user-actor';
import type { SupportUserActorCreateReq } from '../../../../../../Shared/Contracts/messages';
import type {
  ZLinkActorCreateResponse,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpotActorJoinResult
} from '@zlink-systems/framework';

@Injectable()
class SupportEntrySpot implements ZLinkEntrySpot<SupportUserActor> {
  readonly context!: ZLinkEntrySpotContext<SupportUserActor, SupportEntrySpot>;

  constructor(
    private readonly availability: AgentAvailabilityDirectory,
    private readonly directory: SupportActorDirectory
  ) {}

  async onActorJoin(
    _actorId: string,
    _request: ZLinkMessage
  ): Promise<ZLinkSpotActorJoinResult> {
    return { accepted: true };
  }

  async onCreateActor(actor: SupportUserActor, request: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
    const value = request.decode<SupportUserActorCreateReq>(Object as never);
    this.directory.bind(actor.actorId, {
      displayName: value.displayName,
      role: value.role,
      participantId: value.participantId
    });
    return { accepted: true };
  }

  async onJoinedActor(_actor: SupportUserActor): Promise<void> {}
  async onLeaveActor(_actor: SupportUserActor): Promise<void> {}

  async onDisconnectActor(actor: SupportUserActor): Promise<void> {
    const identity = this.directory.get(actor.actorId);
    if (identity?.role === 'Agent' && identity.actorId === identity.participantId) {
      this.availability.setAvailable(identity.actorId, false);
    }
  }
}

export { SupportEntrySpot };
