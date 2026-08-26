import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';

function waitForShutdown(): Promise<void> {
  return new Promise<void>((resolve) => {
    const keepAlive = setInterval(() => undefined, 1000);
    const stop = () => {
      clearInterval(keepAlive);
      resolve();
    };
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
  });
}

async function waitForRouteMeshReady(
  runtime: ZLinkRouteMeshRuntime,
  meshName: string
): Promise<void> {
  const signal = AbortSignal.timeout(30_000);
  const ready = (): boolean => {
    const status = runtime.snapshot(meshName);
    return status.isReady && status.readyPeerCount > 0;
  };
  if (ready()) return;
  try {
    for await (const observed of runtime.observe(meshName, 64, signal)) {
      if (observed.status.isReady && observed.status.readyPeerCount > 0) return;
    }
  } catch (error: unknown) {
    if (!signal.aborted) throw error;
  }
  throw new Error(`SupportChat RouteMesh '${meshName}' did not become ready before startup deadline.`);
}

export {
  waitForRouteMeshReady,
  waitForShutdown
};
