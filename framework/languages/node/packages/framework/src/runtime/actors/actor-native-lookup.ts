import type {
  ZLinkBackendActorRef,
  ZLinkBackendMeshNode,
  ZLinkBackendSpotNode
} from '../backend/contracts';

type ZLinkActorLookupNode = ZLinkBackendMeshNode | ZLinkBackendSpotNode;

export function lookupNativeActorRef(
  node: ZLinkActorLookupNode,
  actorId: string
): ZLinkBackendActorRef | undefined {
  try {
    const location = node.actorLookup(actorId);
    if (location === undefined) {
      return undefined;
    }
    const actor = 'actor' in location ? location.actor : location;
    return {
      nodeRid: actor.nodeRid as ZLinkBackendActorRef['nodeRid'],
      actorId: actor.actorId,
      generation: actor.generation
    };
  } catch (error) {
    if (nativeErrno(error) === 2) {
      return undefined;
    }
    throw error;
  }
}

function nativeErrno(error: unknown): number | undefined {
  if (typeof error !== 'object' || error === null || !('nativeErrno' in error)) {
    return undefined;
  }
  const value = error.nativeErrno;
  return typeof value === 'number' ? value : undefined;
}
