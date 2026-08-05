import type { SpotId } from '../Common';
import type { ZLinkBoundSession } from '../Streams';
import type { ZLinkActorJoinEntrySpotCall, ZLinkActorJoinSpotCall } from './ZLinkActorFactory';

export interface ZLinkActorContext {
  readonly actorId: string;
  readonly objectGeneration: bigint;
  readonly meshName: string;
  readonly spotId?: SpotId;
  readonly boundSession: ZLinkBoundSession;
  joinSpot(spotId: SpotId): ZLinkActorJoinSpotCall;
  joinSpot(spotId: SpotId, request: unknown): ZLinkActorJoinSpotCall;
  joinEntrySpot(): ZLinkActorJoinEntrySpotCall;
  joinEntrySpot(request: unknown): ZLinkActorJoinEntrySpotCall;
}
