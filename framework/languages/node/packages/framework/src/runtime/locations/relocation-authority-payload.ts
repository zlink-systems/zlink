import { type RoutingId } from '../../contracts/Common/CoreTypes';
import type {
  ZLinkLocationOwnerToken,
  ZLinkRelocationActorMembership
} from './internal-location-contracts';
import {
  decodeRelocatingActorAuthorityIdentity,
  rewriteActorAuthorityRoute
} from '../actors/actor-authority-publication';
import { rewriteServiceAuthorityRoute } from '../foundation/service-authority-payload-codec';
import { serviceRelocationAuthorityApplicationPayload } from '../foundation/service-relocation-runtime';

export function relocationAuthorityPayloadForTarget(
  payload: Uint8Array,
  target: ZLinkRelocationActorMembership | undefined,
  placement: {
    readonly owner: ZLinkLocationOwnerToken;
    readonly meshName: string;
    readonly nodeRid: string;
    readonly nodeGeneration: bigint;
    readonly objectGeneration: bigint;
  }
): Uint8Array {
  const applicationPayload = serviceRelocationAuthorityApplicationPayload(payload);
  const actor = decodeRelocatingActorAuthorityIdentity(
    applicationPayload,
    placement.objectGeneration
  );
  if (actor !== undefined) {
    return rewriteActorAuthorityRoute(
      applicationPayload,
      {
        actorId: actor.actor.actorId,
        objectGeneration: actor.actor.objectGeneration,
        meshName: placement.meshName,
        nodeRid: placement.nodeRid as RoutingId
      },
      target?.actorSpotId ?? actor.spotId,
      target?.actorSpotGeneration ?? actor.spotGeneration,
      target?.actorSpotKind ?? actor.spotKind,
      placement.nodeGeneration,
      placement.owner
    );
  }
  return (
    rewriteServiceAuthorityRoute(applicationPayload, placement.owner, placement) ??
    applicationPayload
  );
}
