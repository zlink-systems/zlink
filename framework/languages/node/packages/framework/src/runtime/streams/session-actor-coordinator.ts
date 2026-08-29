import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import {
  ActorRef,
  ZLinkActor,
  ZLinkFrameworkException
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
    signal?: AbortSignal,
    options?: { readonly waitForAcknowledgement?: boolean }
  ) => Promise<void>;
  readonly errorSink?: () => {
    reportRuntimeTaskException(taskName: string, error: unknown): void;
  } | undefined;
  readonly metrics?: ZLinkRuntimeMetrics;
}

export class ZLinkSessionActorCoordinator {
  constructor(
    private readonly routes: ZLinkActorSessionBindingRegistry<DefaultZLinkSessionContext, DefaultZLinkSessionActor>,
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
    signal?: AbortSignal,
    confirmRemoteSessionBinding: boolean | 'send' = true,
    releaseSeal?: { readonly sealId: string }
  ): Promise<DefaultZLinkSessionActor> {
    return await this.replaceBindingCore(
      context,
      actorRef,
      signal,
      confirmRemoteSessionBinding,
      releaseSeal
    );
  }

  private async replaceBindingCore(
    context: DefaultZLinkSessionContext,
    actorRef: ActorRef,
    signal?: AbortSignal,
    confirmRemoteSessionBinding: boolean | 'send' = true,
    releaseSeal?: { readonly sealId: string }
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

    const previous = await this.routes.route(actorRef.actorId);
    const sameIncarnation =
      previous !== undefined
      && previous.context === context
      && previous.actor.ref.actorId === actorRef.actorId
      && BigInt(previous.actor.ref.objectGeneration) === BigInt(actorRef.objectGeneration);
    const samePhysicalBinding = sameIncarnation && sameActorRef(previous!.actor.ref, actorRef);
    const reuseActor =
      sameIncarnation ? previous!.actor : undefined;
    if (reuseActor !== undefined) {
      // A repeated bind from the same physical session is idempotent. A route
      // refresh for the same actor incarnation still submits the native bind,
      // then keeps this session's actor handle and lifecycle token stable.
      if (samePhysicalBinding) {
        return reuseActor;
      }
    }
    const authorityFence = await this.resolveAuthorityFence(actorRef, signal);
    await this.bindNativeActor(context, actorRef, authorityFence, signal);

    const bindingToken = reuseActor?.bindingToken ?? createBindingToken();
    const boundActorRef = withBindingGeneration(
      actorRef,
      context.stream instanceof ZLinkManagedStream
        ? context.stream.actorBindingGeneration(actorRef.actorId)
        : undefined,
      previous?.actor.ref
    );
    const sessionActor = reuseActor
      ?? new DefaultZLinkSessionActor(this.sessionActorRuntime, boundActorRef, bindingToken);
    const sessionIdentity = String(this.actorBindingRoutingId(context));
    sessionActor.updateRef(boundActorRef);
    if (previous === undefined) {
      if (releaseSeal !== undefined) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
          `Actor '${actorRef.actorId}' route switch has no Session binding to release.`,
          true
        );
      }
      await this.routes.bind(context, sessionActor, bindingToken, authorityFence, sessionIdentity);
    } else if (releaseSeal !== undefined) {
      await this.routes.replaceAndReleaseSeal(
        previous,
        context,
        sessionActor,
        bindingToken,
        releaseSeal.sealId,
        authorityFence,
        sessionIdentity
      );
    } else {
      await this.routes.replace(
        previous,
        context,
        sessionActor,
        bindingToken,
        authorityFence,
        sessionIdentity
      );
    }
    if (confirmRemoteSessionBinding !== false && this.options.confirmRemoteActorSessionBinding !== undefined) {
      const waitForAcknowledgement = previous === undefined
        && confirmRemoteSessionBinding !== 'send';
      const confirmation = this.options.confirmRemoteActorSessionBinding(
        boundActorRef,
        this.actorBindingRoutingId(context),
        undefined,
        { waitForAcknowledgement }
      );
      const reportFailure = (error: unknown) => {
        // A fire-and-forget route update retains the already-current binding;
        // its failure is bounded diagnostics rather than a rollback. The
        // relay retries independently and reports the typed failure through
        // the runtime error sink instead of silently discarding it.
        this.options.errorSink?.()?.reportRuntimeTaskException(
          'remote session binding confirmation',
          error
        );
      };
      if (waitForAcknowledgement) {
        try {
          await confirmation;
        } catch (error) {
          // The first bind has no previously accepted route. Its public
          // terminal must fail when the Actor owner rejects or times out the
          // admission, so remove only this provisional local route and retain
          // the relay's typed failure for the caller.
          try {
            if (context.stream instanceof ZLinkManagedStream) {
              await context.stream.unbindActor(
                actorRef.actorId,
                this.options.actorBindTimeoutMs ?? 2000
              );
            }
          } catch (cleanupError) {
            // Native cleanup cannot replace the owner rejection observed by
            // the public bind. It is separately observable for diagnosis.
            reportFailure(cleanupError);
          } finally {
            await this.routes.unbind(actorRef.actorId, context, bindingToken);
          }
          throw error;
        }
      } else {
        // Replacement does not wait for a remote acknowledgement, but its
        // one-way bind must reach transport submission before application
        // relay can use the new Session route. Otherwise the first packet on
        // a reconnected Session can overtake the binding command.
        try {
          await confirmation;
        } catch (error) {
          reportFailure(error);
        }
      }
    }
    return sessionActor;
  }

  private async resolveAuthorityFence(
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<ZLinkActorSessionAuthorityFence | undefined> {
    const internal = actorRef as ActorRef & {
      readonly ownershipGeneration?: bigint;
      readonly ownerLeaseGeneration?: bigint;
      readonly ownerNodeGeneration?: bigint;
    };
    if (internal.ownershipGeneration !== undefined
      && internal.ownerLeaseGeneration !== undefined) {
      const resolved = internal.ownerNodeGeneration === undefined
        ? await this.options.actorAuthorityFenceResolver?.(actorRef.actorId, signal)
        : undefined;
      return {
        authorityOwnerGeneration: internal.ownershipGeneration,
        ownerLeaseGeneration: internal.ownerLeaseGeneration,
        ...((internal.ownerNodeGeneration ?? resolved?.ownerNodeGeneration) === undefined
          ? {}
          : { ownerNodeGeneration: internal.ownerNodeGeneration ?? resolved!.ownerNodeGeneration }),
        ...(resolved?.ownerId === undefined ? {} : { ownerId: resolved.ownerId }),
        ...(resolved?.authorityStoreVersion === undefined
          ? {}
          : { authorityStoreVersion: resolved.authorityStoreVersion }),
        ...(resolved?.actorType === undefined ? {} : { actorType: resolved.actorType })
      };
    }
    return await this.options.actorAuthorityFenceResolver?.(actorRef.actorId, signal);
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
      const existing = await this.routes.find(normalizedActorRef.actorId);
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
      const route = await this.routes.route(normalizedActorRef.actorId);
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
      const route = await this.routes.route(normalizedActorRef.actorId);
      if (route === undefined) return;
      requireSameIncarnation(route.actor.ref, normalizedActorRef);
      await this.replaceBinding(route.context, normalizedActorRef, signal);
    });
  }

  async commitActorRoute(
    actorRef: ActorRef,
    signal?: AbortSignal,
    options: {
      readonly confirmRemoteSessionBinding?: boolean | 'send';
      readonly releaseSeal?: {
        readonly sealId: string;
      };
    } = {}
  ): Promise<void> {
    const normalizedActorRef = normalizeActorRef(
      actorRef as ActorRef & { readonly generation?: bigint | number },
      this.options.nativeActorMeshNameProvider?.()
    );
    await this.lifecycle.run(normalizedActorRef.actorId, async () => {
      throwIfAborted(signal);
      const route = await this.routes.route(normalizedActorRef.actorId);
      if (route === undefined) return;
      requireSameIncarnation(route.actor.ref, normalizedActorRef);
      if (routingIdsEqual(route.actor.ref.nodeRid, normalizedActorRef.nodeRid)) {
        const authorityFence = await this.resolveAuthorityFence(normalizedActorRef, signal);
        if (options.releaseSeal !== undefined
          && !await this.routes.validateSeal(
            normalizedActorRef.actorId,
            options.releaseSeal.sealId
          )) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorLocationStale,
            `Actor '${normalizedActorRef.actorId}' route switch did not match its relocation seal.`,
            true
          );
        }
        route.actor.updateRef(normalizedActorRef);
        if (authorityFence !== undefined) {
          await this.routes.updateAuthorityFence(normalizedActorRef.actorId, authorityFence);
        }
        if (options.releaseSeal !== undefined
          && !await this.routes.abortSeal(
            normalizedActorRef.actorId,
            options.releaseSeal.sealId
          )) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorLocationStale,
            `Actor '${normalizedActorRef.actorId}' route switch lost its relocation seal.`,
            true
          );
        }
        return;
      }
      await this.replaceBinding(
        route.context,
        normalizedActorRef,
        signal,
        options.confirmRemoteSessionBinding,
        options.releaseSeal
      );
    });
  }

  async retireRemoteBinding(
    actorRef: ActorRef,
    sessionRid: ActorRef['nodeRid'],
    bindingGeneration: bigint,
    signal?: AbortSignal
  ): Promise<boolean> {
    return await this.lifecycle.run(actorRef.actorId, async () => {
      throwIfAborted(signal);
      const route = await this.routes.route(actorRef.actorId);
      if (
        route === undefined
        || !sameActorRef(route.actor.ref, actorRef)
        || !routingIdsEqual(route.sessionIdentity, sessionRid)
        || bindingGenerationOf(route.actor.ref) !== bindingGeneration
      ) {
        return false;
      }
      // A late legacy tombstone may remove only the still-current exact route.
      // A replacement route has a different token or ActorRef, so this path
      // cannot roll it back or wait on the retired session owner.
      await this.routes.unbind(actorRef.actorId, route.context, route.bindingToken);
      return true;
    });
  }

  async authorityFence(actorId: string): Promise<{
    readonly authorityOwnerGeneration: bigint;
    readonly ownerLeaseGeneration: bigint;
  } | undefined> {
    return (await this.routes.route(actorId))?.authorityFence;
  }

  async sessionRouteFence(actorId: string): Promise<{
    readonly actor: ActorRef;
    readonly sessionRid: ActorRef['nodeRid'];
    readonly bindingGeneration: bigint;
  } | undefined> {
    const route = await this.routes.route(actorId);
    const sessionRid = route?.sessionIdentity;
    const actor = route?.actor.ref as (ActorRef & { readonly bindingGeneration?: bigint }) | undefined;
    if (
      route === undefined
      || sessionRid === undefined
      || actor?.bindingGeneration === undefined
    ) {
      return undefined;
    }
    return {
      actor,
      sessionRid,
      bindingGeneration: actor.bindingGeneration
    };
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
    authorityFence: ZLinkActorSessionAuthorityFence | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    if (!(context.stream instanceof ZLinkManagedStream)) {
      return;
    }
    try {
      await context.stream.bindActor(
        actorRef,
        this.options.actorBindTimeoutMs ?? 2000,
        signal,
        context.actorBindingReplacedHandler,
        authorityFence
      );
    } catch (error) {
      if (error instanceof ZLinkFrameworkException) {
        throw error;
      }
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

function bindingGenerationOf(actorRef: ActorRef): bigint | undefined {
  return (actorRef as ActorRef & { readonly bindingGeneration?: bigint }).bindingGeneration;
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
