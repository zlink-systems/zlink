import type {
  ZLinkFrameworkRuntime,
  ZLinkFrameworkRuntimeStatus,
  ZLinkObservedStatus,
  ZLinkRouteMeshRuntime,
  ZLinkRouteMeshStatus
} from '@zlink-systems/framework';
import type { EvidenceStore } from '../Infrastructure/evidence-store';

export interface PublicStatusObserverHandle {
  stop(): Promise<void>;
}

export class PublicObserverProbe implements PublicStatusObserverHandle {
  private readonly controllers = new Set<AbortController>();
  private readonly tasks = new Set<Promise<void>>();
  private slowRelease?: () => void;

  startSlowRouteObserver(
    runtime: ZLinkRouteMeshRuntime,
    meshName: string,
    evidence: EvidenceStore
  ): void {
    if (this.slowRelease !== undefined) return;
    const controller = new AbortController();
    this.controllers.add(controller);
    let release!: () => void;
    const gate = new Promise<void>((resolve) => { release = resolve; });
    this.slowRelease = release;
    const task = (async () => {
      try {
        let first = true;
        for await (const observed of runtime.observe(meshName, 64, controller.signal)) {
          evidence.add(`observer-slow|sequence=${observed.status.sequence.toString()}`);
          if (first) {
            first = false;
            await gate;
          }
        }
      } catch (error) {
        if (!controller.signal.aborted) recordObserverFailure(evidence, 'slow', error);
      }
    })();
    this.tasks.add(task);
    void task.finally(() => this.tasks.delete(task));
  }

  releaseSlowRouteObserver(): void {
    this.slowRelease?.();
    this.slowRelease = undefined;
  }

  startFailingRouteObserver(
    runtime: ZLinkRouteMeshRuntime,
    meshName: string,
    evidence: EvidenceStore
  ): void {
    const controller = new AbortController();
    this.controllers.add(controller);
    const task = (async () => {
      try {
        for await (const observed of runtime.observe(meshName, 64, controller.signal)) {
          evidence.add(`observer-failing|sequence=${observed.status.sequence.toString()}`);
          throw new Error('monitoring observer failure for e2e');
        }
      } catch (error) {
        if (!controller.signal.aborted) {
          evidence.add(`observer-failed|message=${error instanceof Error ? error.message : String(error)}`);
        }
      }
    })();
    this.tasks.add(task);
    void task.finally(() => this.tasks.delete(task));
  }

  async stop(): Promise<void> {
    this.releaseSlowRouteObserver();
    for (const controller of this.controllers) controller.abort();
    await Promise.allSettled([...this.tasks]);
    this.controllers.clear();
  }
}

export function startPublicStatusObservers(
  frameworkRuntime: ZLinkFrameworkRuntime,
  routeRuntime: ZLinkMeshRuntime,
  meshName: string,
  evidence: EvidenceStore
): PublicStatusObserverHandle {
  const controller = new AbortController();
  const tasks = [
    observeHost(frameworkRuntime, evidence, controller.signal),
    observeRoute(routeRuntime, meshName, evidence, controller.signal)
  ];

  return {
    async stop(): Promise<void> {
      controller.abort();
      await Promise.allSettled(tasks);
    }
  };
}

type ZLinkMeshRuntime = ZLinkRouteMeshRuntime;

async function observeHost(
  runtime: ZLinkFrameworkRuntime,
  evidence: EvidenceStore,
  signal: AbortSignal
): Promise<void> {
  recordHostStatus(evidence, runtime.status);
  try {
    for await (const observed of runtime.observe(signal)) {
      recordObservedHostStatus(evidence, observed);
    }
  } catch (error) {
    if (!signal.aborted) recordObserverFailure(evidence, 'host', error);
  }
}

