import { zlinkEntrySpotActorRequestHandler } from '@zlink-systems/nestjs';
import { observeMilestoneRes, PacketNames } from '../../../../../../../Shared/Contracts/messages';
import type {
  ZLinkEntrySpotActorRequestHandler,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import type {
  ObserveMilestoneReq,
  ObserveMilestoneRes,
} from '../../../../../../../Shared/Contracts/messages';
import { PlayActor } from '../../../Actors/play-actor';
import { MilestoneObserverRegistry } from '../entry-spot-registries';
import { PlayEntrySpot } from '../play-entry-spot';

@zlinkEntrySpotActorRequestHandler({
  actor: () => PlayActor,
  entrySpot: () => PlayEntrySpot,
  packetName: PacketNames.observeMilestoneReq
})
class PlayActorObserveMilestoneHandler
  implements ZLinkEntrySpotActorRequestHandler<
    PlayEntrySpot,
    PlayActor,
    ObserveMilestoneReq,
    ObserveMilestoneRes
  > {
  constructor(private readonly observers: MilestoneObserverRegistry) {}

  async handle(
    _spot: PlayEntrySpot,
    actor: PlayActor,
    context: ZLinkMessageContext,
    _request: ObserveMilestoneReq
  ): Promise<ObserveMilestoneRes> {
    void context;
    this.observers.subscribe(actor.actorId);
    return observeMilestoneRes(true);
  }
}

export { PlayActorObserveMilestoneHandler };
