import {
  ZLinkFrameworkRuntimeState,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime,
  type ZLinkRouteClient
} from '@zlink-systems/framework';
import { ProfileReq, type ProfileRes } from '../../../Shared/messages';
import { ChannelNames } from '../../../Shared/messages';
import type { StoreResponseGate } from '../../../Shared/location-store';
import type { HttpRoute } from '../Support/http-server';

export function createConsumerEndpoints(
  channel: ZLinkRouteClient,
  locationQuery: ZLinkLocationRuntimeQuery,
  routeRuntime: ZLinkRouteMeshRuntime,
  storeResponseGate: StoreResponseGate | undefined,
  stop: () => void
): readonly HttpRoute[] {
  const routes: HttpRoute[] = [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'consumer' }) },
    { method: 'POST', path: '/profile/request', handle: (body) => requestProfile(channel, toProfileReq(body)) },
    { method: 'POST', path: '/profile/request-once', handle: (body) => requestProfileOnce(channel, toProfileReq(body)) },
    { method: 'GET', path: '/location/status', handle: () => locationQuery.getStatus() },
    {
      method: 'GET',
      path: '/location/peers',
      handle: async () => {
        const page = await locationQuery.listMeshNodeDescriptors(ChannelNames.profile);
        return page.items
          .filter((row) => row.channelWeights[ChannelNames.profile] !== undefined)
          .map((row) => ({
            endpoint: row.endpoint,
            nodeRid: String(row.rid),
            ownerId: row.ownerId,
            draining: row.state === ZLinkFrameworkRuntimeState.Draining
              || row.state === ZLinkFrameworkRuntimeState.Relocating
              || row.state === ZLinkFrameworkRuntimeState.Relocated
          }));
      }
    },
    {
      method: 'GET',
      path: '/route/status',
      handle: () => routeRuntime.snapshot(ChannelNames.profile)
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
  if (storeResponseGate !== undefined) {
    routes.push(
      { method: 'GET', path: '/location/store-gate', handle: () => storeResponseGate.snapshot() },
      { method: 'POST', path: '/location/store-gate/close', handle: () => { storeResponseGate.close(); return storeResponseGate.snapshot(); } },
      { method: 'POST', path: '/location/store-gate/open', handle: () => { storeResponseGate.open(); return storeResponseGate.snapshot(); } }
    );
  }
  return routes;
}

function toProfileReq(body: unknown): ProfileReq {
  const request = body as ProfileReq;
  return new ProfileReq(request.value, request.marker);
}

async function requestProfile(channel: ZLinkRouteClient, request: ProfileReq): Promise<ProfileRes> {
  return await channel
    .requestToChannel(ChannelNames.profile, request)
    .timeout(5000)
    .submit<ProfileRes>();
}

async function requestProfileOnce(channel: ZLinkRouteClient, request: ProfileReq): Promise<ProfileRes> {
  return await channel
    .requestToChannel(ChannelNames.profile, request)
    .timeout(1000)
    .submit<ProfileRes>();
}