async function observeRoute(
  runtime: ZLinkRouteMeshRuntime,
  meshName: string,
  evidence: EvidenceStore,
  signal: AbortSignal
): Promise<void> {
  recordRouteStatus(evidence, runtime.snapshot(meshName));
  try {
    for await (const observed of runtime.observe(meshName, 64, signal)) {
      recordObservedRouteStatus(evidence, observed);
    }
  } catch (error) {
    if (!signal.aborted) recordObserverFailure(evidence, 'route', error);
  }
}

export function serializeHostStatus(status: ZLinkFrameworkRuntimeStatus): object {
  return {
    state: status.state,
    isReady: status.isReady,
    acceptingWork: status.acceptingWork,
    deadline: status.deadline?.toISOString(),
    relocationResult: status.relocationResult === undefined
      ? undefined
      : {
          mode: status.relocationResult.mode,
          effectiveTargetApplicationVersion: String(status.relocationResult.effectiveTargetApplicationVersion),
          outcome: status.relocationResult.outcome,
          reason: status.relocationResult.reason
        },
    terminationResult: status.terminationResult,
    inboundDispatch: {
      applicationHwmBytes: String(status.inboundDispatch.applicationHwmBytes),
      pendingPayloadBytes: String(status.inboundDispatch.pendingPayloadBytes),
      queuedPayloadBytes: String(status.inboundDispatch.queuedPayloadBytes),
      activePayloadBytes: String(status.inboundDispatch.activePayloadBytes),
      applicationReceivePaused: status.inboundDispatch.applicationReceivePaused,
      pendingCompletionSends: String(status.inboundDispatch.pendingCompletionSends),
      completionSendLimit: String(status.inboundDispatch.completionSendLimit)
    },
    sequence: String(status.sequence),
    observedAt: status.observedAt.toISOString()
  };
}

export function serializeRouteStatus(status: ZLinkRouteMeshStatus): object {
  return {
    meshName: status.meshName,
    state: status.state,
    isReady: status.isReady,
    readyPeerCount: status.readyPeerCount,
    channels: status.channels.map((channel) => ({
      channelName: channel.channelName,
      isReady: channel.isReady,
      readyTargetCount: channel.readyTargetCount
    })),
    peers: status.peers.map((peer) => ({
      nodeRid: String(peer.nodeRid),
      state: peer.state,
      unavailableReason: peer.unavailableReason
    })),
    placement: {
      isAvailable: status.placement.isAvailable,
      activeActorCount: status.placement.activeActorCount,
      activeSpotCount: status.placement.activeSpotCount,
      unavailableReason: status.placement.unavailableReason
    },
    sequence: String(status.sequence),
    observedAt: status.observedAt.toISOString()
  };
}

function recordHostStatus(evidence: EvidenceStore, status: ZLinkFrameworkRuntimeStatus): void {
  evidence.add(`public-status|host=${JSON.stringify(serializeHostStatus(status))}`);
}

function recordRouteStatus(evidence: EvidenceStore, status: ZLinkRouteMeshStatus): void {
  evidence.add(`public-status|route=${JSON.stringify(serializeRouteStatus(status))}`);
}

function recordObservedHostStatus(
  evidence: EvidenceStore,
  observed: ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>
): void {
  evidence.add(`public-observed|host=${JSON.stringify({
    status: serializeHostStatus(observed.status),
    loss: serializeLoss(observed.loss)
  })}`);
}

function recordObservedRouteStatus(
  evidence: EvidenceStore,
  observed: ZLinkObservedStatus<ZLinkRouteMeshStatus>
): void {
  evidence.add(`public-observed|route=${JSON.stringify({
    status: serializeRouteStatus(observed.status),
    loss: serializeLoss(observed.loss)
  })}`);
}

function serializeLoss(loss: { readonly coalescedCount: bigint; readonly discardedTerminalCount: bigint }): object {
  return {
    coalescedCount: String(loss.coalescedCount),
    discardedTerminalCount: String(loss.discardedTerminalCount)
  };
}

function recordObserverFailure(evidence: EvidenceStore, source: string, error: unknown): void {
  const message = error instanceof Error ? error.message : String(error);
  evidence.add(`public-observer-failure|source=${source}|message=${message}`);
}
