import {
  type ZLinkActorClient,
  type ZLinkActorManager,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntimeOptions,
  type ZLinkSpotManager,
  type ZLinkSpotOutbound
} from '@zlink-systems/framework';
import type {
  CreateSpotReq,
  CreateSpotRes,
  EvidenceWaitReq,
  MultiNodeCreateSpotReq,
  MultiNodeStateRouteReq,
  ScaleOutReadinessReq,
  ScaleOutReadinessRes,
  ScaleOutActorProbeRes,
  SpotOnlyJoinRes,
  SpotOnlyMeshReq,
  SpotOnlyMeshRes
} from '../../../Shared/messages';
import { ScaleOutActorProbeReq, SpotOnlyJoinReq, SpotServiceNames, spotServicePacket } from '../../../Shared/messages';
import type { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';
import { createLocalMultiNodeSpot, requestStateViaSpotOutbound, SpotOnlyUserSpot } from '../Spots/multi-node-spots';

export function createMultiNodeEndpoints(
  evidence: EvidenceStore,
  spots: ZLinkSpotManager,
  outbound: ZLinkSpotOutbound,
  spotRefs: ZLinkSpotManager,
  actors: ZLinkActorManager,
  actorClient: ZLinkActorClient,
  locations: ZLinkLocationRuntimeQuery,
  runtimeOptions: ZLinkRouteMeshRuntimeOptions,
  actorMeshName: string,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'multi-node', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST',
      path: '/placement/weight',
      handle: async (body) => {
        const weight = (body as { weight: number }).weight;
        runtimeOptions.mesh(SpotServiceNames.spotOnlyMesh).placementWeight = weight;
        const deadline = Date.now() + 5000;
        do {
          const descriptors = await locations.listMeshNodeDescriptors(SpotServiceNames.spotOnlyMesh);
          const local = descriptors.items.find((descriptor) => String(descriptor.rid) === evidence.rid);
          if (local?.placementWeight === weight) {
            evidence.add(`placement-weight|rid=${evidence.rid}|weight=${weight}`);
            return { nodeRid: evidence.rid, weight };
          }
          await new Promise((resolve) => setTimeout(resolve, 50));
        } while (Date.now() < deadline);
        throw new Error(`Placement weight ${weight} was not published for '${evidence.rid}'.`);
      }
    },
    {
      method: 'POST',
      path: '/scale-out/readiness/wait',
      handle: (body) => waitForScaleOutReadiness(
        locations, spotRefs, body as ScaleOutReadinessReq
      )
    },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: (body) => {
        const request = body as EvidenceWaitReq;
        const timeout = Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000));
        return evidence.waitUntil((entries) =>
          request.containsAll.every((expected) => entries.some((entry) => entry.includes(expected))), timeout);
      }
    },
    {
      method: 'POST',
      path: '/spot/create-local',
      handle: (body) => {
        const request = body as MultiNodeCreateSpotReq;
        return createLocalMultiNodeSpot(spots, evidence, evidence.rid, request.spotId);
      }
    },
    {
      method: 'POST',
      path: '/spot/create-user-local',
      handle: async (body) => {
        const request = body as CreateSpotReq;
        let created;
        try {
          created = await spots
            .getOrCreate(request.spotId, SpotOnlyUserSpot.name)
            .inMesh(SpotServiceNames.spotOnlyMesh)
            .submit();
        } catch (error) {
          evidence.add(
            `create-user-spot-failed|rid=${evidence.rid}|spot=${request.spotId}`
            + `|error=${error instanceof Error ? error.message : String(error)}`
          );
          throw error;
        }
        evidence.add(`create-user-spot|rid=${evidence.rid}|spot=${created.spot.spotId}|state=${created.state}`);
        return {
          spotId: String(created.spot.spotId),
          nodeRid: String(created.spot.nodeRid),
          objectGeneration: String(created.spot.objectGeneration),
          meshName: created.spot.meshName,
          state: String(created.state)
        } satisfies CreateSpotRes;
      }
    },
    {
      method: 'POST',
      path: '/spot/spot-only/request-send',
      handle: async (body) => {
        const request = body as SpotOnlyMeshReq;
        const source = await spots
          .getOrCreate(request.sourceSpotId, SpotOnlyUserSpot.name)
          .inMesh(SpotServiceNames.spotOnlyMesh)
          .request(request)
          .submit();
        evidence.add(`create-source-spot|requester=${evidence.rid}|owner=${source.spot.nodeRid}|spot=${source.spot.spotId}`);
        const reply = source.reply as { value?: unknown } | undefined;
        if (typeof reply?.value !== 'number') {
          throw new Error(`Source Spot '${request.sourceSpotId}' did not return the target request value.`);
        }
        return {
          sourceSpotId: request.sourceSpotId,
          targetSpotId: request.targetSpotId,
          targetValue: reply.value,
          marker: request.marker
        } satisfies SpotOnlyMeshRes;
      }
    },
    {
      method: 'POST',
      path: '/actor/spot-only-join',
      handle: async (body) => {
        const bodyRequest = body as SpotOnlyJoinReq;
        const request = new SpotOnlyJoinReq(
          bodyRequest.targetSpotId,
          bodyRequest.actorId,
          bodyRequest.marker
        );
        const actor = await actors
          .getOrCreate(request.actorId, SpotServiceNames.actorType)
          .inMesh(actorMeshName)
          .request({ displayName: `spot-only-${request.actorId}` })
          .submit();
        if (actor.status === 'rejected') {
          throw new Error(`Actor '${request.actorId}' creation was rejected.`);
        }
        const result = await actorClient
          .requestToActor(actor.actor.actorId, request)
          .timeout(10000)
          .submit<SpotOnlyJoinRes>();
        await evidence.waitUntil((entries) =>
          entries.some((entry) =>
            entry.includes(`spot-only-actor-join|rid=${evidence.rid}|actor=${request.actorId}|target=${request.targetSpotId}`)
            && entry.includes(`|marker=${request.marker}`)), 10000);
        return result;
      }
    },
    {
      method: 'POST',
      path: '/actor/scale-out-probe',
      handle: async (body) => {
        const request = body as ScaleOutActorProbeReq;
        const actor = await actors
          .getOrCreate(request.actorId, SpotServiceNames.actorType)
          .inMesh(actorMeshName)
          .request({ displayName: `scale-out-${request.actorId}` })
          .submit();
        if (actor.status === 'rejected') {
          throw new Error(`Actor '${request.actorId}' creation was rejected.`);
        }
        return await actorClient
          .requestToActor(actor.actor.actorId, spotServicePacket(ScaleOutActorProbeReq, request))
          .timeout(10_000)
          .submit<ScaleOutActorProbeRes>();
      }
    },
    {
      method: 'POST',
      path: '/spot/state/request',
      handle: (body) => {
        const request = body as MultiNodeStateRouteReq;
        return requestStateViaSpotOutbound(
          outbound,
          spotRefs,
          actorMeshName,
          request.spotId,
          request.delta
        );
      }
    },
    {
      method: 'POST',
      path: '/shutdown',
      handle: () => {
        stop();
        return { status: 'stopping' };
      }
    },
    {
      method: 'POST',
      path: '/crash',
      handle: () => {
        setTimeout(() => process.exit(1), 10);
        return { status: 'crashing' };
      }
    }
  ];
}

async function waitForScaleOutReadiness(
  locations: ZLinkLocationRuntimeQuery,
  spotRefs: ZLinkSpotManager,
  request: ScaleOutReadinessReq
): Promise<ScaleOutReadinessRes> {
  const deadline = Date.now() + Math.max(1, Math.min(request.timeoutMilliseconds ?? 30_000, 30_000));
  do {
    const rows = await locations.listMeshNodeDescriptors(SpotServiceNames.spotOnlyMesh);
    const peer = rows.items.find((row) => String(row.rid) === request.nodeRid);
    const capabilities = peer?.objectCapabilities.map((capability) =>
      `${capability.objectKind}:${capability.stableType}`) ?? [];
    const entrySpotReady = peer?.entrySpotId !== undefined
      && await spotRefs.find(peer.entrySpotId) !== undefined;
    if (peer !== undefined
      && capabilities.includes(`actor:${SpotServiceNames.actorType}`)
      && entrySpotReady) {
      return {
        nodeRid: request.nodeRid,
        peerReady: true,
        entrySpotReady: true,
        capabilities
      };
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  } while (Date.now() < deadline);
  throw new Error(`SpotNode '${request.nodeRid}' did not publish peer, actor capability, and Entry Spot readiness.`);
}
