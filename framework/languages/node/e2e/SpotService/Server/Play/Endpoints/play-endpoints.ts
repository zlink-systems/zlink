import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkRouteClient,
  type ZLinkSpotManager,
  type ZLinkSpotOutbound
} from '@zlink-systems/framework';
import type {
  ChannelEchoRes,
  ChannelRouteRes,
  ChannelRouteReq,
  CloseSpotExactReq,
  CloseSpotExactRes,
  CloseSpotReq,
  CrossRoleActorPushRes,
  NodeRouteReq,
  NodeRouteRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotIdleCloseReq,
  SpotMissingMsgReq,
  SpotMissingHandlerReq,
  SpotMissingTargetMsgReq,
  SpotMissingTargetReq,
  SpotMixedRouteRes,
  SpotMixedRouteReq,
  SpotStageProbeReq,
  SpotStageTimerReq,
  SpotOverrunStartReq,
  SpotOutboundRouteReq,
  SpotPublishReq,
  SpotSlowRouteReq,
  SpotStateMsgReq,
  SpotStateRouteReq,
  SpotTimerStartReq,
  SpotToSpotNegativeRes,
  SpotToSpotNegativeRouteReq,
  SpotToSpotRes,
  SpotToSpotRouteReq,
  SpotToSpotTimeoutRes,
  SpotToSpotTimeoutRouteReq,
  SpotTypeMismatchReq,
  SpotWorkerCompleteReq,
  SpotWorkerStartReq,
  StateRes
} from '../../../Shared/messages';
import {
  ChannelEchoReq,
  CrossRoleActorPushReq,
  MissingSpotMsg,
  MissingSpotReq,
  SlowSpotReq,
  SpotAdminReq,
  SpotOutboundMsg,
  SpotOutboundNegativeMsg,
  SpotServiceNames,
  SpotToSpotNegativeReq,
  SpotToSpotReq,
  SpotToSpotTimeoutReq,
  StageProbeReq,
  StageTimerStartMsg,
  StateMsg,
  StateReq,
  spotServicePacket
} from '../../../Shared/messages';
import type { EvidenceStore } from '../Infrastructure/evidence-store';
import { ScenarioAlternateSpot, ScenarioUserSpot } from '../Spots/scenario-spots';
import type { HttpRoute } from '../Support/http-server';

