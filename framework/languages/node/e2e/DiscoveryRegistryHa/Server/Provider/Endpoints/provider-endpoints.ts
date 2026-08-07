import {
  ZLinkFrameworkRelocationMode,
  ZLinkFrameworkErrorKind,
  type ZLinkFanoutClient,
  type ZLinkRouteMeshRuntimeOptions,
  type ZLinkFrameworkRuntime,
  type ZLinkLocationRuntimeQuery,
  type ZLinkActorManager,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { ChannelNames, FanoutEvent, type EvidenceWaitReq } from '../../../Shared/messages';
import { Config6ActorType, Config6UserSpotType } from '../Handlers/capacity-objects';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';

export function createProviderEndpoints(
  evidence: EvidenceStore,
  runtimeOptions: ZLinkRouteMeshRuntimeOptions,
  frameworkRuntime: ZLinkFrameworkRuntime,
  locationQuery: ZLinkLocationRuntimeQuery,
  actors: ZLinkActorManager,
  spots: ZLinkSpotManager,
  fanout: ZLinkFanoutClient,
  stop: () => void
): readonly HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'provider', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    { method: 'GET', path: '/location/status', handle: () => locationQuery.getStatus() },
    {
      method: 'POST',
      path: '/capacity/actors',
      handle: async (body) => {
        const request = body as { actorId: string };
        try {
          const result = await actors.create(request.actorId, Config6ActorType)
            .inMesh(ChannelNames.profile)
            .request({})
            .timeout(5000)
            .submit();
          if (result.status === 'rejected') {
            return { status: result.status, actorId: request.actorId };
          }
          return {
            status: result.status,
            actorId: result.actor.actorId,
            objectGeneration: result.actor.objectGeneration.toString()
          };
        } catch (error) {
          return publicFailure(error);
        }
      }
    },
    {
      method: 'POST',
      path: '/capacity/actors/destroy',
      handle: async (body) => {
        const actorId = (body as { actorId: string }).actorId;
        const actor = await actors.find(actorId);
        return { destroyed: actor === undefined ? false : await actors.destroy(actor) };
      }
    },
    {
      method: 'POST',
      path: '/capacity/spots',
      handle: async (body) => {
        const request = body as { spotId: string; failFactory?: boolean };
        try {
          const result = await spots.getOrCreate(request.spotId, Config6UserSpotType)
            .inMesh(ChannelNames.profile)
            .request({ failFactory: request.failFactory === true })
            .timeout(5000)
            .submit();
          return {
            status: String(result.state),
            spotId: String(result.spot.spotId),
            objectGeneration: result.spot.objectGeneration.toString()
          };
        } catch (error) {
          return publicFailure(error);
        }
      }
    },
    {
      method: 'POST',
      path: '/capacity/spots/close',
      handle: async (body) => {
        const spotId = (body as { spotId: string }).spotId;
        const spot = await spots.find(spotId);
        return { closed: spot === undefined ? false : await spots.close(spot) };
      }
    },
    {
      method: 'POST',
      path: '/c4/fanout',
      handle: async (body) => {
        const request = body as { value: string; marker?: string };
        await fanout.publish(ChannelNames.fanout, new FanoutEvent(request.value, request.marker)).submit();
        return { status: 'accepted' };
      }
    },
    { method: 'POST', path: '/evidence/clear', handle: () => { evidence.clear(); return { status: 'cleared' }; } },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: (body) => {
        const request = body as EvidenceWaitReq;
        return evidence.waitUntil(
          (entries) => entries.some((entry) => entry.includes(request.contains)),
          Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000))
        );
      }
    },
    {
      method: 'POST',
      path: '/drain',
      handle: async () => {
        runtimeOptions.channel(ChannelNames.profile).weight = 0;
        evidence.add(`drain-started|rid=${evidence.rid}|weight=0`);
        const result = await frameworkRuntime.relocate({ mode: ZLinkFrameworkRelocationMode.PlannedMaintenance, deadlineMs: 30_000 });
        evidence.add(`retire-finished|rid=${evidence.rid}|outcome=${result.outcome}|reason=${result.reason}`);
        stop();
        return result;
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

function publicFailure(error: unknown): { status: 'error'; errorKind: string; message: string } {
  const kind = typeof error === 'object' && error !== null && 'kind' in error
    ? (error as { kind: unknown }).kind
    : undefined;
  return {
    status: 'error',
    errorKind: typeof kind === 'number' && ZLinkFrameworkErrorKind[kind] !== undefined
      ? ZLinkFrameworkErrorKind[kind]
      : error instanceof Error
        ? error.name
        : 'Unknown',
    message: error instanceof Error ? error.message : String(error)
  };
}
