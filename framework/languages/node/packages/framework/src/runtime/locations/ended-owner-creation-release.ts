import {
  actorRelocationAuthorityApplicationPayload,
  decodeActorAuthorityPayload
} from '../actors/actor-authority-payload-codec';
import { decodeServiceReadySpotAuthority } from '../foundation/service-authority-payload-codec';
import type {
  ZLinkAuthoritySnapshot,
  ZLinkEndedOwnerReleaseIntent,
  ZLinkMeshNodeDescriptor
} from './internal-location-contracts';

export function endedOwnerCreationState(
  snapshot: Pick<ZLinkAuthoritySnapshot, 'allocation' | 'payload'>,
  creation: ZLinkEndedOwnerReleaseIntent
) {
  if (
    snapshot.allocation.objectKind !== creation.key.kind ||
    snapshot.allocation.stableType !== creation.intent.stableType
  )
    return undefined;

  if (creation.key.kind === 'actor') {
    const payload = actorRelocationAuthorityApplicationPayload(snapshot.payload);
    const actor = payload === undefined ? undefined : decodeActorAuthorityPayload(payload);
    if (
      actor?.state === 'ready' &&
      actor.stableType === creation.intent.stableType &&
      actor.actorId === creation.key.globalId
    )
      return { kind: 'ready' as const };
    return undefined;
  }

  const spot = decodeServiceReadySpotAuthority(snapshot.payload);
  if (
    spot?.kind === creation.key.kind &&
    spot.stableType === creation.intent.stableType &&
    spot.spotId === creation.key.globalId &&
    spot.activationRecovery === undefined
  )
    return { kind: 'ready' as const, spot };
  return undefined;
}

/** Location runtime §6.1. Both repository backends check the selected host run's descriptor. */
export function allowsEndedReadyRelease(
  creation: ZLinkEndedOwnerReleaseIntent,
  descriptor: ZLinkMeshNodeDescriptor | undefined
): boolean {
  const target = creation.target;
  return (
    descriptor !== undefined &&
    descriptor.lifecycleGeneration === target.nodeLifecycleGeneration &&
    descriptor.ownerId === target.owner.ownerId &&
    descriptor.leaseGeneration === target.owner.leaseGeneration &&
    descriptor.objectCapabilities.some(
      (capability) =>
        capability.objectKind === creation.key.kind &&
        capability.stableType === creation.intent.stableType &&
        capability.policy === 'disabled'
    )
  );
}
