import { type EvidenceWaitReq } from '../../../Shared/messages';
import type {
  ZLinkFanoutRuntime,
  ZLinkFanoutStatus,
  ZLinkLocationRuntimeQuery
} from '@zlink-systems/framework';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';
import { FanoutStatusObserverProbe } from '../Support/fanout-status-observer';

export function createSubscriberEndpoints(
  evidence: EvidenceStore,
  fanoutRuntime: ZLinkFanoutRuntime | undefined,
  stop: () => void,
  observerProbe?: FanoutStatusObserverProbe,
  locationRuntimeQuery?: ZLinkLocationRuntimeQuery,
  channelName = 'events'
): readonly HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'subscriber', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'GET',
      path: '/location/status',
      handle: async () => locationRuntimeQuery === undefined
        ? { status: 'unavailable' as const }
        : await locationRuntimeQuery.getStatus()
    },
    {
      method: 'GET',
      path: '/status/fanout',
      handle: () => {
        if (fanoutRuntime === undefined || fanoutRuntime === null) {
          return { status: 'unavailable' as const };
        }
        try {
          return toFanoutStatusEvidence(fanoutRuntime.snapshot(channelName));
        } catch (error) {
          return {
            status: 'error' as const,
            error: error instanceof Error ? error.message : String(error)
          };
        }
      }
    },
    {
      method: 'POST',
      path: '/observer/fanout/slow/start',
      handle: () => {
        observerProbe?.startSlow();
        return { status: observerProbe === undefined ? 'unavailable' : 'started' };
      }
    },
    {
      method: 'POST',
      path: '/observer/fanout/normal/start',
      handle: () => {
        observerProbe?.startNormal();
        return { status: observerProbe === undefined ? 'unavailable' : 'started' };
      }
    },
    {
      method: 'POST',
      path: '/observer/fanout/slow/release',
      handle: () => {
        observerProbe?.releaseSlow();
        return { status: observerProbe === undefined ? 'unavailable' : 'released' };
      }
    },
    {
      method: 'POST',
      path: '/observer/fanout/slow/cancel',
      handle: async () => {
        await observerProbe?.cancelSlow();
        return { status: observerProbe === undefined ? 'unavailable' : 'cancelled' };
      }
    },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: async (body) => {
        const request = body as EvidenceWaitReq;
        const timeout = clamp(request.timeoutMilliseconds ?? 10_000, 1, 30_000);
        return await evidence.waitUntil((entries) => matches(entries.slice(request.afterIndex ?? 0), request), timeout);
      }
    },
    { method: 'POST', path: '/evidence/clear', handle: () => { evidence.clear(); return { status: 'cleared' }; } },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

function toFanoutStatusEvidence(status: ZLinkFanoutStatus): Record<string, unknown> {
  return {
    channelName: status.channelName,
    state: status.state,
    isReady: status.isReady,
    readyPublisherCount: status.readyPublisherCount,
    publishers: status.publishers.map((publisher) => ({
      nodeRid: String(publisher.nodeRid),
      state: publisher.state,
      ...(publisher.unavailableReason === undefined
        ? {}
        : { unavailableReason: publisher.unavailableReason })
    })),
    sequence: status.sequence.toString(),
    observedAt: status.observedAt.toISOString()
  };
}

function matches(entries: readonly string[], request: EvidenceWaitReq): boolean {
  const containsAll = request.containsAll ?? [];
  const containsAnyGroups = request.containsAnyGroups ?? [];
  const containsAllLineGroups = request.containsAllLineGroups ?? [];
  const containsAnyLineGroups = request.containsAnyLineGroups ?? [];
  return containsAll.every((expected) => entries.some((entry) => entry.includes(expected)))
    && containsAnyGroups.every((group) => group.some((expected) => entries.some((entry) => entry.includes(expected))))
    && containsAllLineGroups.every((group) => entries.some((entry) => group.every((expected) => entry.includes(expected))))
    && (containsAnyLineGroups.length === 0
      || containsAnyLineGroups.some((group) => entries.some((entry) => group.every((expected) => entry.includes(expected)))));
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}
