import type { ZLinkActor } from '../../contracts';
import type { ZLinkActorRuntimeState } from './actor-runtime-state';

export interface ZLinkActorSourceTransfer {
  /** Starts the ingress handoff before target route lookup. */
  beginDeferredActorHandoff?(actor: ZLinkActor, state: ZLinkActorRuntimeState): void;
  /** Finishes a same-node deferred Join handoff after completion callback. */
  completeDeferredActorHandoff?(
    actor: ZLinkActor,
    completionFailure?: { readonly error: unknown }
  ): Promise<void>;
  /** Releases an early handoff when Join cannot commit. */
  cancelDeferredActorHandoff?(actor: ZLinkActor, state: ZLinkActorRuntimeState): Promise<void>;
}
