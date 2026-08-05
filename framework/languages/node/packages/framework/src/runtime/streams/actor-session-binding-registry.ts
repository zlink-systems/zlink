import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { createAbortError, throwIfAborted } from '../abort';
export interface ZLinkActorSessionBindingActor {
  readonly actorId: string;
}

export interface ZLinkActorSessionBindingContext<TActor extends ZLinkActorSessionBindingActor> {
  bindLocal(actor: TActor, bindingToken: string): void;
  unbindLocal(actorId: string, bindingToken: string): void;
}

export interface ZLinkActorSessionRoute<
  TContext extends ZLinkActorSessionBindingContext<TActor>,
  TActor extends ZLinkActorSessionBindingActor
> {
  readonly context: TContext;
  readonly actor: TActor;
  readonly bindingToken: string;
  acceptedHighWater: bigint;
  sealId?: string;
  authorityFence?: ZLinkActorSessionAuthorityFence;
}

export interface ZLinkActorSessionAuthorityFence {
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
}

export class ZLinkActorSessionBindingRegistry<
  TContext extends ZLinkActorSessionBindingContext<TActor>,
  TActor extends ZLinkActorSessionBindingActor
> {
  private readonly routes = new Map<string, ZLinkActorSessionRoute<TContext, TActor>>();
  private readonly sealWaiters = new Map<string, Set<{
    readonly bindingToken: string;
    readonly resolve: () => void;
    readonly reject: (error: unknown) => void;
  }>>();

  bind(
    context: TContext,
    actor: TActor,
    bindingToken: string,
    authorityFence?: ZLinkActorSessionAuthorityFence
  ): void {
    this.routes.set(actor.actorId, {
      context,
      actor,
      bindingToken,
      acceptedHighWater: actorAcceptedHighWater(actor),
      authorityFence
    });
    context.bindLocal(actor, bindingToken);
  }

  replace(
    previous: ZLinkActorSessionRoute<TContext, TActor>,
    context: TContext,
    actor: TActor,
    bindingToken: string,
    authorityFence?: ZLinkActorSessionAuthorityFence
  ): void {
    const current = this.routes.get(actor.actorId);
    if (current !== previous) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
        `Actor '${actor.actorId}' session binding changed before route replacement.`,
        true
      );
    }

    context.bindLocal(actor, bindingToken);
    const sameLocalBinding = previous.context === context
      && previous.bindingToken === bindingToken;
    if (!sameLocalBinding) {
      try {
        previous.context.unbindLocal(actor.actorId, previous.bindingToken);
      } catch (error) {
        context.unbindLocal(actor.actorId, bindingToken);
        previous.context.bindLocal(previous.actor, previous.bindingToken);
        throw error;
      }
    }
    this.routes.set(actor.actorId, {
      context,
      actor,
      bindingToken,
      acceptedHighWater: previous.acceptedHighWater,
      sealId: previous.sealId,
      authorityFence: authorityFence ?? previous.authorityFence
    });
  }

  find(actorId: string): TActor | undefined {
    return this.routes.get(actorId)?.actor;
  }

  route(actorId: string): ZLinkActorSessionRoute<TContext, TActor> | undefined {
    return this.routes.get(actorId);
  }

  unbind(actorId: string, context: TContext, bindingToken: string): void {
    const route = this.routes.get(actorId);
    if (route === undefined || route.context !== context || route.bindingToken !== bindingToken) {
      return;
    }
    this.routes.delete(actorId);
    context.unbindLocal(actorId, bindingToken);
    this.rejectSealWaiters(
      actorId,
      new Error(`Actor '${actorId}' session binding was removed while ingress was held.`)
    );
  }

  unbindActor(actorId: string): void {
    const route = this.routes.get(actorId);
    if (route === undefined) {
      return;
    }
    this.unbind(actorId, route.context, route.bindingToken);
  }

  cleanup(context: TContext): void {
    for (const route of [...this.routes.values()]) {
      if (route.context === context) {
        this.unbind(route.actor.actorId, context, route.bindingToken);
      }
    }
  }

  requireRoute(actorId: string): ZLinkActorSessionRoute<TContext, TActor> {
    const route = this.routes.get(actorId);
    if (route !== undefined) {
      return route;
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
      `No current session binding exists for actor '${actorId}'.`,
      true
    );
  }

  requireCurrentToken(actorId: string, bindingToken: string): void {
    const route = this.requireRoute(actorId);
    if (route.bindingToken === bindingToken) {
      return;
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
      `Actor '${actorId}' session binding is stale.`,
      true
    );
  }

  accept(actorId: string, bindingToken: string): bigint {
    const route = this.requireRoute(actorId);
    if (route.bindingToken !== bindingToken) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
        `Actor '${actorId}' session binding is stale.`,
        true
      );
    }
    if (route.sealId !== undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actorId}' session ingress is sealed for relocation.`,
        true
      );
    }
    route.acceptedHighWater++;
    return route.acceptedHighWater;
  }

  /**
   * Holds Session ingress accepted after a relocation seal until the target
   * route is published and the seal is released. The caller keeps the
   * request payload and reply context open while this wait is in progress.
   */
  async acceptWhenReady(
    actorId: string,
    bindingToken: string,
    signal?: AbortSignal
  ): Promise<bigint> {
    for (;;) {
      throwIfAborted(signal);
      const route = this.requireRoute(actorId);
      this.requireCurrentToken(actorId, bindingToken);
      if (route.sealId === undefined) {
        try {
          const acceptedHighWater = this.accept(actorId, bindingToken);
          return acceptedHighWater;
        } catch (error) {
          // A new seal can race the check above. Re-enter the wait only for
          // that relocation fence; unrelated binding failures stay visible.
          if (this.routes.get(actorId)?.sealId === undefined) throw error;
        }
      }
      await this.waitForSealRelease(actorId, bindingToken, signal);
    }
  }

  seal(actorId: string, sealId: string, expected: ZLinkActorSessionRouteFence): bigint {
    const route = this.requireRoute(actorId);
    if (route.sealId !== undefined) {
      if (route.sealId === sealId && routeMatchesFence(route, expected)) {
        return route.acceptedHighWater;
      }
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actorId}' session ingress is sealed by another relocation.`,
        true
      );
    }
    if (!routeMatchesFence(route, expected)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
        `Actor '${actorId}' session route seal was fenced by its binding identity.`,
        true
      );
    }
    route.sealId = sealId;
    return route.acceptedHighWater;
  }

  abortSeal(actorId: string, sealId: string): boolean {
    const route = this.routes.get(actorId);
    if (route === undefined || route.sealId !== sealId) return false;
    route.sealId = undefined;
    this.resolveSealWaiters(actorId, sealId);
    return true;
  }

  updateAuthorityFence(actorId: string, authorityFence: ZLinkActorSessionAuthorityFence): void {
    const route = this.requireRoute(actorId);
    route.authorityFence = authorityFence;
  }

  validateSeal(actorId: string, sealId: string, acceptedHighWater: bigint): boolean {
    const route = this.routes.get(actorId);
    return route !== undefined
      && route.sealId === sealId
      && route.acceptedHighWater === acceptedHighWater;
  }

  private async waitForSealRelease(
    actorId: string,
    bindingToken: string,
    signal?: AbortSignal
  ): Promise<void> {
    const route = this.requireRoute(actorId);
    this.requireCurrentToken(actorId, bindingToken);
    if (route.sealId === undefined) return;
    await new Promise<void>((resolve, reject) => {
      const waiters = this.sealWaiters.get(actorId) ?? new Set();
      const waiter = { bindingToken, resolve, reject };
      waiters.add(waiter);
      this.sealWaiters.set(actorId, waiters);
      const onAbort = () => {
        waiters.delete(waiter);
        if (waiters.size === 0) this.sealWaiters.delete(actorId);
        reject(createAbortError());
      };
      if (signal === undefined) return;
      if (signal.aborted) {
        onAbort();
        return;
      }
      signal.addEventListener('abort', onAbort, { once: true });
    });
  }

  private resolveSealWaiters(actorId: string, _sealId: string): void {
    const waiters = this.sealWaiters.get(actorId);
    if (waiters === undefined) return;
    this.sealWaiters.delete(actorId);
    for (const waiter of waiters) waiter.resolve();
  }

  private rejectSealWaiters(actorId: string, error: unknown): void {
    const waiters = this.sealWaiters.get(actorId);
    if (waiters === undefined) return;
    this.sealWaiters.delete(actorId);
    for (const waiter of waiters) waiter.reject(error);
  }
}

