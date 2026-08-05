import type { ActorRef, ZLinkActor } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendActorRef, ZLinkBackendSpotNode } from '../backend';
import type { DefaultZLinkActorManager, ZLinkRemoteBoundSessionTarget } from '../actors';
import { ZLinkPostCommitActorBinder } from '../actors/post-commit-actor-binder';
import type { DefaultZLinkSpotManager, ZLinkSpotNodeRuntimeManager } from '../spots';
import type { ZLinkEntryActorRuntime } from '../spots/spot-runtime-ports';
import type { ZLinkStreamActorLifecyclePort } from '../streams/stream-binding-runtime-ports';
import type { ZLinkBoundSessionRelay } from './bound-session-relay';

export interface ZLinkEntryActorRuntimeOptions {
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
  readonly spotNodeRuntime: () => ZLinkSpotNodeRuntimeManager | undefined;
  readonly streamBindingRuntime: Pick<ZLinkStreamActorLifecyclePort, 'refreshActor'>;
  readonly boundSessionRelay: ZLinkBoundSessionRelay;
  readonly reportPostCommitError?: (error: unknown) => void;
  readonly shutdownSignal?: () => AbortSignal | undefined;
}

export class ZLinkEntryActorRuntimeService implements ZLinkEntryActorRuntime {
  private readonly binder: ZLinkPostCommitActorBinder;

  constructor(private readonly options: ZLinkEntryActorRuntimeOptions) {
    this.binder = new ZLinkPostCommitActorBinder({
      bind: (actorRef) => options.streamBindingRuntime.refreshActor(actorRef),
      reportError: options.reportPostCommitError,
      signal: options.shutdownSignal
    });
  }

  resolveActor(actorId: string): ZLinkActor | undefined {
    return this.options.actorManager()?.getState(actorId)?.actor;
  }

  async commitActorTransaction(actor: ZLinkActor, onJoined: () => Promise<void>): Promise<void> {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    const entryNode = this.options.spotNodeRuntime()?.primaryMeshNode;
    if (state === undefined) {
      throw new Error(`Entry Spot actor '${actor.context.actorId}' state is not available.`);
    }
    if (entryNode === undefined) {
      throw new Error('Entry Spot actor commit requires a started SPOT node runtime.');
    }
    const actorRef = {
      // Preserve Core's binary routing identity. Converting the binding value
      // to its display string would make a later RoutingId.from(...) call
      // interpret the hexadecimal display as different literal bytes.
      nodeRid: entryNode.status().routingId,
      actorId: actor.context.actorId,
      generation: state.nativeActorRef?.generation ?? 0n
    } as unknown as ZLinkBackendActorRef;
    state.clearJoinedSpot();
    state.setNativeActorRef(actorRef);
    let callbackError: unknown;
    try {
      await onJoined();
    } catch (error) {
      callbackError = error;
    }
    const committedState = this.options.actorManager()?.getState(actor.context.actorId);
    if (committedState?.actor === actor) {
      this.options.boundSessionRelay.clearRemoteActorPacketTarget(actor.context.actorId);
      this.binder.bindEventually({
        actorId: actorRef.actorId,
        objectGeneration: actorRef.generation,
        meshName: actor.context.meshName,
        nodeRid: actorRef.nodeRid
      });
    }
    if (callbackError !== undefined) throw callbackError;
  }

  async destroyActor(
    node: ZLinkBackendSpotNode,
    entryNodeRid: ActorRef['nodeRid'],
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    await this.requireActorManager().destroyActor(node, entryNodeRid, actor, signal);
  }

  async routePacket(
    actorId: string,
    parts: readonly Message[],
    returnResponse?: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ): Promise<{ readonly handled: boolean; readonly response?: unknown }> {
    const spotManager = this.options.spotManager();
    const spotId = this.options.actorManager()?.getState(actorId)?.spotId;
    if (spotId === undefined || spotManager === undefined) {
      return { handled: false };
    }
    return {
      handled: true,
      response: await spotManager.dispatchRoutedActorPacket(
        spotId,
        actorId,
        parts,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef
      )
    };
  }

  private requireActorManager(): DefaultZLinkActorManager {
    const manager = this.options.actorManager();
    if (manager === undefined) {
      throw new Error('Entry Spot actor destroy requires ZLINK_ACTOR_MANAGER.');
    }
    return manager;
  }
}
