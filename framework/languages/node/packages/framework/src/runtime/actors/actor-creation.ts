import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  RoutingId,
  Type,
  ZLinkActor,
  ZLinkActorFactory,
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import {
  ZLinkMessage
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkConfigurationException } from '../configuration';
import { DefaultZLinkActorContext } from './actor-context';
import {
  ZLinkActorRuntimeState,
  toFrameworkActorRef,
  toFrameworkRoutingId,
  type ZLinkActorCreationAttemptResult
} from './actor-runtime-state';
import type { ZLinkActorManagerOptions } from './actor-runtime-contracts';

export interface ZLinkActorCreateRequest {
  readonly nativeRequest: Message | undefined;
  readonly callbackRequest: ZLinkMessage;
}

export class ZLinkActorCreationCoordinator {
  constructor(private readonly options: ZLinkActorManagerOptions) {}

  async createActor(
    actorId: string,
    actorType: string,
    state: ZLinkActorRuntimeState,
    createRequest: ZLinkActorCreateRequest,
    claimLocation: boolean,
    signal?: AbortSignal
  ): Promise<ZLinkActorCreationAttemptResult> {
    const lifecycle = this.options.locationLifecycle;
    if (lifecycle !== undefined && claimLocation) {
      const nodeRid = this.resolveLocationNodeRid();
      const activation = await lifecycle.executeActorClaimThenActivate(
        actorType,
        actorId,
        nodeRid,
        async () => {
          // A target takeover is the commit point of an in-flight remote move.
          // Keep the source runtime state until the commit reply installs the
          // remote ActorRef used to finish the caller's current request.
          if (state.isMoving || state.remoteActorPacketTarget !== undefined) {
            return;
          }
          this.options.actorDestroyedCleanup?.(actorId);
          state.clearAfterDestroy();
        },
        () => this.createActorAfterClaim(actorId, actorType, state, createRequest, true, signal)
      );
      if (activation.activated !== undefined) {
        if (activation.activated.status === 'rejected') {
          await lifecycle.releaseActor(actorType, actorId);
          return activation.activated;
        }
        if (activation.generation !== undefined) state.setLocationGeneration(activation.generation);
        state.markLocationOwned();
        return activation.activated;
      }
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
        activation.existingLocation === undefined
          ? `Actor '${actorId}' location claim was rejected and no live location row was found.`
          : `Actor '${actorId}' is already active on node '${activation.existingLocation.ownerNodeRid}' (location claim conflict).`
      );
    }