export function createPlayEndpoints(
  evidence: EvidenceStore,
  spotManager: ZLinkSpotManager,
  spotOutbound: ZLinkSpotOutbound,
  spotRefs: ZLinkSpotManager,
  routeClient: ZLinkRouteClient,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'play', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST',
      path: '/crash',
      handle: () => {
        setTimeout(() => process.exit(1), 10);
        return { status: 'crashing' };
      }
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
      path: '/spot/create',
      handle: async (body) => {
        const request = body as CreateSpotReq;
        const created = await spotManager
          .getOrCreate(request.spotId, ScenarioUserSpot.name)
          .inMesh(SpotServiceNames.spotChannel)
          .submit();
        const state = typeof created.state === 'string' ? created.state : String(created.state);
        evidence.add(`create-spot|rid=${evidence.rid}|spot=${created.spot.spotId}|state=${state}`);
        return {
          spotId: String(created.spot.spotId),
          nodeRid: String(created.spot.nodeRid),
          objectGeneration: created.spot.objectGeneration.toString(),
          meshName: created.spot.meshName,
          state
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/close-exact',
      handle: async (body) => {
        const request = body as CloseSpotExactReq;
        try {
          const closed = await spotManager.close({
            spotId: request.spotId,
            objectGeneration: BigInt(request.objectGeneration),
            meshName: request.meshName,
            nodeRid: request.nodeRid
          });
          return {
            spotId: request.spotId,
            closed,
            staleGeneration: false
          } satisfies CloseSpotExactRes;
        } catch (error) {
          if (
            error instanceof ZLinkFrameworkException
            && error.kind === ZLinkFrameworkErrorKind.SpotGenerationStale
          ) {
            evidence.add(`close-spot-stale|rid=${evidence.rid}|spot=${request.spotId}`);
            return {
              spotId: request.spotId,
              closed: false,
              staleGeneration: true,
              errorKind: error.kind
            } satisfies CloseSpotExactRes;
          }
          throw error;
        }
      }
    },
    {
      method: 'POST',
      path: '/spot/close',
      handle: async (body) => {
        const request = body as CloseSpotReq;
        const spot = await spotManager.find(request.spotId);
        const closed = spot === undefined ? false : await spotManager.close(spot);
        evidence.add(`close-spot|rid=${evidence.rid}|spot=${request.spotId}|closed=${closed}`);
        if (closed) {
          await evidence.waitUntil((entries) =>
            entries.some((entry) => entry.includes(`spot-closing|rid=${evidence.rid}|spot=${request.spotId}`)), 10000);
        }
        return { spotId: request.spotId, closed };
      }
    },
    {
      method: 'POST',
      path: '/spot/type-mismatch',
      handle: async (body) => {
        const request = body as SpotTypeMismatchReq;
        const first = await spotManager
          .getOrCreate(request.spotId, ScenarioUserSpot.name)
          .inMesh(SpotServiceNames.spotChannel)
          .submit();
        try {
          await spotManager
            .getOrCreate(request.spotId, ScenarioAlternateSpot.name)
            .inMesh(SpotServiceNames.spotChannel)
            .submit();
        } catch (error) {
          if (error instanceof ZLinkFrameworkException && error.kind === ZLinkFrameworkErrorKind.SpotTypeMismatch) {
            evidence.add(`spot-type-mismatch|rid=${evidence.rid}|spot=${request.spotId}|kind=SpotTypeMismatch`);
            const state = typeof first.state === 'string' ? first.state : String(first.state);
            return {
              spotId: request.spotId,
              failed: true,
              errorKind: 'SpotTypeMismatch',
              state
            };
          }
          throw error;
        }
        throw new Error('Expected SpotTypeMismatch for reused spot rid.');
      }
    },
    {
      method: 'POST',
      path: '/spot/create-alternate',
      handle: async (body) => {
        const request = body as CreateSpotReq;
        const created = await spotManager
          .getOrCreate(request.spotId, ScenarioAlternateSpot.name)
          .inMesh(SpotServiceNames.spotChannel)
          .submit();
        const state = typeof created.state === 'string' ? created.state : String(created.state);
        evidence.add(`create-alternate-spot|rid=${evidence.rid}|spot=${created.spot.spotId}|state=${state}`);
        return { spotId: String(created.spot.spotId), nodeRid: String(created.spot.nodeRid), state };
      }
    },
    {
      method: 'POST',
      path: '/spot/state/request',
      handle: (body) => requestSpotState(spotOutbound, spotRefs, body as SpotStateRouteReq)
    },
    {
      method: 'POST',
      path: '/spot/stage/request',
      handle: async (body) => {
        const request = body as SpotStageProbeReq;
        const spot = await requireSpotRef(spotRefs, request.spotId);
        return await spotOutbound
          .requestToSpot(spot.spotId, spotServicePacket(StageProbeReq,
            { marker: request.marker, delta: request.delta }))
          .timeout(5000)
          .submit<StateRes>();
      }
    },
    {
      method: 'POST',
      path: '/spot/stage/timer',
      handle: async (body) => {
        const request = body as SpotStageTimerReq;
        const before = evidence.snapshot();
        const spot = await requireSpotRef(spotRefs, request.spotId);
        await spotOutbound
          .sendToSpot(spot.spotId, spotServicePacket(StageTimerStartMsg,
            { name: request.name, periodMs: request.periodMs }))
          .submit();
        const marker = `stage-timer|rid=${evidence.rid}|spot=${request.spotId}|name=${request.name}`;
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, marker) >= 1, 10000);
        return {
          spotId: request.spotId,
          name: request.name,
          started: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/state/command',
      handle: async (body) => {
        const request = body as SpotStateMsgReq;
        const before = evidence.snapshot();
        const spot = await requireSpotRef(spotRefs, request.spotId);
        await spotOutbound
          .sendToSpot(spot.spotId, spotServicePacket(StateMsg, { marker: request.marker }))
          .submit();
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, `spot-state-command|rid=${evidence.rid}|spot=${request.spotId}|marker=${request.marker}`) >= 1,
          10000);
        return {
          spotId: request.spotId,
          marker: request.marker,
          accepted: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/slow/request',
      handle: async (body) => {
        const request = body as SpotSlowRouteReq;
        const timedOut = await fails(async () => {
          const spot = await requireSpotRef(spotRefs, request.spotId);
          await spotOutbound
            .requestToSpot(spot.spotId, spotServicePacket(SlowSpotReq,
              { marker: request.marker, delayMs: request.delayMs }))
            .timeout(request.timeoutMs)
            .submit();
        });
        return {
          spotId: request.spotId,
          marker: request.marker,
          timedOut
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/outbound',
      handle: async (body) => {
        const request = body as SpotOutboundRouteReq;
        const before = evidence.snapshot();
        const spot = await requireSpotRef(spotRefs, request.spotId);
        await spotOutbound
          .sendToSpot(spot.spotId, spotServicePacket(SpotOutboundMsg, { marker: request.marker }))
          .submit();
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, `spot-outbound|rid=${evidence.rid}|spot=${request.spotId}|echo=echo-sm-c2|notify=notify-sm-c2`) >= 1
          && countNew(entries, before, `spot-msg|rid=${evidence.rid}|spot=${request.spotId}|marker=sm-c2-publish`) >= 1
          && countNew(entries, before, 'channel-echo|value=sm-c2') >= 1
          && countNew(entries, before, 'channel-notify|marker=notify-sm-c2') >= 1,
          10000);
        return {
          spotId: request.spotId,
          marker: request.marker,
          accepted: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/outbound-negative',
      handle: async (body) => {
        const request = body as SpotOutboundRouteReq;
        const before = evidence.snapshot();
        const spot = await requireSpotRef(spotRefs, request.spotId);
        await spotOutbound
          .sendToSpot(spot.spotId, spotServicePacket(SpotOutboundNegativeMsg, { marker: request.marker }))
          .submit();
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, `spot-outbound-negative|rid=${evidence.rid}|spot=${request.spotId}|requestFailed=True`) >= 1
          && countNew(entries, before, 'dispatch-error|surface=channel|kind=request|reason=no_handler|action=reply_error|packet=MissingChannelReq') >= 1
          && countNew(entries, before, 'dispatch-error|surface=channel|kind=send|reason=no_handler|action=drop|packet=MissingChannelNotify') >= 1,
          10000);
        return {
          spotId: request.spotId,
          marker: request.marker,
          accepted: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/channel/route/request',
      handle: async (body) => {
        const request = body as ChannelRouteReq;
        const channel = await routeClient
          .requestToChannel(
            SpotServiceNames.externalSpotChannel,
            spotServicePacket(ChannelEchoReq, { value: request.value })
          )
          .timeout(5000)
          .submit<ChannelEchoRes>();
        return { value: channel.value } satisfies ChannelRouteRes;
      }
    },
    {
      method: 'POST',
      path: '/actor/cross-role/push',
      handle: async (body) => {
        const request = body as CrossRoleActorPushReq;
        const reply = await routeClient
          .requestToNode(SpotServiceNames.externalSpotChannel, request.nodeRid,
            spotServicePacket(CrossRoleActorPushReq, request))
          .timeout(5000)
          .submit<CrossRoleActorPushRes>();
        evidence.add(
          `cross-role-entry|rid=${evidence.rid}|target=${request.nodeRid}|actor=${request.actorId}|value=${request.value}`
        );
        return reply;
      }
    },
    {
      method: 'POST',
      path: '/node/route/request',
      handle: async (body) => {
        const request = body as NodeRouteReq;
        const reply = await routeClient
          .requestToNode(
            SpotServiceNames.externalSpotChannel,
            request.nodeRid,
            spotServicePacket(ChannelEchoReq, { value: request.value })
          )
          .timeout(5000)
          .submit<ChannelEchoRes>();
        return { value: reply.value } satisfies NodeRouteRes;
      }
    },
    {
      method: 'POST',
      path: '/spot/mixed-route/request',
      handle: async (body) => {
        const request = body as SpotMixedRouteReq;
        const channel = await routeClient
          .requestToChannel(
            SpotServiceNames.externalSpotChannel,
            spotServicePacket(ChannelEchoReq, { value: request.channelValue })
          )
          .timeout(5000)
          .submit<ChannelEchoRes>();
        const node = request.nodeRid !== undefined && request.nodeValue !== undefined
          ? await routeClient
              .requestToNode(
                SpotServiceNames.externalSpotChannel,
                request.nodeRid,
                spotServicePacket(ChannelEchoReq, { value: request.nodeValue })
              )
              .timeout(5000)
              .submit<ChannelEchoRes>()
          : undefined;
        const spot = await requireSpotRef(spotRefs, request.spotId);
        const state = await spotOutbound
          .requestToSpot(spot.spotId, spotServicePacket(StateReq, { operation: 'add', delta: request.delta }))
          .timeout(5000)
          .submit<StateRes>();
        return {
          spotId: request.spotId,
          channelReply: channel.value,
          nodeReply: node?.value,
          spotValue: state.value
        } satisfies SpotMixedRouteRes;
      }
    },
    {
      method: 'POST',
      path: '/spot/to-spot/request',
      handle: async (body) => {
        const request = body as SpotToSpotRouteReq;
        const sourceSpot = await requireSpotRef(spotRefs, request.sourceSpotId);
        await requireSpotRef(spotRefs, request.targetSpotId);
        return await spotOutbound
          .requestToSpot(sourceSpot.spotId, spotServicePacket(SpotToSpotReq, {
            targetSpotId: request.targetSpotId,
            marker: request.marker
          }))
          .timeout(5000)
          .submit<SpotToSpotRes>();
      }
    },
    {
      method: 'POST',
      path: '/spot/to-spot/timeout',
      handle: async (body) => {
        const request = body as SpotToSpotTimeoutRouteReq;
        const sourceSpot = await requireSpotRef(spotRefs, request.sourceSpotId);
        await requireSpotRef(spotRefs, request.targetSpotId);
        return spotOutbound
          .requestToSpot(sourceSpot.spotId, spotServicePacket(SpotToSpotTimeoutReq, {
            targetSpotId: request.targetSpotId,
            marker: request.marker
          }))
          .timeout(5000)
          .submit<SpotToSpotTimeoutRes>();
      }
    },
    {
      method: 'POST',
      path: '/spot/to-spot/negative',
      handle: async (body) => {
        const request = body as SpotToSpotNegativeRouteReq;
        const sourceSpot = await requireSpotRef(spotRefs, request.sourceSpotId);
        await requireSpotRef(spotRefs, request.targetSpotId);
        return spotOutbound
          .requestToSpot(sourceSpot.spotId, spotServicePacket(SpotToSpotNegativeReq, {
            targetSpotId: request.targetSpotId,
            marker: request.marker
          }))
          .timeout(5000)
          .submit<SpotToSpotNegativeRes>();
      }
    },
    {
      method: 'POST',
      path: '/spot/publish/wait',
      handle: async (body) => {
        const request = body as SpotPublishReq;
        const snapshot = await evidence.waitUntil((entries) =>
          entries.some((entry) =>
            entry.includes(`spot-msg|rid=${evidence.rid}|spot=${request.spotId}|marker=${request.marker}`)),
          30000);
        return {
          operation: 'spot.sm-c4-observe',
          spotId: request.spotId,
          marker: request.marker,
          received: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/publish/local',
      handle: async (body) => {
        const request = body as SpotPublishReq;
        await submitSpotAdmin(spotOutbound, spotRefs, request.spotId, new SpotAdminReq('publish', request.marker));
        return {
          operation: 'spot.sm-c5-publish',
          publisherRid: evidence.rid,
          spotId: request.spotId,
          marker: request.marker,
          evidence: evidence.snapshot()
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/missing-handler/request',
      handle: async (body) => {
        const request = body as SpotMissingHandlerReq;
        const before = evidence.snapshot();
        const failed = await fails(async () => {
          const spot = await requireSpotRef(spotRefs, request.spotId);
          await spotOutbound
            .requestToSpot(spot.spotId, spotServicePacket(MissingSpotReq, { operation: 'noop', delta: 0 }))
            .timeout(2000)
            .submit<StateRes>();
        });
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, 'dispatch-error|surface=spot|kind=request|reason=no_handler|action=fail_caller|packet=MissingSpotReq') >= 1,
          10000);
        return {
          spotId: request.spotId,
          failed,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/missing-handler/command',
      handle: async (body) => {
        const request = body as SpotMissingMsgReq;
        const before = evidence.snapshot();
        const abort = new AbortController();
        const timeout = setTimeout(() => abort.abort(), 2000);
        try {
          const spot = await requireSpotRef(spotRefs, request.spotId);
          await spotOutbound
            .sendToSpot(spot.spotId, spotServicePacket(MissingSpotMsg, { marker: request.marker }))
            .submit();
        } catch (error) {
          if (!abort.signal.aborted) {
            throw error;
          }
        } finally {
          clearTimeout(timeout);
        }
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, 'dispatch-error|surface=spot|kind=send|reason=no_handler|action=drop|packet=MissingSpotMsg') >= 1,
          10000);
        return {
          spotId: request.spotId,
          marker: request.marker,
          sent: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/missing-target/request',
      handle: async (body) => {
        const request = body as SpotMissingTargetReq;
        let failed = false;
        let errorKind: string | undefined;
        try {
          await spotOutbound
            .requestToSpot(request.spotId, spotServicePacket(StateReq, { operation: 'noop', delta: 0 }))
            .timeout(2000)
            .submit<StateRes>();
        } catch (error) {
          failed = true;
          if (error instanceof ZLinkFrameworkException) errorKind = error.kind;
        }
        return {
          spotId: request.spotId,
          failed,
          errorKind,
          evidence: evidence.snapshot()
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/missing-target/command',
      handle: async (body) => {
        const request = body as SpotMissingTargetMsgReq;
        let failed = false;
        let errorKind: string | undefined;
        try {
          await spotOutbound
            .sendToSpot(request.spotId, spotServicePacket(StateMsg, { marker: request.marker }))
            .submit();
        } catch (error) {
          failed = true;
          if (error instanceof ZLinkFrameworkException) errorKind = error.kind;
        }
        return {
          spotId: request.spotId,
          marker: request.marker,
          sent: !failed,
          failed,
          errorKind,
          evidence: evidence.snapshot()
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/state/local',
      handle: async (body) => {
        const request = body as SpotStateRouteReq;
        return requestSpotState(spotOutbound, spotRefs, request);
      }
    },
    {
      method: 'POST',
      path: '/spot/worker/start',
      handle: (body) => {
        const request = body as SpotWorkerStartReq;
        evidence.add(`worker-start|rid=${evidence.rid}|spot=${request.spotId}|marker=${request.marker}`);
        void submitSpotAdmin(spotOutbound, spotRefs, request.spotId,
          new SpotAdminReq('worker', request.marker, undefined, undefined, request.delayMs))
          .catch((error: unknown) => evidence.add(
            `worker-error|rid=${evidence.rid}|spot=${request.spotId}|marker=${request.marker}|error=${String(error)}`
          ));
        return {
          spotId: request.spotId,
          nodeRid: evidence.rid,
          marker: request.marker
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/worker/complete',
      handle: async (body) => {
        const request = body as SpotWorkerCompleteReq;
        const marker = `worker-complete|rid=${evidence.rid}|spot=${request.spotId}|marker=${request.marker}`;
        const snapshot = await evidence.waitUntil((entries) =>
          entries.some((entry) => entry.includes(marker)), 30000);
        return {
          spotId: request.spotId,
          marker: request.marker,
          completed: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/idle-close/start',
      handle: async (body) => {
        const request = body as SpotIdleCloseReq;
        const before = evidence.snapshot();
        await submitSpotAdmin(spotOutbound, spotRefs, request.spotId,
          new SpotAdminReq('idleTimer', undefined, request.name, request.periodMs));
        const idleMarker = `timer-idle-close|rid=${evidence.rid}|spot=${request.spotId}|name=${request.name}|closed=True`;
        const closingMarker = `spot-closing|rid=${evidence.rid}|spot=${request.spotId}`;
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, idleMarker) === 1
          && countNew(entries, before, closingMarker) === 1,
          30000);
        return {
          spotId: request.spotId,
          name: request.name,
          closed: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/timer/start',
      handle: async (body) => {
        const request = body as SpotTimerStartReq;
        const before = evidence.snapshot();
        await submitSpotAdmin(spotOutbound, spotRefs, request.spotId,
          new SpotAdminReq('timer', undefined, request.name, request.periodMs));
        const marker = `timer-basic|rid=${evidence.rid}|spot=${request.spotId}|name=${request.name}`;
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, marker) >= 2, 30000);
        return {
          spotId: request.spotId,
          name: request.name,
          started: true,
          evidence: snapshot
        };
      }
    },
    {
      method: 'POST',
      path: '/spot/overrun/start',
      handle: async (body) => {
        const request = body as SpotOverrunStartReq;
        const before = evidence.snapshot();
        await submitSpotAdmin(spotOutbound, spotRefs, request.spotId,
          new SpotAdminReq('overrunTimer', undefined, request.name, request.periodMs, undefined,
            request.policy as SpotAdminReq['policy']));
        const marker = `timer-overrun|rid=${evidence.rid}|spot=${request.spotId}|name=${request.name}`;
        const snapshot = await evidence.waitUntil((entries) =>
          countNew(entries, before, marker) >= 3, 30000);
        return {
          spotId: request.spotId,
          name: request.name,
          policy: request.policy,
          started: true,
          evidence: snapshot
        };
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

function countNew(entries: readonly string[], before: readonly string[], marker: string): number {
  return entries.slice(before.length).filter((entry) => entry.includes(marker)).length;
}

async function requestSpotState(
  spotOutbound: ZLinkSpotOutbound,
  spotRefs: ZLinkSpotManager,
  request: SpotStateRouteReq
): Promise<StateRes> {
  const spot = await requireSpotRef(spotRefs, request.spotId);
  return await spotOutbound
    .requestToSpot(spot.spotId, spotServicePacket(StateReq,
      { operation: request.operation, delta: request.delta }))
    .timeout(5000)
    .submit<StateRes>();
}

async function submitSpotAdmin(
  spotOutbound: ZLinkSpotOutbound,
  spotRefs: ZLinkSpotManager,
  spotId: string,
  request: SpotAdminReq
): Promise<unknown> {
  const spot = await requireSpotRef(spotRefs, spotId);
  return spotOutbound.requestToSpot(spot.spotId, request).timeout(30000).submit();
}

async function requireSpotRef(spotRefs: ZLinkSpotManager, spotId: string) {
  const spot = await spotRefs.find(spotId);
  if (spot === undefined) {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.SpotRouteNotFound,
      `SPOT '${spotId}' has no live location row.`
    );
  }
  return spot;
}

async function fails(operation: () => Promise<void>): Promise<boolean> {
  try {
    await operation();
    return false;
  } catch {
    return true;
  }
}
