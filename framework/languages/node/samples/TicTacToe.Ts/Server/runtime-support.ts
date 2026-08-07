async function closeNestRuntime(container: { close(): Promise<void> }): Promise<void> {
  try {
    await container.close();
  } catch (error) {
    const candidate = error as { name?: string; code?: number };
    if (candidate.name === 'CloseError' && (candidate.code === 0 || candidate.code === 401)) {
      return;
    }
    throw error;
  }
}

function waitForShutdown(): Promise<void> {
  return new Promise<void>((resolve) => {
    process.once('SIGINT', resolve);
    process.once('SIGTERM', resolve);
  });
}

async function waitForRouteMeshReady(
  runtime: ZLinkRouteMeshRuntime,
  meshName: string,
  requiresPlacement = false
): Promise<void> {
  const signal = AbortSignal.timeout(30_000);
  const ready = (): boolean => {
    const status = runtime.snapshot(meshName);
    return status.isReady && (!requiresPlacement || status.placement.isAvailable);
  };
  if (ready()) return;
  try {
    for await (const observed of runtime.observe(meshName, 64, signal)) {
      if (observed.status.isReady
        && (!requiresPlacement || observed.status.placement.isAvailable)) return;
    }
  } catch (error: unknown) {
    if (!signal.aborted) throw error;
  }
  throw new Error(`TicTacToe RouteMesh '${meshName}' did not become ready before startup deadline.`);
}

export { closeNestRuntime, waitForRouteMeshReady, waitForShutdown };
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
