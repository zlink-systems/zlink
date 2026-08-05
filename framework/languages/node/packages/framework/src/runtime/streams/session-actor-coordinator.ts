import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  ActorRef,
  ZLinkActor
} from '../../contracts';
import { throwIfAborted } from '../abort';
import { routingIdsEqual } from '../routing-id';
import type { ZLinkRuntimeMetrics } from '../diagnostics';
import {
  ZLinkActorSessionBindingRegistry,
  type ZLinkActorSessionAuthorityFence
} from './actor-session-binding-registry';
import { ZLinkActorSessionLifecycleCoordinator } from './actor-session-lifecycle-coordinator';
import {
  ZLinkManagedStream
} from './managed-stream';
import {
  DefaultZLinkSessionActor,
  DefaultZLinkSessionContext
} from './session-context';

export interface ZLinkSessionActorCoordinatorOptions {
  readonly actorBindTimeoutMs?: number;
  readonly actorRefResolver?: (actor: ZLinkActor) => ActorRef;
  readonly actorAuthorityFenceResolver?: (
    actorId: string,
    signal?: AbortSignal
  ) => Promise<ZLinkActorSessionAuthorityFence | undefined>;
  readonly nativeActorNodeProvider?: () => {
    status(): { readonly routingId: unknown };
  } | undefined;
  readonly nativeActorMeshNameProvider?: () => string | undefined;
  readonly confirmRemoteActorSessionBinding?: (
    actor: ActorRef,
    sessionRid: ActorRef['nodeRid'],
    signal?: AbortSignal
  ) => Promise<void>;
  readonly metrics?: ZLinkRuntimeMetrics;
}

export interface ZLinkRemoteBoundSessionBindRelay {
  relayRemoteBoundSessionBind(stream: ZLinkManagedStream, actorRef: ActorRef): void;
}

export class ZLinkSessionActorCoordinator {
  constructor(
    private readonly routes: ZLinkActorSessionBindingRegistry<DefaultZLinkSessionContext, DefaultZLinkSessionActor>,
    private readonly remoteBoundSessions: ZLinkRemoteBoundSessionBindRelay,
    private readonly sessionActorRuntime: ConstructorParameters<typeof DefaultZLinkSessionActor>[0],
    private readonly options: ZLinkSessionActorCoordinatorOptions = {},
    private readonly lifecycle = new ZLinkActorSessionLifecycleCoordinator()
  ) {}

  async bind(
    context: DefaultZLinkSessionContext,
    actorOrRef: ZLinkActor | ActorRef,
    signal?: AbortSignal
  ): Promise<DefaultZLinkSessionActor> {
    const actorRef = isActorRefLike(actorOrRef)
      ? normalizeActorRef(actorOrRef, this.options.nativeActorMeshNameProvider?.())
      : this.resolveActorRef(actorOrRef as ZLinkActor);
    return await this.lifecycle.run(actorRef.actorId, async () => this.replaceBinding(context, actorRef, signal));
  }

  private async replaceBinding(
    context: DefaultZLinkSessionContext,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<DefaultZLinkSessionActor> {
    return await this.replaceBindingCore(context, actorRef, signal);
  }

  private async replaceBindingCore(
    context: DefaultZLinkSessionContext,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<DefaultZLinkSessionActor> {
    throwIfAborted(signal);
    if (actorRef.actorId.trim().length === 0) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        'Actor id must not be empty.'
      );
    }
    if (context.routingId === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        'Actor session binding requires a stream routing id.'
      );
    }