export interface ZLinkActorSessionRouteFence {
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly bindingGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
}

function routeMatchesFence<
  TContext extends ZLinkActorSessionBindingContext<TActor>,
  TActor extends ZLinkActorSessionBindingActor
>(route: ZLinkActorSessionRoute<TContext, TActor>, expected: ZLinkActorSessionRouteFence): boolean {
  const ref = (route.actor as TActor & { readonly ref?: unknown }).ref as {
    readonly objectGeneration?: bigint;
    readonly generation?: bigint;
    readonly ownershipGeneration?: bigint;
    readonly bindingGeneration?: bigint;
    readonly ownerLeaseGeneration?: bigint;
  } | undefined;
  return ref !== undefined
    && BigInt(ref.objectGeneration ?? ref.generation ?? -1n) === expected.objectGeneration
    && ref.bindingGeneration === expected.bindingGeneration
    && route.authorityFence?.authorityOwnerGeneration === expected.authorityOwnerGeneration
    && route.authorityFence.ownerLeaseGeneration === expected.ownerLeaseGeneration;
}

function actorAcceptedHighWater<TActor extends ZLinkActorSessionBindingActor>(actor: TActor): bigint {
  const value = (actor as TActor & { readonly ref?: { readonly acceptedHighWater?: bigint } })
    .ref?.acceptedHighWater;
  return value === undefined || value < 0n ? 0n : value;
}
