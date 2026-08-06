import type { ZLinkRouteMeshRuntime, ZLinkRouteMeshStatus } from '@zlink-systems/framework';

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
  role: string
): void {
  let reported = false;
  const report = (status: ZLinkRouteMeshStatus): void => {
    if (reported || !status.isReady) return;
    reported = true;
    console.log(`deliverydispatch-route-ready role=${role} mesh=${meshName}`);
  };
  void (async () => {
    report(runtime.snapshot(meshName));
    for await (const observed of runtime.observe(meshName, 64)) report(observed.status);
  })().catch((error: unknown) => {
    console.error(`deliverydispatch readiness observer failed role=${role} mesh=${meshName}`, error);
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
