import type { EvidenceWaitReq } from '../../../Shared/messages';
import {
  MonitoringActorCreateReq,
  MonitoringEvent,
  MonitoringSpotCreateReq,
  RuntimeMonitoringNames
} from '../../../Shared/messages';
import type { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';
import {
  ZLinkFrameworkException,
  ZLinkFrameworkRelocationMode,
  type ZLinkActorManager,
  type ZLinkRouteMeshRuntime,
  type ZLinkRouteMeshRuntimeOptions,
  type ZLinkFrameworkRuntime,
  type ZLinkHostCapacityStatus,
  type ZLinkLocationRuntimeQuery,
  type ZLinkSpotManager,
  type ZLinkSpotPublisherClient
} from '@zlink-systems/framework';
import {
  PublicObserverProbe,
  serializeHostStatus,
  serializeRouteStatus
} from '../Support/public-status-observer';
import {
  MonitoringCapacityGate,
  MonitoringPublishGate,
  MonitoringUserSpot
} from '../Handlers/service-handlers';

export function createServiceEndpoints(
  evidence: EvidenceStore,
  runtimeOptions: ZLinkRouteMeshRuntimeOptions,
  routeRuntime: ZLinkRouteMeshRuntime,
  frameworkRuntime: ZLinkFrameworkRuntime,
  locations: ZLinkLocationRuntimeQuery,
  publisher: ZLinkSpotPublisherClient,
  spots: ZLinkSpotManager,
  actors: ZLinkActorManager,
  publishGate: MonitoringPublishGate,
  capacityGate: MonitoringCapacityGate,
  observerProbe: PublicObserverProbe,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'service', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    { method: 'GET', path: '/status/host', handle: () => serializeHostStatus(frameworkRuntime.status) },
    {
      method: 'GET',
      path: '/status/capacity',
      handle: () => serializeCapacityStatus(frameworkRuntime.status.capacity)
    },
    {
      method: 'POST',
      path: '/admin/capacity-gate/arm',
      handle: () => {
        capacityGate.arm();
        return { status: 'armed' };
      }
    },
    {
      method: 'POST',
      path: '/admin/capacity/reset',
      handle: () => {
        frameworkRuntime.resetCapacityMetrics();
        return serializeCapacityStatus(frameworkRuntime.status.capacity);
      }
    },
    { method: 'GET', path: '/status/route/missing', handle: () => routeRuntime.snapshot('missing-mesh') },
    {
      method: 'GET',
      path: '/status/route/missing/observe',
      handle: () => {
        routeRuntime.observe('missing-mesh');
        return { accepted: true };
      }
    },
    {
      method: 'POST',
      path: '/admin/publish-gate',
      handle: (body) => {
        const request = body as { readonly target?: unknown; readonly blocked?: unknown };
        if (typeof request.target !== 'string' || typeof request.blocked !== 'boolean') {
          throw new TypeError('publish gate requires target and blocked.');
        }
        publishGate.setBlocked(request.target, request.blocked);
        return { target: request.target, blocked: request.blocked };
      }
    },
    {
      method: 'POST',
      path: '/observer/slow/start',
      handle: () => {
        observerProbe.startSlowRouteObserver(routeRuntime, RuntimeMonitoringNames.channel, evidence);
        return { status: 'started' };
      }
    },
    {
      method: 'POST',
      path: '/observer/slow/release',
      handle: () => {
        observerProbe.releaseSlowRouteObserver();
        return { status: 'released' };
      }
    },
    {
      method: 'POST',
      path: '/observer/failing/start',
      handle: () => {
        observerProbe.startFailingRouteObserver(routeRuntime, RuntimeMonitoringNames.channel, evidence);
        return { status: 'started' };
      }
    },
    {
      method: 'POST',
      path: '/admin/publish',
      handle: async (body) => {
        const request = body as { readonly marker?: unknown; readonly blockerBytes?: unknown };
        const marker = request.marker;
        if (typeof marker !== 'string' || marker.length === 0) throw new TypeError('publish marker is required.');
        const blockerBytesValue = request.blockerBytes === undefined
          ? 0
          : request.blockerBytes;
        if (
          typeof blockerBytesValue !== 'number'
          || !Number.isInteger(blockerBytesValue)
          || blockerBytesValue < 0
          || blockerBytesValue > 1_048_576
        ) {
          throw new TypeError('blockerBytes must be an integer between 0 and 1048576.');
        }
        const blockerBytes = blockerBytesValue;
        await publisher
          .publish(
            RuntimeMonitoringNames.spotChannel,
            RuntimeMonitoringNames.spotChannel,
            RuntimeMonitoringNames.publishTopic,
            new MonitoringEvent(marker, blockerBytes === 0 ? undefined : 'x'.repeat(blockerBytes))
          )
          .submit();
        evidence.add(`publish-submitted|rid=${evidence.rid}|marker=${marker}`);
        return { status: 'submitted', marker };
      }
    },
    {
      method: 'POST',
      path: '/admin/capacity-publish',
      handle: async (body) => {
        const marker = (body as { readonly marker?: unknown }).marker;
        if (typeof marker !== 'string' || marker.length === 0) throw new TypeError('publish marker is required.');
        await publisher
          .publish(
            RuntimeMonitoringNames.spotChannel,
            RuntimeMonitoringNames.spotChannel,
            RuntimeMonitoringNames.capacityTopic,
            new MonitoringEvent(marker)
          )
          .submit();
        evidence.add(`capacity-publish-submitted|rid=${evidence.rid}|marker=${marker}`);
        return { status: 'submitted', marker };
      }
    },
    {
      method: 'POST',
      path: '/spot/create',
      handle: async () => {
        try {
          const created = await spots
            .create(MonitoringUserSpot.name)
            .inMesh(RuntimeMonitoringNames.spotChannel)
            .request(new MonitoringSpotCreateReq())
            .submit();
          const result = {
            state: created.state,
            spotId: String(created.spot.spotId),
            objectGeneration: created.spot.objectGeneration.toString(),
            meshName: created.spot.meshName,
            nodeRid: String(created.spot.nodeRid)
          };
          evidence.add(`spot-create|rid=${evidence.rid}|spot=${result.spotId}|state=${result.state}`);
          return result;
        } catch (error) {
          return { state: 'rejected', errorKind: publicErrorKind(error) };
        }
      }
    },
    {
      method: 'POST',
      path: '/spot/close',
      handle: async (body) => {
        const spotId = (body as { readonly spotId?: unknown }).spotId;
        if (typeof spotId !== 'string' || spotId.length === 0) throw new TypeError('spotId is required.');
        const spot = await spots.find(spotId);
        const closed = spot === undefined ? false : await spots.close(spot);
        evidence.add(`spot-close|rid=${evidence.rid}|spot=${spotId}|closed=${closed}`);
        return { spotId, closed };
      }
    },
    {
      method: 'POST',
      path: '/actor/create',
      handle: async (body) => {
        const actorId = (body as { readonly actorId?: unknown }).actorId;
        if (typeof actorId !== 'string' || actorId.length === 0) throw new TypeError('actorId is required.');
        try {
          const created = await actors
            .create(actorId, RuntimeMonitoringNames.actorType)
            .inMesh(RuntimeMonitoringNames.spotChannel)
            .request(new MonitoringActorCreateReq())
            .submit();
          if (created.status === 'rejected') {
            return { state: created.status, reply: created.reply };
          }
          const result = {
            state: created.status,
            actorId: created.actor.actorId,
            objectGeneration: created.actor.objectGeneration.toString(),
            nodeRid: String(created.actor.nodeRid)
          };
          evidence.add(`actor-create|rid=${evidence.rid}|actor=${actorId}|state=${result.state}`);
          return result;
        } catch (error) {
          return { state: 'rejected', errorKind: publicErrorKind(error) };
        }
      }
    },
    {
      method: 'POST',
      path: '/actor/destroy',
      handle: async (body) => {
        const actorId = (body as { readonly actorId?: unknown }).actorId;
        if (typeof actorId !== 'string' || actorId.length === 0) throw new TypeError('actorId is required.');
        const actor = await actors.find(actorId);
        const destroyed = actor === undefined ? false : await actors.destroy(actor);
        evidence.add(`actor-destroy|rid=${evidence.rid}|actor=${actorId}|destroyed=${destroyed}`);
        return { actorId, destroyed };
      }
    },
    {
      method: 'POST',
      path: '/admin/drain',
      handle: async () => {
        const result = await frameworkRuntime.relocate({ mode: ZLinkFrameworkRelocationMode.PlannedMaintenance });
        evidence.add(`admin|rid=${evidence.rid}|action=retire|outcome=${result.outcome}|reason=${result.reason}`);
        return {
          ...result,
          effectiveTargetApplicationVersion: String(result.effectiveTargetApplicationVersion)
        };
      }
    },
    {
      method: 'POST',
      path: '/admin/exclude',
      handle: () => {
        runtimeOptions.channel(RuntimeMonitoringNames.channel).weight = 0;
        evidence.add(`admin|rid=${evidence.rid}|action=exclude|weight=0`);
        return { status: 'excluded', weight: 0 };
      }
    },
    {
      method: 'POST',
      path: '/admin/include',
      handle: () => {
        runtimeOptions.channel(RuntimeMonitoringNames.channel).weight = 100;
        evidence.add(`admin|rid=${evidence.rid}|action=include|weight=100`);
        return { status: 'included', weight: 100 };
      }
    },
    {
      method: 'POST',
      path: '/admin/placement-exclude',
      handle: () => {
        runtimeOptions.mesh(RuntimeMonitoringNames.spotChannel).placementWeight = 0;
        evidence.add(`admin|rid=${evidence.rid}|action=placement-exclude|weight=0`);
        return { status: 'placement-excluded', weight: 0 };
      }
    },
    {
      method: 'POST',
      path: '/admin/placement-include',
      handle: () => {
        runtimeOptions.mesh(RuntimeMonitoringNames.spotChannel).placementWeight = 100;
        evidence.add(`admin|rid=${evidence.rid}|action=placement-include|weight=100`);
        return { status: 'placement-included', weight: 100 };
      }
    },
    {
      method: 'GET',
      path: '/locations/peers',
      handle: async () => (await locations.listTopology({ meshName: RuntimeMonitoringNames.channel })).items
        .map((row) => ({ rid: String(row.nodeRid), endpoint: row.endpoint }))
    },
    {
      method: 'GET',
      path: '/status/route',
      handle: () => serializeRouteStatus(routeRuntime.snapshot(RuntimeMonitoringNames.channel))
    },
    {
      method: 'GET',
      path: '/status/route/spot',
      handle: () => serializeRouteStatus(routeRuntime.snapshot(RuntimeMonitoringNames.spotChannel))
    },
    {
      method: 'GET',
      path: '/admin/weight',
      handle: () => ({
        weight: runtimeOptions.channel(RuntimeMonitoringNames.channel).weight
      })
    },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: (body) => {
        const request = body as EvidenceWaitReq;
        const timeout = Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000));
        return evidence.waitUntil((entries) =>
          request.containsAll.every((expected) => entries.some((entry) => entry.includes(expected)))
          && request.containsAnyGroups.every((group) => group.some((expected) =>
            entries.some((entry) => entry.includes(expected)))), timeout);
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } },
    {
      method: 'POST', path: '/crash', handle: () => {
        setTimeout(() => process.kill(process.pid, 'SIGKILL'), 10);
        return { status: 'crashing', signal: 'SIGKILL' };
      }
    }
  ];
}

