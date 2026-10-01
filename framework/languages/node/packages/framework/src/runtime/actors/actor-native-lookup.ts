import { ConfigError, ConfigResult } from '@zlink-systems/zlink';
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
    if (error instanceof ConfigError && error.result === ConfigResult.NotFound) {
      return undefined;
    }
    throw error;
  }
}
