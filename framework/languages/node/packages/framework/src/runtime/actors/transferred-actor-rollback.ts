import type { ZLinkActor } from '../../contracts';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendMeshNode
} from '../backend/contracts';
import { closeMeshCompletion } from '../backend';
import { RequestResult } from '../backend/runtime-values';
import type { ZLinkActorManagerOptions } from './actor-runtime-contracts';
import type { ZLinkActorRuntimeState } from './actor-runtime-state';
import { ZLinkActorRetryDelay } from './actor-retry-delay';

type ZLinkTransferredActorRollbackOptions = Pick<
  ZLinkActorManagerOptions,
  | 'actorDestroyedCleanup'
  | 'nativeActorNode'
  | 'nativeActorNodeProvider'
  | 'nativeActorCompletionTableProvider'
  | 'shutdownSignal'
  | 'metrics'
>;

export class ZLinkTransferredActorRollbackCoordinator {
  private readonly tasks = new Map<string, Promise<void>>();

  constructor(
    private readonly states: Map<string, ZLinkActorRuntimeState>,
    private readonly options: ZLinkTransferredActorRollbackOptions
  ) {}

  async rollback(actor: ZLinkActor, signal?: AbortSignal): Promise<void> {
    const state = this.states.get(actor.context.actorId);
    if (state?.actor !== actor) {
      return;
    }

    state.clearJoinedSpot();
    if (!state.isMoving) {
      state.beginMove();
    }

    const actorRef = state.nativeActorRef;
    const node = this.options.nativeActorNode ?? this.options.nativeActorNodeProvider?.();
    try {
      if (node !== undefined && actorRef !== undefined) {
        await this.destroyNativeActor(node, actorRef, signal);
      }
    } catch (error) {
      this.schedule(actor, state, node, actorRef);
      throw error;
    }
    this.complete(actor, state);
  }

  private schedule(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    node: ZLinkBackendMeshNode | undefined,
    actorRef: ZLinkBackendActorRef | undefined
  ): void {
    if (this.tasks.has(actor.context.actorId)) {
      return;
    }
    const task = this.retry(actor, state, node, actorRef)
      .finally(() => this.tasks.delete(actor.context.actorId));
    this.tasks.set(actor.context.actorId, task);
  }

  private async retry(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    node: ZLinkBackendMeshNode | undefined,
    actorRef: ZLinkBackendActorRef | undefined
  ): Promise<void> {
    const retryDelay = new ZLinkActorRetryDelay();
    while (this.states.get(actor.context.actorId) === state && this.options.shutdownSignal?.aborted !== true) {
      if (!await retryDelay.wait(this.options.shutdownSignal)) return;
      try {
        if (node !== undefined && actorRef !== undefined) {
          await this.destroyNativeActor(node, actorRef);
        }
        this.complete(actor, state);
        return;
      } catch {
        // The tombstone prevents dispatch while the next retry is pending.
      }
    }
  }

  private async destroyNativeActor(
    node: ZLinkBackendMeshNode,
    actorRef: ZLinkBackendActorRef,
    signal?: AbortSignal
  ): Promise<void> {
    const completions = this.options.nativeActorCompletionTableProvider?.();
    if (completions === undefined) {
      throw new Error('Actor rollback requires a running MeshNode completion table.');
    }
    const operationId = node.destroyActor({
      nodeRid: actorRef.nodeRid,
      actorId: actorRef.actorId,
      generation: actorRef.generation
    });
    const completion = await completions.wait(operationId, signal);
    try {
      if (completion.terminalResult !== RequestResult.Ok) {
        throw new Error(
          `Actor rollback destroy failed with '${completion.terminalResult}' (errno ${completion.failureErrno}).`
        );
      }
    } finally {
      closeMeshCompletion(completion);
    }
  }

  private complete(actor: ZLinkActor, state: ZLinkActorRuntimeState): void {
    if (this.states.get(actor.context.actorId) !== state) {
      return;
    }
    this.options.actorDestroyedCleanup?.(actor.context.actorId);
    state.clearAfterDestroy();
    this.states.delete(actor.context.actorId);
    this.options.metrics?.change('zlink.actor.count', -1);
  }
}