    return await this.createActorAfterClaim(actorId, actorType, state, createRequest, claimLocation, signal);
  }

  async materializeTransferredActor(
    actorId: string,
    actorType: string,
    state: ZLinkActorRuntimeState,
    restore: ((actor: ZLinkActor) => Promise<void>) | undefined
  ): Promise<ZLinkActorCreationAttemptResult> {
    void actorId;
    const context = state.ensureContext(() => new DefaultZLinkActorContext(
      state,
      this.options.joinCoordinator,
      this.options.boundSessionFactory,
      this.options.messageSerializers,
      this.options.actorMeshNameProvider
    ));
    const actor = await (await this.createFactory(actorType)).create(context);
    attachTransferredActorContext(actor, context);
    await restore?.(actor);
    state.bindActor(actor, context);
    const nativeActorNode = this.options.nativeActorNode ?? this.options.nativeActorNodeProvider?.();
    if (nativeActorNode !== undefined) {
      state.ensureNativeActorRef(nativeActorNode);
    }
    return { status: 'created', actor };
  }

  private async createActorAfterClaim(
    actorId: string,
    actorType: string,
    state: ZLinkActorRuntimeState,
    createRequest: ZLinkActorCreateRequest,
    updateLocation: boolean,
    signal?: AbortSignal
  ): Promise<ZLinkActorCreationAttemptResult> {
    const factory = await this.createFactory(actorType);
    const context = state.ensureContext(() => new DefaultZLinkActorContext(
      state,
      this.options.joinCoordinator,
      this.options.boundSessionFactory,
      this.options.messageSerializers,
      this.options.actorMeshNameProvider
    ));
    const actor = await factory.create(context, signal);
    if (actor.context !== context || actor.context.actorId !== context.actorId) {
      throw new ZLinkConfigurationException(
        `Actor factory '${actorType}' must return an Actor bound to the exact supplied context.`
      );
    }
    const nativeActorNode = this.options.nativeActorNode ?? this.options.nativeActorNodeProvider?.();
    const meshName = state.meshName ?? '';
    if (meshName.length === 0 && state.nativeActorRef === undefined) {
      throw new ZLinkConfigurationException(
        `Actor '${actorId}' has no RouteMesh identity.`
      );
    }
    try {
      let nodeRid: RoutingId | undefined;
      if (nativeActorNode !== undefined) {
        const actorRef = state.ensureNativeActorRef(nativeActorNode, createRequest.nativeRequest);
        nodeRid = toFrameworkRoutingId(actorRef.nodeRid);
      } else {
        nodeRid = this.options.actorCreatedNodeRidProvider?.();
      }
      const response = nodeRid === undefined
        ? { accepted: true }
        : await this.options.actorCreatedNotifier?.(
          nodeRid,
          actor,
          createRequest.callbackRequest,
          signal
        ) ?? { accepted: true };
      if (!response.accepted) {
        await this.discardStagingActor(state, nativeActorNode);
        return { status: 'rejected', reply: response.reply };
      }

      state.bindActor(actor, context);
      if (nativeActorNode !== undefined) {
        const actorRef = state.nativeActorRef!;
        if (updateLocation) {
          await this.options.locationLifecycle?.setActorRef(
            actorType,
            actorId,
            toFrameworkActorRef(actorRef, meshName),
            nativeActorNode.status().lifecycleGeneration
          );
          await this.options.publishActorAuthority?.(
            actorType,
            toFrameworkActorRef(actorRef, meshName),
            nativeActorNode.status().lifecycleGeneration,
            signal
          );
        }
      } else if (nodeRid !== undefined && updateLocation) {
        await this.options.locationLifecycle?.setActorRef(
          actorType,
          actorId,
          { actorId, objectGeneration: 1n, meshName, nodeRid },
          0n
        );
      }
      return { status: 'created', actor, reply: response.reply };
    } catch (error) {
      await this.discardStagingActor(state, nativeActorNode);
      throw error;
    }
  }

  private async discardStagingActor(
    state: ZLinkActorRuntimeState,
    nativeActorNode: ZLinkActorManagerOptions['nativeActorNode']
  ): Promise<void> {
    const actorRef = state.nativeActorRef;
    if (nativeActorNode !== undefined && actorRef !== undefined) {
      await nativeActorNode.destroyActor(actorRef, 0);
      state.markNativeActorDestroyed(actorRef);
    }
  }

  private resolveLocationNodeRid(): RoutingId {
    const nativeActorNode = this.options.nativeActorNode ?? this.options.nativeActorNodeProvider?.();
    const nodeRid = nativeActorNode === undefined
      ? this.options.actorCreatedNodeRidProvider?.()
      : toFrameworkRoutingId(nativeActorNode.status().routingId as never);
    if (nodeRid === undefined) {
      throw new ZLinkConfigurationException('Location actor claim requires a node routing id.');
    }
    return nodeRid;
  }

  private async createFactory(actorType: string): Promise<ZLinkActorFactory> {
    const factoryOrType = this.options.actorFactories.get(actorType);
    if (factoryOrType === undefined) {
      throw new ZLinkConfigurationException(`Actor factory '${actorType}' is not registered.`);
    }
    if (typeof factoryOrType === 'function') {
      const type = factoryOrType as Type<ZLinkActorFactory>;
      return await createProviderInstance(type, this.options.providerResolver);
    }
    return factoryOrType;
  }
}

function attachTransferredActorContext(actor: ZLinkActor, context: ZLinkActor['context']): void {
  const current = (actor as { context?: ZLinkActor['context'] }).context;
  if (current === context) {
    return;
  }
  Object.defineProperty(actor, 'context', {
    configurable: false,
    enumerable: true,
    writable: false,
    value: context
  });
}

async function createProviderInstance<T>(
  type: Type<T>,
  resolver: ZLinkProviderResolver | undefined
): Promise<T> {
  const existing = resolver?.get?.(type);
  if (existing !== undefined) {
    return existing;
  }
  const created = await resolver?.create?.(type);
  if (created !== undefined) {
    return created;
  }
  return new (type as new () => T)();
}
