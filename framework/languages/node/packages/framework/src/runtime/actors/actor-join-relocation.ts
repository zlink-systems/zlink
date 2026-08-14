import type { ZLinkActor } from '../../contracts';
import type { ZLinkBackendActorRef } from '../backend';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import type { ZLinkActorRuntimeState } from './actor-runtime-state';

/** Goal port implemented by the Host relocation aggregate for cross-node Actor Join. */
export interface ZLinkActorJoinRelocation {
  relocateActorJoin(input: {
    readonly meshName: string;
    readonly actor: ZLinkActor;
    readonly state: ZLinkActorRuntimeState;
    readonly target: ZLinkSpotRouteTarget;
    /** Private RelocationId; never the public Join OperationId. */
    readonly relocationId: string;
    readonly completionOperationId?: string;
    readonly signal?: AbortSignal;
  }): Promise<{
    readonly actorRef: ZLinkBackendActorRef;
    readonly membershipEpoch: bigint;
    readonly spotGeneration: bigint;
  }>;
}
