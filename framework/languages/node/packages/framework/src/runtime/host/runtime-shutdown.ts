import { traceShutdownStep } from '../diagnostics';
import type { ZLinkBackendContext } from '../backend';
import type { ZLinkChannelRuntimeManager } from '../channels';
import type { ZLinkFrameworkExecutionState } from '../execution';
import type { ZLinkLocationRuntime } from '../locations';
import type { ZLinkSpotNodeRuntimeManager } from '../spots';
import type { ZLinkStreamRuntimeManager } from '../streams';
import type { ZLinkLocationRuntimeStopSnapshot } from './location-runtime-owner';
import { awaitWithAbort, isAbortError } from '../abort';
import type { RuntimeDisposal } from '../disposal';

interface ZLinkRuntimeOwnedStore {
  dispose?(): void | Promise<void>;
}

export interface ZLinkRuntimeStartRollbackParts {
  readonly context: Pick<ZLinkBackendContext, 'dispose'>;
  readonly startedLocationRuntime?: ZLinkLocationRuntime;
  readonly streamRuntime?: ZLinkStreamRuntimeManager;
  readonly spotNodeRuntime?: ZLinkSpotNodeRuntimeManager;
  readonly channelRuntime?: ZLinkChannelRuntimeManager;
  readonly ownedStores?: ReadonlyMap<ZLinkRuntimeOwnedStore, RuntimeDisposal>;
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
  readonly serviceRelocation?: {
    drainActorJoinSources?(): Promise<void>;
    dispose(): Promise<void>;
  };
  readonly ownedStores?: ReadonlyMap<ZLinkRuntimeOwnedStore, RuntimeDisposal>;
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
  await runShutdownStep(
    errors,
    () =>
      awaitWithAbort(
        Promise.resolve(parts.serviceRelocation?.drainActorJoinSources?.()),
        parts.shutdownSignal
      ),
    'accepted_work.actor_join_sources'
  );
  await runShutdownStep(
    errors,
    () => parts.streamRuntime?.dispose(parts.shutdownSignal),
    'transport_cleanup.streams'
  );
  await runShutdownStep(
    errors,
    () => parts.spotNodeRuntime?.dispose(parts.shutdownSignal, parts.cleanupDeadline),
    'transport_cleanup.spot_nodes'
  );
  await runShutdownStep(
    errors,
    () => parts.channelRuntime?.dispose(),
    'transport_cleanup.channels'
  );
  await runShutdownStep(
    errors,
    () => awaitWithAbort(Promise.resolve(parts.serviceRelocation?.dispose()), parts.shutdownSignal),
    'owner_cleanup.service_relocation'
  );
  await runShutdownStep(
    errors,
    () => parts.locationSnapshot.lifecycle?.dispose(),
    'owner_cleanup.location_lifecycle'
  );
  await runShutdownStep(
    errors,
    () => parts.locationSnapshot.runtime?.stop(parts.shutdownSignal),
    'owner_cleanup.location_runtime'
  );
  await traceShutdownStep('transport_cleanup.listener_tasks', () =>
    Promise.allSettled(
      state.listenerTasks.map((task) =>
        traceShutdownStep('transport_cleanup.listener_task', () => task)
      )
    )
  );
  if (errors.every(isShutdownAbort))
    await runShutdownStep(errors, () => state.dispose(), 'transport_cleanup.execution_state');
  await disposeOwnedStores(parts.ownedStores, errors, 'owner_cleanup.store');
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
  stores: ReadonlyMap<ZLinkRuntimeOwnedStore, RuntimeDisposal> | undefined,
  errors: unknown[],
  stage?: string
): Promise<void> {
  for (const [store, disposal] of stores ?? []) {
    await runShutdownStep(errors, () => disposal.run(async () => store.dispose?.()), stage);
  }
}

async function runShutdownStep(
  errors: unknown[],
  step: () => Promise<unknown> | unknown,
  stage?: string
): Promise<void> {
  try {
    if (stage === undefined) await step();
    else await traceShutdownStep(stage, step);
  } catch (error) {
    errors.push(error);
  }
}

function isShutdownAbort(error: unknown): boolean {
  if (isAbortError(error)) return true;
  return error instanceof AggregateError && error.errors.every((nested) => isShutdownAbort(nested));
}
