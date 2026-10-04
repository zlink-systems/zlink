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
  readonly shutdownSignal?: AbortSignal;
}

export interface ZLinkRuntimeStopParts {
  readonly state: ZLinkFrameworkExecutionState;
  readonly cleanupDeadline?: Date;
  readonly shutdownSignal?: AbortSignal;
  readonly locationSnapshot: ZLinkLocationRuntimeStopSnapshot;
  readonly streamRuntime?: ZLinkStreamRuntimeManager;
  readonly spotNodeRuntime?: ZLinkSpotNodeRuntimeManager;
  readonly channelRuntime?: ZLinkChannelRuntimeManager;
  readonly serviceRelocation?: { dispose(): Promise<void> };
  readonly ownedStores?: readonly ZLinkRuntimeOwnedStore[];
}

export async function rollbackRuntimeStart(parts: ZLinkRuntimeStartRollbackParts): Promise<void> {
  const errors: unknown[] = [];
  await runShutdownStep(errors, () => parts.streamRuntime?.dispose());
  await runShutdownStep(errors, () => parts.spotNodeRuntime?.dispose());
  await runShutdownStep(errors, () => parts.channelRuntime?.dispose());
  await runShutdownStep(errors, () => parts.startedLocationRuntime?.stop(parts.shutdownSignal));
  if (errors.length === 0) await runShutdownStep(errors, () => parts.context.dispose());
  await disposeOwnedStores(parts.ownedStores, errors);
  if (errors.length === 1) throw errors[0];
  if (errors.length > 1) throw new AggregateError(errors, 'Framework start rollback failed.');
}

export async function stopRuntimeParts(parts: ZLinkRuntimeStopParts): Promise<void> {
  const state = parts.state;
  const errors: unknown[] = [];
  await runShutdownStep(errors, () => parts.streamRuntime?.dispose());
  await runShutdownStep(errors, () =>
    parts.spotNodeRuntime?.dispose(undefined, parts.cleanupDeadline)
  );
  await runShutdownStep(errors, () => parts.channelRuntime?.dispose());
  await runShutdownStep(errors, () => parts.serviceRelocation?.dispose());
  await runShutdownStep(errors, () => parts.locationSnapshot.lifecycle?.dispose());
  await runShutdownStep(errors, () => parts.locationSnapshot.runtime?.stop(parts.shutdownSignal));
  await Promise.allSettled(state.listenerTasks);
  if (errors.every(isShutdownAbort)) await runShutdownStep(errors, () => state.dispose());
  await disposeOwnedStores(parts.ownedStores, errors);
  const failures = errors.filter((error) => !isShutdownAbort(error));
  for (const failure of failures) {
    state.errorSink.reportRuntimeTaskException('framework shutdown', failure);
  }
  if (failures.length === 1) throw failures[0];
  if (failures.length > 1) {
    throw new AggregateError(failures, 'Framework runtime shutdown failed.');
  }
}

async function disposeOwnedStores(
  stores: readonly ZLinkRuntimeOwnedStore[] | undefined,
  errors: unknown[]
): Promise<void> {
  const disposed = new Set<ZLinkRuntimeOwnedStore>();
  for (const store of stores ?? []) {
    if (disposed.has(store)) continue;
    disposed.add(store);
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
  return error instanceof AggregateError && error.errors.every((nested) => isShutdownAbort(nested));
}