    const previous = this.routes.route(actorRef.actorId);
    const sameIncarnation =
      previous !== undefined
      && previous.actor.ref.actorId === actorRef.actorId
      && BigInt(previous.actor.ref.objectGeneration) === BigInt(actorRef.objectGeneration);
    const reuseActor =
      previous?.context === context && sameIncarnation ? previous.actor : undefined;
    const previousRef = previous?.actor.ref;
    const replacesSameNativeBinding = previous?.context === context && sameIncarnation;
    if (previous !== undefined && !replacesSameNativeBinding) {
      await this.unbindNativeActor(previous.context, actorRef.actorId, signal);
    }
    let replacementBound = false;
    try {
      await this.bindNativeActor(context, actorRef, signal);
      replacementBound = true;
      if (this.options.confirmRemoteActorSessionBinding !== undefined) {
        await this.options.confirmRemoteActorSessionBinding(
          actorRef,
          this.actorBindingRoutingId(context),
          signal
        );
      }
    } catch (error) {
      const rollbackErrors: unknown[] = [];
      if (replacementBound && !replacesSameNativeBinding) {
        await this.unbindNativeActor(context, actorRef.actorId).catch((rollbackError) => {
          rollbackErrors.push(rollbackError);
        });
      }
      if (
        previous !== undefined
        && previousRef !== undefined
      ) {
        try {
          await this.bindNativeActor(previous.context, previousRef);
          try {
            if (
              this.options.confirmRemoteActorSessionBinding !== undefined
              && previous.context.routingId !== undefined
            ) {
              await this.options.confirmRemoteActorSessionBinding(
                previousRef,
                this.actorBindingRoutingId(previous.context)
              );
            } else {
              this.relayRemoteBoundSessionBind(previous.context, previousRef);
            }
          } catch (relayError) {
            if (!replacesSameNativeBinding) {
              await this.unbindNativeActor(previous.context, previousRef.actorId).catch((unbindError) => {
                rollbackErrors.push(unbindError);
              });
            }
            throw relayError;
          }
        } catch (rollbackError) {
          rollbackErrors.push(rollbackError);
        }
      }
      if (rollbackErrors.length > 0) {
        throw new AggregateError(
          [error, ...rollbackErrors],
          `Actor '${actorRef.actorId}' session bind and rollback failed.`
        );
      }
      throw error;
    }

