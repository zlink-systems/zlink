import { ZLinkPeerState, type ZLinkRouteMeshRuntime, type ZLinkRouteMeshStatus } from '@zlink-systems/framework';

function waitForShutdown(): Promise<void> {
  return new Promise((resolve) => {
    const keepAlive = setInterval(() => undefined, 60_000);
    const stop = (): void => {
      clearInterval(keepAlive);
      resolve();
    };
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
  });
}

function observeDeliveryRouteReadiness(
  runtime: ZLinkRouteMeshRuntime,
  meshName: string,
  nodeId: string,
  actorRouteTargets: readonly string[] = []
): void {
  let routeReported = false;
  const reportedActorRoutes = new Set<string>();
  const report = (status: ZLinkRouteMeshStatus): void => {
    if (status.isReady && !routeReported) {
      routeReported = true;
      console.log(`deliverydispatch-ready kind=route node=${nodeId}`);
    }
    for (const target of actorRouteTargets) {
      if (reportedActorRoutes.has(target)) continue;
      if (!status.peers.some((peer) => peer.nodeRid === target && peer.state === ZLinkPeerState.Ready)) continue;
      reportedActorRoutes.add(target);
      console.log(`deliverydispatch-ready kind=actor-route node=${nodeId} target=${target}`);
    }
  };
  void (async () => {
    report(runtime.snapshot(meshName));
    for await (const observed of runtime.observe(meshName, 64)) report(observed.status);
  })().catch((error: unknown) => {
    console.error(`deliverydispatch readiness observer failed node=${nodeId} mesh=${meshName}`, error);
  });
}

async function closeNestRuntime(container: { close(): Promise<void> }): Promise<void> {
  try {
    await container.close();
  } catch (error) {
    const candidate = error as { name?: string; code?: number };
    if (candidate.name === 'CloseError' && [0, 401, 403, 404].includes(candidate.code ?? -1)) return;
    throw error;
  }
}

export { closeNestRuntime, observeDeliveryRouteReadiness, waitForShutdown };
