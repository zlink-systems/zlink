import type { ZLinkBackendContext } from '../backend';
import type { ZLinkChannelRuntimeManager } from '../channels';
import type { ZLinkFrameworkExecutionState } from '../execution';
import type { ZLinkLocationRuntime } from '../locations';
import type { ZLinkSpotNodeRuntimeManager } from '../spots';
import type { ZLinkStreamRuntimeManager } from '../streams';
import type { ZLinkLocationRuntimeStopSnapshot } from './location-runtime-owner';
import { isAbortError } from '../abort';

interface ZLinkRuntimeOwnedStore {
  dispose?(): void | Promise<void>;
}

export interface ZLinkRuntimeStartRollbackParts {
  readonly context: ZLinkBackendContext;
  readonly startedLocationRuntime?: ZLinkLocationRuntime;
  readonly streamRuntime?: ZLinkStreamRuntimeManager;
  readonly spotNodeRuntime?: ZLinkSpotNodeRuntimeManager;
  readonly channelRuntime?: ZLinkChannelRuntimeManager;
  readonly ownedStores?: readonly ZLinkRuntimeOwnedStore[];
}

export interface ZLinkRuntimeStopParts {
  readonly state: ZLinkFrameworkExecutionState;
  readonly locationSnapshot: ZLinkLocationRuntimeStopSnapshot;
  readonly streamRuntime?: ZLinkStreamRuntimeManager;
  readonly spotNodeRuntime?: ZLinkSpotNodeRuntimeManager;
  readonly channelRuntime?: ZLinkChannelRuntimeManager;
  readonly serviceRelocation?: { dispose(): Promise<void> };
  readonly ownedStores?: readonly ZLinkRuntimeOwnedStore[];
}

export async function rollbackRuntimeStart(parts: ZLinkRuntimeStartRollbackParts): Promise<void> {
  await Promise.allSettled([
    parts.streamRuntime?.dispose(),
    parts.spotNodeRuntime?.dispose(),
    parts.channelRuntime?.dispose()
  ]);
  await parts.startedLocationRuntime?.stop().catch(() => undefined);
  await parts.context.dispose().catch(() => undefined);
  await disposeOwnedStores(parts.ownedStores);
}

export async function stopRuntimeParts(parts: ZLinkRuntimeStopParts): Promise<void> {
  const state = parts.state;
  state.abortController.abort();
  const errors: unknown[] = [];
  await runShutdownStep(errors, () => parts.streamRuntime?.dispose());
  await runShutdownStep(errors, () => parts.spotNodeRuntime?.dispose());
  await runShutdownStep(errors, () => parts.channelRuntime?.dispose());
  await runShutdownStep(errors, () => parts.serviceRelocation?.dispose());
  await runShutdownStep(errors, () => parts.locationSnapshot.lifecycle?.dispose());
  await runShutdownStep(errors, () => parts.locationSnapshot.runtime?.stop());
  await Promise.allSettled(state.listenerTasks);
  await runShutdownStep(errors, () => state.dispose());
  await disposeOwnedStores(parts.ownedStores, errors);
  const failures = errors.filter((error) => !isShutdownAbort(error));
  if (failures.length === 1) throw failures[0];
  if (failures.length > 1) {
    throw new AggregateError(failures, 'Framework runtime shutdown failed.');
  }
}

async function disposeOwnedStores(
  stores: readonly ZLinkRuntimeOwnedStore[] | undefined,
  errors?: unknown[]
): Promise<void> {
  const disposed = new Set<ZLinkRuntimeOwnedStore>();
  for (const store of stores ?? []) {
    if (disposed.has(store)) continue;
    disposed.add(store);
    if (errors === undefined) {
      try {
        await store.dispose?.();
      } catch {
        // Rollback is already handling the original start failure. Store
        // cleanup remains best effort in this path.
      }
      continue;
    }
    await runShutdownStep(errors, () => store.dispose?.());
  }
}

async function runShutdownStep(
  errors: unknown[],
  step: () => Promise<unknown> | unknown
): Promise<void> {
  try {
    await step();
  } catch (error) {
    errors.push(error);
  }
}

function isShutdownAbort(error: unknown): boolean {
  if (isAbortError(error)) return true;
  return error instanceof AggregateError
    && error.errors.every((nested) => isShutdownAbort(nested));
}