    const bindingToken = reuseActor?.bindingToken ?? createBindingToken();
    const boundActorRef = withBindingGeneration(
      actorRef,
      context.stream instanceof ZLinkManagedStream
        ? context.stream.actorBindingGeneration(actorRef.actorId)
        : undefined,
      previous?.actor.ref
    );
    const authorityFence = await this.resolveAuthorityFence(boundActorRef, signal);
    const sessionActor = reuseActor ?? new DefaultZLinkSessionActor(this.sessionActorRuntime, boundActorRef, bindingToken);
    sessionActor.updateRef(boundActorRef);
    if (previous === undefined) {
      this.routes.bind(context, sessionActor, bindingToken, authorityFence);
    } else {
      this.routes.replace(previous, context, sessionActor, bindingToken, authorityFence);
    }
    return sessionActor;
  }

  private async resolveAuthorityFence(
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<ZLinkActorSessionAuthorityFence | undefined> {
    const resolved = await this.options.actorAuthorityFenceResolver?.(actorRef.actorId, signal);
    if (resolved !== undefined) return resolved;
    const internal = actorRef as ActorRef & {
      readonly ownershipGeneration?: bigint;
      readonly ownerLeaseGeneration?: bigint;
    };
    return internal.ownershipGeneration === undefined || internal.ownerLeaseGeneration === undefined
      ? undefined
      : {
          authorityOwnerGeneration: internal.ownershipGeneration,
          ownerLeaseGeneration: internal.ownerLeaseGeneration
        };
  }

  async bindOrGet(
    context: DefaultZLinkSessionContext,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<DefaultZLinkSessionActor> {
    const normalizedActorRef = normalizeActorRef(
      actorRef as ActorRef & { readonly generation?: bigint | number },
      this.options.nativeActorMeshNameProvider?.()
    );
    return await this.lifecycle.run(normalizedActorRef.actorId, async () => {
      throwIfAborted(signal);
      const existing = this.routes.find(normalizedActorRef.actorId);
      if (existing !== undefined && sameActorRef(existing.ref, normalizedActorRef)) {
        if (context.findBoundActor(normalizedActorRef.actorId) === existing) {
          return existing;
        }
        return await this.replaceBinding(context, normalizedActorRef, signal);
      }
      if (existing !== undefined) {
        try {
          return await this.replaceBinding(context, normalizedActorRef, signal);
        } catch (error) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorLocationStale,
            `Actor '${normalizedActorRef.actorId}' bound session ref is stale and could not be rebound.`,
            true,
            error
          );
        }
      }
      return await this.replaceBinding(context, normalizedActorRef, signal);
    });
  }

  async rebindActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void> {
    const normalizedActorRef = normalizeActorRef(
      actorRef as ActorRef & { readonly generation?: bigint | number },
      this.options.nativeActorMeshNameProvider?.()
    );
    await this.lifecycle.run(normalizedActorRef.actorId, async () => {
      throwIfAborted(signal);
      const route = this.routes.route(normalizedActorRef.actorId);
      if (route === undefined || sameActorRef(route.actor.ref, normalizedActorRef)) return;
      requireSameIncarnation(route.actor.ref, normalizedActorRef);
      await this.replaceBinding(route.context, normalizedActorRef, signal);
    });
  }

  async refreshActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void> {
    const normalizedActorRef = normalizeActorRef(
      actorRef as ActorRef & { readonly generation?: bigint | number },
      this.options.nativeActorMeshNameProvider?.()
    );
    await this.lifecycle.run(normalizedActorRef.actorId, async () => {
      throwIfAborted(signal);
      const route = this.routes.route(normalizedActorRef.actorId);
      if (route === undefined) return;
      requireSameIncarnation(route.actor.ref, normalizedActorRef);
      await this.replaceBinding(route.context, normalizedActorRef, signal);
    });
  }

  async commitActorRoute(actorRef: ActorRef, signal?: AbortSignal): Promise<void> {
    const normalizedActorRef = normalizeActorRef(
      actorRef as ActorRef & { readonly generation?: bigint | number },
      this.options.nativeActorMeshNameProvider?.()
    );
    await this.lifecycle.run(normalizedActorRef.actorId, async () => {
      throwIfAborted(signal);
      const route = this.routes.route(normalizedActorRef.actorId);
      if (route === undefined) return;
      requireSameIncarnation(route.actor.ref, normalizedActorRef);
      if (routingIdsEqual(route.actor.ref.nodeRid, normalizedActorRef.nodeRid)) {
        route.actor.updateRef(normalizedActorRef);
        const authorityFence = await this.resolveAuthorityFence(normalizedActorRef, signal);
        if (authorityFence !== undefined) {
          this.routes.updateAuthorityFence(normalizedActorRef.actorId, authorityFence);
        }
        return;
      }
      await this.replaceBinding(route.context, normalizedActorRef, signal);
    });
  }

  private resolveActorRef(actor: ZLinkActor): ActorRef {
    if (this.options.actorRefResolver !== undefined) {
      return this.options.actorRefResolver(actor);
    }
    const state = actor.context as unknown as { actorRef?: ActorRef };
    if (state.actorRef !== undefined) {
      return state.actorRef;
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
      `Actor '${actor.context.actorId}' does not have a concrete actor ref.`
    );
  }

  private async bindNativeActor(
    context: DefaultZLinkSessionContext,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<void> {
    if (!(context.stream instanceof ZLinkManagedStream)) {
      return;
    }
    try {
      await context.stream.bindActor(actorRef, this.options.actorBindTimeoutMs ?? 2000, signal);
    } catch (error) {
      throw new Error(
        `Actor '${actorRef.actorId}' native session bind failed: ${error instanceof Error ? error.message : String(error)}`,
        { cause: error }
      );
    }
  }

  private actorBindingRoutingId(context: DefaultZLinkSessionContext): ActorRef['nodeRid'] {
    return context.stream instanceof ZLinkManagedStream
      ? context.stream.actorBindingRoutingId
      : context.routingId as ActorRef['nodeRid'];
  }

  private async unbindNativeActor(
    context: DefaultZLinkSessionContext,
    actorId: string,
    signal?: AbortSignal
  ): Promise<void> {
    if (!(context.stream instanceof ZLinkManagedStream)) {
      return;
    }
    try {
      await context.stream.unbindActor(actorId, this.options.actorBindTimeoutMs ?? 2000, signal);
    } catch (error) {
      throw new Error(
        `Actor '${actorId}' previous native session unbind failed: ${error instanceof Error ? error.message : String(error)}`,
        { cause: error }
      );
    }
  }

  private relayRemoteBoundSessionBind(
    context: DefaultZLinkSessionContext,
    actorRef: ActorRef
  ): void {
    if (!(context.stream instanceof ZLinkManagedStream)) {
      return;
    }
    const localNode = this.options.nativeActorNodeProvider?.();
    if (localNode !== undefined && routingIdsEqual(String(localNode.status().routingId), actorRef.nodeRid)) {
      return;
    }
    this.remoteBoundSessions.relayRemoteBoundSessionBind(context.stream, actorRef);
  }
}

