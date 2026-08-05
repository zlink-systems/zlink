import type { ActorId, RoutingId } from './CoreTypes';

export interface ActorRef {
  readonly actorId: ActorId;
  readonly objectGeneration: bigint;
  readonly meshName: string;
  readonly nodeRid: RoutingId;
}