function publicErrorKind(error: unknown): string {
  return error instanceof ZLinkFrameworkException
    ? String(error.kind)
    : error instanceof Error
      ? error.name
      : String(error);
}

function serializeCapacityStatus(status: ZLinkHostCapacityStatus): object {
  const core = status.coreHwm;
  const queue = status.applicationJobQueue;
  return {
    measurementEpoch: String(status.measurementEpoch),
    coreHwm: {
      configuredMemoryLimitBytes: core.configuredMemoryLimitBytes === undefined
        ? undefined : String(core.configuredMemoryLimitBytes),
      configuredBudgetBytes: core.configuredBudgetBytes === undefined
        ? undefined : String(core.configuredBudgetBytes),
      configuredProfile: core.configuredProfile,
      effectiveBudgetBytes: String(core.effectiveBudgetBytes),
      totalAppliedHwmBytes: String(core.totalAppliedHwmBytes),
      coreQueueAccountedBytes: String(core.coreQueueAccountedBytes),
      applicationAccountedBytes: String(core.applicationAccountedBytes),
      currentAccountedBytes: String(core.currentAccountedBytes),
      peakAccountedBytes: String(core.peakAccountedBytes),
      completionCurrentAccountedBytes: String(core.completionCurrentAccountedBytes),
      completionPeakAccountedBytes: String(core.completionPeakAccountedBytes),
      completionPendingMessageCount: String(core.completionPendingMessageCount)
    },
    applicationJobQueue: {
      configuredProfile: queue.configuredProfile,
      configuredManualMax: queue.configuredManualMax === undefined
        ? undefined : String(queue.configuredManualMax),
      configuredPauseThresholdPercent: queue.configuredPauseThresholdPercent,
      configuredResumeThresholdPercent: queue.configuredResumeThresholdPercent,
      effectiveProcessorCount: String(queue.effectiveProcessorCount),
      effectiveMaxQueuedApplicationJobs: String(queue.effectiveMaxQueuedApplicationJobs),
      pausePermitCount: String(queue.pausePermitCount),
      resumePermitCount: String(queue.resumePermitCount),
      reservedSupplyPermits: String(queue.reservedSupplyPermits),
      queuedApplicationJobs: String(queue.queuedApplicationJobs),
      permitsInUse: String(queue.permitsInUse),
      peakPermitsInUse: String(queue.peakPermitsInUse),
      capacityWaiters: String(queue.capacityWaiters),
      capacityWaitCount: String(queue.capacityWaitCount),
      capacityWaitDurationSeconds: queue.capacityWaitDurationSeconds,
      pressureState: queue.pressureState,
      currentPauseDurationSeconds: queue.currentPauseDurationSeconds
    }
  };
}
