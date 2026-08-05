import {
  type ZLinkLocationRuntimeQuery,
  ZLinkLocationTopologyState,
} from '@zlink-systems/framework';
import type { LocationProbeOptions } from '../Configuration/location-probe-options';
import type { HttpRoute } from '../Support/http-server';

export function createLocationProbeEndpoints(
  options: LocationProbeOptions,
  locations: ZLinkLocationRuntimeQuery,
  stop: () => void
): readonly HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'location-store-probe', rid: options.rid }) },
    { method: 'GET', path: '/location/status', handle: () => ({ storeHealthy: true, watchEnabled: false }) },
    {
      method: 'GET',
      path: '/location/service-summary',
      handle: async () => {
        const page = await locations.listServiceSummaries({ meshName: 'profile' });
        return page.items.map((row) => ({
          channelName: row.meshName,
          serviceRole: 3,
          totalCount: row.totalCount,
          readyCount: row.readyCount
        }));
      }
    },
    {
      method: 'GET',
      path: '/location/topology',
      handle: async () => {
        const page = await locations.listTopology({ meshName: 'profile' });
        return page.items.map((row) => ({
          channelName: row.meshName,
          serviceRole: 3,
          state: row.state === ZLinkLocationTopologyState.Ready ? 3 : row.state,
          routingId: row.nodeRid,
          endpoint: row.endpoint
        }));
      }
    },
    {
      method: 'GET',
      path: '/location/member-peers',
      handle: async () => {
        const page = await locations.listTopology({ meshName: 'profile' });
        return page.items.map((row) => ({ rid: row.nodeRid, endpoint: row.endpoint, state: row.state }));
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}
