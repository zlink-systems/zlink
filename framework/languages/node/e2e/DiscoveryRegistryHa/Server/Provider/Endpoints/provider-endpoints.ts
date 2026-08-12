import {
  ZLinkFrameworkRelocationMode,
  ZLinkFrameworkRelocationOutcome,
  ZLinkFrameworkErrorKind,
  type ZLinkFanoutClient,
  type ZLinkRouteMeshRuntimeOptions,
  type ZLinkFrameworkRuntime,
  type ZLinkLocationRuntimeQuery,
  type ZLinkActorManager,
  type ZLinkActorClient,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import {
  ChannelNames,
  Config6ActorCreateReq,
  Config6UserSpotCreateReq,
  FanoutEvent,
  type CapacityActorCreateReq,
  type CapacitySpotCreateReq,
  type EvidenceWaitReq
} from '../../../Shared/messages';
import {
  Config6ActorType,
  Config6LeaveReq,
  type Config6LeaveRes,
  Config6JoinReq,
  type Config6JoinRes,
  Config6ProbeReq,
  Config6UserSpotType,
  type Config6ProbeRes
} from '../Handlers/capacity-objects';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';
import {
  closeScenarioGate,
  openScenarioGate,
  parseScenarioGate,
  scenarioGateSnapshot
} from '../Infrastructure/scenario-gates';

export function createProviderEndpoints(
  evidence: EvidenceStore,
  runtimeOptions: ZLinkRouteMeshRuntimeOptions,
  frameworkRuntime: ZLinkFrameworkRuntime,
  locationQuery: ZLinkLocationRuntimeQuery,
  actors: ZLinkActorManager,
  actorClient: ZLinkActorClient,
  spots: ZLinkSpotManager,
  fanout: ZLinkFanoutClient,
  stop: () => void
): readonly HttpRoute[] {
  const actorRefs = new Map<string, NonNullable<Awaited<ReturnType<ZLinkActorManager['find']>>>>();
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'provider', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'GET',
      path: '/scenario-gate',
      handle: (_body, requestUrl) => {
        const name = new URL(requestUrl ?? '/', 'http://127.0.0.1').searchParams.get('name');
        return scenarioGateSnapshot(parseScenarioGate(name));
      }
    },
    {
      method: 'POST',
      path: '/scenario-gate/close',
      handle: (body) => closeScenarioGate(parseScenarioGate((body as { name?: unknown }).name))
    },
    {
      method: 'POST',
      path: '/scenario-gate/open',
      handle: (body) => openScenarioGate(parseScenarioGate((body as { name?: unknown }).name))
    },
    { method: 'GET', path: '/location/status', handle: () => locationQuery.getStatus() },
    {
      method: 'POST',
      path: '/placement/weight',
      handle: (body) => {
        const weight = Number((body as { weight: number }).weight);
        if (!Number.isInteger(weight) || weight < 0 || weight > 1000) {
          throw new RangeError('Placement weight must be an integer in 0..1000.');
        }
        runtimeOptions.mesh(ChannelNames.profile).placementWeight = weight;
        return { weight };
      }
    },
    {
      method: 'POST',
      path: '/capacity/actors',
      handle: async (body) => {
        const request = body as CapacityActorCreateReq;
        try {
          const result = await actors.create(request.actorId, Config6ActorType)
            .inMesh(ChannelNames.profile)
            .request(new Config6ActorCreateReq(request.state ?? ''))
            .timeout(5000)
            .submit();
          if (result.status === 'rejected') {
            return { status: result.status, actorId: request.actorId };
          }
          actorRefs.set(`${result.actor.actorId}:${result.actor.objectGeneration}`, result.actor);
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
      path: '/capacity/actors/destroy-exact',
      handle: async (body) => {
        const request = body as { actorId: string; objectGeneration: string };
        const actor = actorRefs.get(`${request.actorId}:${request.objectGeneration}`);
        return { destroyed: actor === undefined ? false : await actors.destroy(actor) };
      }
    },
    {
      method: 'POST',
      path: '/capacity/actors/join',
      handle: async (body) => {
        const request = body as { actorId: string; spotId: string };
        return await actorClient.requestToActor(request.actorId, new Config6JoinReq(request.spotId))
          .timeout(10_000)
          .submit<Config6JoinRes>();
      }
    },
    {
      method: 'POST',
      path: '/capacity/actors/probe',
      handle: async (body) => {
        const request = body as { actorId: string };
        return await actorClient.requestToActor(request.actorId, new Config6ProbeReq())
          .timeout(10_000)
          .submit<Config6ProbeRes>();
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
      path: '/capacity/actors/leave',
      handle: async (body) => {
        const request = body as { actorId: string };
        return await actorClient.requestToActor(request.actorId, new Config6LeaveReq())
          .timeout(10_000)
          .submit<Config6LeaveRes>();
      }
    },
    {
      method: 'POST',
      path: '/capacity/spots',
      handle: async (body) => {
        const request = body as CapacitySpotCreateReq;
        try {
          const result = await spots.getOrCreate(request.spotId, Config6UserSpotType)
            .inMesh(ChannelNames.profile)
            .request(new Config6UserSpotCreateReq(
              request.failFactory === true,
              request.state ?? '',
              request.stateLength,
              request.fillByte
            ))
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
      handle: async (body) => {
        const deadlineMs = (body as { deadlineMs?: number }).deadlineMs ?? 30_000;
        if (!Number.isInteger(deadlineMs) || deadlineMs <= 0 || deadlineMs > 180_000) {
          throw new RangeError('Drain deadline must be an integer in 1..180000.');
        }
        runtimeOptions.mesh(ChannelNames.profile).placementWeight = 0;
        evidence.add(`drain-started|rid=${evidence.rid}|weight=0`);
        const result = await frameworkRuntime.relocate({
          mode: ZLinkFrameworkRelocationMode.PlannedMaintenance,
          deadlineMs
        });
        evidence.add(`retire-finished|rid=${evidence.rid}|outcome=${result.outcome}|reason=${result.reason}`);
        if (result.outcome === ZLinkFrameworkRelocationOutcome.Relocated
          && (body as { stopOnSuccess?: boolean }).stopOnSuccess !== false) stop();
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