function withBindingGeneration(
  actorRef: ActorRef,
  nativeBindingGeneration: bigint | undefined,
  previousRef: ActorRef | undefined
): ActorRef {
  const input = actorRef as ActorRef & { readonly bindingGeneration?: bigint };
  const previous = previousRef as (ActorRef & { readonly bindingGeneration?: bigint }) | undefined;
  const bindingGeneration = nativeBindingGeneration
    ?? input.bindingGeneration
    ?? previous?.bindingGeneration;
  if (bindingGeneration === undefined) {
    return actorRef;
  }
  const result = { ...actorRef, bindingGeneration } as ActorRef;
  preserveNormalizedActorRefField(result, actorRef, 'objectGeneration');
  preserveNormalizedActorRefField(result, actorRef, 'meshName');
  preserveInternalActorRefField(result, actorRef, 'ownershipGeneration');
  preserveInternalActorRefField(result, actorRef, 'ownerLeaseGeneration');
  return result;
}

function preserveNormalizedActorRefField(
  target: ActorRef,
  source: ActorRef,
  field: 'objectGeneration' | 'meshName'
): void {
  if (Object.prototype.propertyIsEnumerable.call(source, field)) {
    return;
  }
  Object.defineProperty(target, field, {
    configurable: false,
    enumerable: false,
    value: source[field]
  });
}

function preserveInternalActorRefField(
  target: ActorRef,
  source: ActorRef,
  field: 'ownershipGeneration' | 'ownerLeaseGeneration'
): void {
  const value = (source as ActorRef & {
    readonly ownershipGeneration?: bigint;
    readonly ownerLeaseGeneration?: bigint;
  })[field];
  if (value === undefined) return;
  Object.defineProperty(target, field, {
    configurable: false,
    enumerable: false,
    value
  });
}

type CompatibleActorRef = Omit<ActorRef, 'objectGeneration' | 'meshName'> & {
  readonly objectGeneration?: bigint;
  readonly meshName?: string;
  readonly generation?: bigint | number;
};

function isActorRefLike(
  value: ZLinkActor | ActorRef | CompatibleActorRef
): value is CompatibleActorRef {
  return (
    typeof value === 'object'
    && 'nodeRid' in value
    && 'actorId' in value
    && ('objectGeneration' in value || 'generation' in value)
  );
}

function normalizeActorRef(
  value: CompatibleActorRef,
  defaultMeshName: string | undefined
): ActorRef {
  const objectGeneration = value.objectGeneration ?? value.generation;
  if (objectGeneration === undefined) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
      `Actor '${value.actorId}' does not have an object generation.`
    );
  }
  if (value.objectGeneration !== undefined && value.meshName !== undefined) {
    return value as ActorRef;
  }
  const normalized = { ...value };
  if (value.objectGeneration === undefined) {
    Object.defineProperty(normalized, 'objectGeneration', {
      configurable: false,
      enumerable: false,
      value: BigInt(objectGeneration)
    });
  }
  if (value.meshName === undefined) {
    Object.defineProperty(normalized, 'meshName', {
      configurable: false,
      enumerable: false,
      value: defaultMeshName ?? ''
    });
  }
  return normalized as ActorRef;
}

function createBindingToken(): string {
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function sameActorRef(left: ActorRef, right: ActorRef): boolean {
  return routingIdsEqual(left.nodeRid, right.nodeRid)
    && left.actorId === right.actorId
    && BigInt(left.objectGeneration) === BigInt(right.objectGeneration);
}

function requireSameIncarnation(current: ActorRef, updated: ActorRef): void {
  if (
    current.actorId === updated.actorId
    && BigInt(current.objectGeneration) === BigInt(updated.objectGeneration)
  ) {
    return;
  }
  throw createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorLocationStale,
    `Actor '${updated.actorId}' route update cannot replace object generation `
      + `${String(current.objectGeneration)} with ${String(updated.objectGeneration)}.`,
    true
  );
}
