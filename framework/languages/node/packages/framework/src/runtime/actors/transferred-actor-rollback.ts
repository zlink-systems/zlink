import type { ZLinkActor } from '../../contracts';
import type { ZLinkBackendActorRef, ZLinkBackendMeshNode } from '../backend/contracts';
import { closeMeshCompletion } from '../backend';
import { RequestResult } from '../backend/runtime-values';
import type { ZLinkActorManagerOptions } from './actor-runtime-contracts';
import type { ZLinkActorRuntimeState } from './actor-runtime-state';
import { createActorMembership } from './actor-lifecycle-snapshot';

type ZLinkTransferredActorRollbackOptions = Pick<
  ZLinkActorManagerOptions,
  'nativeActorNode' | 'nativeActorNodeProvider' | 'nativeActorCompletionTableProvider'
>;

export class ZLinkTransferredActorRollbackCoordinator {
  constructor(
    private readonly states: Map<string, ZLinkActorRuntimeState>,
    private readonly options: ZLinkTransferredActorRollbackOptions,
    private readonly finalize: (actorId: string, state: ZLinkActorRuntimeState) => Promise<void>
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
    if (node !== undefined && actorRef !== undefined) {
      await this.destroyNativeActor(node, actorRef, signal);
    }
    await this.complete(actor, state);
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
    const completion = await completions.submit(
      () =>
        node.destroyActor({
          nodeRid: actorRef.nodeRid,
          actorId: actorRef.actorId,
          generation: actorRef.generation
        }),
      signal
    );
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

  private async complete(actor: ZLinkActor, state: ZLinkActorRuntimeState): Promise<void> {
    if (this.states.get(actor.context.actorId) !== state) {
      return;
    }
    await state.getOrStartDestroy(createActorMembership(actor).actor.nodeRid, () =>
      this.finalize(actor.context.actorId, state)
    );
  }
}
