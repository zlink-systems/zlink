import {
  ZLinkFrameworkRuntimeState,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime,
  type ZLinkRouteClient,
  type ZLinkSpotOutbound
} from '@zlink-systems/framework';
import { ObjectReq, ObjectSpotType, ProfileReq, type ObjectRes, type ProfileRes } from '../../../Shared/messages';
import { ChannelNames } from '../../../Shared/messages';
import type { StoreResponseGate } from '../../../Shared/location-store';
import type { HttpRoute } from '../Support/http-server';

export function createConsumerEndpoints(
  channel: ZLinkRouteClient,
  spotOutbound: ZLinkSpotOutbound,
  locationQuery: ZLinkLocationRuntimeQuery,
  routeRuntime: ZLinkRouteMeshRuntime,
  storeResponseGate: StoreResponseGate | undefined,
  stop: () => void
): readonly HttpRoute[] {
  const routes: HttpRoute[] = [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'consumer' }) },
    { method: 'POST', path: '/profile/request', handle: (body) => requestProfile(channel, toProfileReq(body)) },
    { method: 'POST', path: '/profile/request-once', handle: (body) => requestProfileOnce(channel, toProfileReq(body)) },
    { method: 'POST', path: '/object/request', handle: (body) => requestObject(spotOutbound, body as { spotId: string; operationId: string; payload: string }) },
    { method: 'GET', path: '/location/status', handle: () => locationQuery.getStatus() },
    {
      method: 'GET',
      path: '/location/objects',
      handle: async (_body, requestUrl) => {
        const query = new URL(requestUrl ?? '/', 'http://127.0.0.1').searchParams;
        const page = {
          pageSize: parsePageSize(query.get('pageSize')),
          continuationToken: query.get('continuationToken') ?? undefined
        };
        if (query.get('kind') === 'actor') {
          const result = await locationQuery.listObjectLocations({ objectKind: 'actor' }, page);
          return { items: result.items.map((row) => ({
            actorId: row.globalId,
            actorType: row.stableType,
            meshName: row.meshName,
            ownerNodeRid: String(row.nodeRid),
            objectGeneration: row.objectGeneration.toString(),
            state: row.state
          })), continuationToken: result.continuationToken };
        }
        const result = await locationQuery.listObjectLocations({
          objectKind: 'instance_spot',
          meshName: query.get('meshName') ?? undefined
        }, page);
        return { items: result.items.map((row) => ({
          spotId: row.globalId,
          spotType: row.stableType,
          meshName: row.meshName,
          ownerNodeRid: String(row.nodeRid),
          spotGeneration: row.objectGeneration.toString(),
          state: row.state
        })), continuationToken: result.continuationToken };
      }
    },
    {
      method: 'GET',
      path: '/location/mesh',
      handle: async () => (await locationQuery.listMeshNodeDescriptors(ChannelNames.profile)).items.map((row) => ({
        rid: String(row.rid),
        endpoint: row.endpoint,
        objectRole: row.objectRole,
        state: row.state,
        placementWeight: row.placementWeight,
        populationCapacity: row.populationCapacity,
        activationConcurrency: row.activationConcurrency,
        objectCapabilities: row.objectCapabilities
      }))
    },
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

function parsePageSize(value: string | null): number | undefined {
  if (value === null) return undefined;
  const pageSize = Number(value);
  if (!Number.isInteger(pageSize) || pageSize < 1 || pageSize > 1000) {
    throw new RangeError('pageSize must be an integer in 1..1000.');
  }
  return pageSize;
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

async function requestObject(
  outbound: ZLinkSpotOutbound,
  request: { readonly spotId: string; readonly operationId: string; readonly payload: string }
): Promise<ObjectRes> {
  return await outbound
    .requestToSpot(request.spotId, new ObjectReq(request.spotId, request.operationId, request.payload))
    .instanceSpot(ObjectSpotType)
    .inMesh(ChannelNames.profile)
    .timeout(5000)
    .submit<ObjectRes>();
}
