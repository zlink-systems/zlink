import type { ZLinkLocationRuntimeQuery, ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import type { HttpRoute } from './http-server';

/**
 * Exposes the public aggregate topology query to the runner readiness probe.
 * The runner must not decode opaque Location Store records to infer readiness.
 */
export function createLocationTopologyRoute(
  locations: ZLinkLocationRuntimeQuery,
  routeMeshRuntime: ZLinkRouteMeshRuntime
): HttpRoute {
  return {
    method: 'GET',
    path: '/location/topology',
    handle: async () => {
      const topology = await locations.listTopology({ meshName: 'to-actor' });
      const route = routeMeshRuntime.snapshot('to-actor');
      return {
        ...topology,
        route: {
          isReady: route.isReady,
          readyPeerCount: route.readyPeerCount,
          peers: route.peers.map((peer) => ({
            nodeRid: String(peer.nodeRid),
            state: peer.state,
            unavailableReason: peer.unavailableReason
          }))
        }
      };
    }
  };
}
