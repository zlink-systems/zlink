import type { ZLinkActor, ZLinkActorJoinOperationId } from '../../contracts';
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
    readonly completionOperationId?: ZLinkActorJoinOperationId;
    readonly canonicalRecovery?: {
      readonly handoffId: string;
      /** Core command-28 request identity retained by the target admission. */
      readonly admissionOperationId: ZLinkActorJoinOperationId;
      readonly requestContentType: string;
      readonly request: Buffer;
      readonly replyContentType?: string;
      readonly reply: Buffer;
      readonly actorNodeGeneration: bigint;
      readonly expectedOwnerLeaseGeneration: bigint;
      readonly targetNodeGeneration: bigint;
      readonly targetSpotGeneration: bigint;
      readonly targetAuthorityOwnerGeneration: bigint;
      readonly targetSpotAuthorityOwnerGeneration: bigint;
    };
    /** Target's advertised relocation state chunk cap from the accepted admission reply. */
    readonly advertisedReceiveChunkLimitBytes?: number;
    readonly signal?: AbortSignal;
  }): Promise<{
    readonly actorRef: ZLinkBackendActorRef;
    readonly membershipEpoch: bigint;
    readonly spotGeneration: bigint;
  }>;
}
