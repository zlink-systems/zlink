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
    return (
      status.isReady &&
      status.readyPeerCount > 0 &&
      (!requiresPlacement || status.placement.isAvailable)
    );
  };
  if (ready()) return;
  try {
    for await (const observed of runtime.observe(meshName, 64, signal)) {
      if (
        observed.status.isReady &&
        observed.status.readyPeerCount > 0 &&
        (!requiresPlacement || observed.status.placement.isAvailable)
      )
        return;
    }
  } catch (error: unknown) {
    if (!signal.aborted) throw error;
  }
  throw new Error(
    `TicTacToe RouteMesh '${meshName}' did not become ready before startup deadline.`
  );
}

async function logSpotPeerReady(
  runtime: ZLinkRouteMeshRuntime,
  host: ZLinkFrameworkRuntime,
  meshName: string,
  nodeId: string,
  peerNodeId: string
): Promise<void> {
  const peerRoutingIdPrefix = `tictactoe-play-${peerNodeId}-`;
  const isReady = (): boolean =>
    host.status.isReady &&
    runtime
      .snapshot(meshName)
      .peers.some(
        (peer) =>
          peer.state === ZLinkPeerState.Ready &&
          String(peer.nodeRid).startsWith(peerRoutingIdPrefix)
      );
  const signal = AbortSignal.timeout(30_000);
  try {
    // User Spot placement requires this host's published descriptor.
    if (!host.status.isReady) {
      for await (const observed of host.observe(signal)) {
        if (observed.status.isReady) break;
      }
    }
    signal.throwIfAborted();
    if (isReady()) {
      process.stdout.write(`tictactoe-ready kind=peer-route node=${nodeId} peer=${peerNodeId}\n`);
      return;
    }
    for await (const _event of runtime.observe(meshName, 64, signal)) {
      if (!isReady()) continue;
      process.stdout.write(`tictactoe-ready kind=peer-route node=${nodeId} peer=${peerNodeId}\n`);
      return;
    }
  } catch (error: unknown) {
    if (!signal.aborted) throw error;
  }
  process.stderr.write('Timed out waiting for a ready Play RouteMesh peer.\n');
}

export { closeNestRuntime, logSpotPeerReady, waitForRouteMeshReady, waitForShutdown };
import {
  ZLinkPeerState,
  type ZLinkFrameworkRuntime,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
