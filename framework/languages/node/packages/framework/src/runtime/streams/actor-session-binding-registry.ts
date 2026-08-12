import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { createAbortError, throwIfAborted } from '../abort';
import { routingIdsEqual } from '../routing-id';
import type {
  ServiceSessionBindingAdmissionClaim,
  ServiceSessionBindingAdmissionResult
} from '../foundation/service-session-binding-ingress-port';
export interface ZLinkActorSessionBindingActor {
  readonly actorId: string;
}

export interface ZLinkActorSessionBindingContext<TActor extends ZLinkActorSessionBindingActor> {
  readonly routingId?: unknown;
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
  readonly sessionIdentity?: string;
  acceptedHighWater: bigint;
  activeFrames: number;
  sealId?: string;
  authorityFence?: ZLinkActorSessionAuthorityFence;
}

export interface ZLinkActorSessionRelocationClaim {
  readonly actorId: string;
  readonly actorGeneration: bigint;
  readonly actorOwnershipGeneration: bigint;
  readonly bindingGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
  readonly sessionIdentity?: string;
  readonly actorNodeRid?: string;
  readonly actorNodeGeneration?: bigint;
  readonly sealId: string;
}

export interface ZLinkActorSessionRetainedOutbound {
  deliver(): Promise<boolean>;
  fail(error: unknown): void;
}

export interface ZLinkActorSessionAcceptedProducerProof {
  readonly actorId: string;
  readonly objectGeneration: bigint;
  readonly actorNodeRid: string;
  readonly actorNodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
  readonly sessionIdentity: string;
  readonly bindingGeneration: bigint;
}

export interface ZLinkActorSessionRelocationSnapshot extends ZLinkActorSessionRelocationClaim {
  readonly acceptedHighWater: bigint;
  readonly phase: 'sealed' | 'applying' | 'applied' | 'terminal';
  readonly applyFingerprint?: string;
}

interface ZLinkActorSessionFrameAdmission {
  readonly acceptedHighWater: bigint;
  complete(): void;
}

interface ZLinkActorSessionActiveFrameWaiter {
  readonly resolve: () => void;
  readonly reject: (error: unknown) => void;
}

interface ZLinkActorSessionRelocationState {
  readonly actorId: string;
  readonly actorGeneration: bigint;
  actorOwnershipGeneration: bigint;
  readonly bindingGeneration: bigint;
  ownerLeaseGeneration: bigint;
  readonly sessionIdentity?: string;
  readonly actorNodeRid?: string;
  readonly actorNodeGeneration?: bigint;
  readonly sealId: string;
  readonly acceptedHighWater: bigint;
  phase: 'sealed' | 'applying' | 'applied' | 'terminal';
  applyFingerprint?: string;
  applyPromise?: Promise<void>;
  acceptedProducerProof?: ZLinkActorSessionAcceptedProducerProof;
  readonly ready: Promise<void>;
  readonly terminal: Promise<void>;
  readonly resolveTerminal: () => void;
}

interface ZLinkActorSessionOutboundEntry {
  readonly sealId?: string;
  readonly operation: ZLinkActorSessionRetainedOutbound;
  readonly arrivalOrder: bigint;
  readonly claim?: ServiceSessionBindingAdmissionClaim;
  readonly authorization: 'legacy' | 'source' | 'pendingTarget';
  released: boolean;
  settled: boolean;
}

interface ZLinkActorSessionRelocationQueue {
  readonly actorId: string;
  activeSealId?: string;
  readonly seals: Map<string, ZLinkActorSessionRelocationState>;
  readonly outbound: ZLinkActorSessionOutboundEntry[];
  nextArrivalOrder: bigint;
  outboundCount: number;
  drainPromise?: Promise<void>;
}

export interface ZLinkActorSessionAuthorityFence {
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
  readonly ownerId?: string;
  readonly ownerNodeGeneration?: bigint;
  readonly authorityStoreVersion?: string;
  readonly actorType?: string;
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
  private readonly activeFrameWaiters = new Map<string, Set<ZLinkActorSessionActiveFrameWaiter>>();
  private readonly relocations = new Map<string, ZLinkActorSessionRelocationQueue>();
  private readonly terminalRelocations = new Map<ZLinkActorSessionRelocationState, true>();

  constructor(
    private readonly terminalRelocationCapacity = 4096,
    private readonly outboundCapacity = 4096
  ) {
    if (!Number.isSafeInteger(terminalRelocationCapacity) || terminalRelocationCapacity <= 0) {
      throw new RangeError('Session relocation terminal capacity must be a positive safe integer.');
    }
    if (!Number.isSafeInteger(outboundCapacity) || outboundCapacity <= 0) {
      throw new RangeError('Session relocation outbound capacity must be a positive safe integer.');
    }
  }

  bind(
    context: TContext,
    actor: TActor,
    bindingToken: string,
    authorityFence?: ZLinkActorSessionAuthorityFence,
    sessionIdentity?: string
  ): void {
    this.routes.set(actor.actorId, {
      context,
      actor,
      bindingToken,
      sessionIdentity: sessionIdentity ?? sessionIdentityFromContext(context),
      acceptedHighWater: actorAcceptedHighWater(actor),
      activeFrames: 0,
      authorityFence
    });
    context.bindLocal(actor, bindingToken);
  }

  replace(
    previous: ZLinkActorSessionRoute<TContext, TActor>,
    context: TContext,
    actor: TActor,
    bindingToken: string,
    authorityFence?: ZLinkActorSessionAuthorityFence,
    sessionIdentity?: string
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
      sessionIdentity: sessionIdentity ?? sessionIdentityFromContext(context),
      acceptedHighWater: previous.acceptedHighWater,
      activeFrames: previous.activeFrames,
      sealId: previous.sealId,
      authorityFence: authorityFence ?? previous.authorityFence
    });
  }

  replaceAndReleaseSeal(
    previous: ZLinkActorSessionRoute<TContext, TActor>,
    context: TContext,
    actor: TActor,
    bindingToken: string,
    sealId: string,
    acceptedHighWater: bigint,
    authorityFence?: ZLinkActorSessionAuthorityFence,
    sessionIdentity?: string
  ): void {
    if (
      previous.sealId !== sealId
      || previous.acceptedHighWater !== acceptedHighWater
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actor.actorId}' route switch did not match its relocation seal.`,
        true
      );
    }
    // JavaScript cannot interleave another ingress turn between these two
    // synchronous mutations. The replacement preserves the seal, then the
    // exact release publishes the route to held ingress as one owner turn.
    this.replace(previous, context, actor, bindingToken, authorityFence, sessionIdentity);
    if (!this.abortSeal(actor.actorId, sealId)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actor.actorId}' route switch lost its relocation seal.`,
        true
      );
    }
  }

  find(actorId: string): TActor | undefined {
    return this.routes.get(actorId)?.actor;
  }

  route(actorId: string): ZLinkActorSessionRoute<TContext, TActor> | undefined {
    return this.routes.get(actorId);
  }

  capturePendingReplyClaim(
    actorId: string,
    actor: ZLinkActorSessionBindingActor,
    bindingToken: string
  ): { readonly context: TContext } | undefined {
    const route = this.routes.get(actorId);
    if (route === undefined || route.actor !== actor || route.bindingToken !== bindingToken) {
      return undefined;
    }
    return Object.freeze({ context: route.context });
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
    this.rejectActiveFrameWaiters(
      actorId,
      new Error(`Actor '${actorId}' session binding was removed while accepted frames were active.`)
    );
    this.clearRelocation(actorId, new Error(`Actor '${actorId}' session binding was removed.`));
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

  async beginAcceptedFrameWhenReady(
    actorId: string,
    bindingToken: string,
    signal?: AbortSignal
  ): Promise<ZLinkActorSessionFrameAdmission> {
    for (;;) {
      throwIfAborted(signal);
      const route = this.requireRoute(actorId);
      this.requireCurrentToken(actorId, bindingToken);
      if (route.sealId === undefined) {
        route.acceptedHighWater++;
        route.activeFrames++;
        let completed = false;
        return {
          acceptedHighWater: route.acceptedHighWater,
          complete: () => {
            if (completed) return;
            completed = true;
            route.activeFrames--;
            if (route.activeFrames === 0) this.resolveActiveFrameWaiters(actorId);
          }
        };
      }
      await this.waitForSealRelease(actorId, bindingToken, signal);
    }
  }

  async runAcceptedFrameWhenReady<TResult>(
    actorId: string,
    bindingToken: string,
    operation: () => Promise<TResult>,
    signal?: AbortSignal
  ): Promise<TResult> {
    const admission = await this.beginAcceptedFrameWhenReady(
      actorId,
      bindingToken,
      signal
    );
    try {
      return await operation();
    } finally {
      admission.complete();
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

  async sealAndWait(
    actorId: string,
    sealId: string,
    expected: ZLinkActorSessionRouteFence,
    signal?: AbortSignal
  ): Promise<bigint> {
    const acceptedHighWater = this.seal(actorId, sealId, expected);
    await this.waitForActiveFrames(actorId, signal);
    return acceptedHighWater;
  }

  async sealRelocation(
    claim: ZLinkActorSessionRelocationClaim,
    expected: ZLinkActorSessionRouteFence,
    signal?: AbortSignal
  ): Promise<bigint> {
    for (;;) {
      throwIfAborted(signal);
      const queue = this.relocations.get(claim.actorId);
      const existing = queue?.seals.get(claim.sealId);
      if (existing !== undefined) {
        assertRelocationClaim(existing, claim);
        await existing.ready;
        return existing.acceptedHighWater;
      }
      const predecessor = queue?.activeSealId === undefined
        ? undefined
        : queue.seals.get(queue.activeSealId);
      if (predecessor !== undefined && predecessor.phase !== 'terminal') {
        if (predecessor.sessionIdentity === undefined || claim.sessionIdentity === undefined) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorLocationStale,
            `Actor '${claim.actorId}' session ingress is sealed by another relocation.`,
            true
          );
        }
        assertSuccessorSessionIdentity(predecessor, claim);
        await predecessor.terminal;
        continue;
      }

      const acceptedHighWater = this.seal(claim.actorId, claim.sealId, expected);
      let resolveTerminal!: () => void;
      const terminal = new Promise<void>((resolve) => { resolveTerminal = resolve; });
      const ready = this.waitForActiveFrames(claim.actorId, signal);
      const actorQueue: ZLinkActorSessionRelocationQueue = queue ?? {
        actorId: claim.actorId,
        seals: new Map(),
        outbound: [],
        nextArrivalOrder: 1n,
        outboundCount: 0
      };
      const state: ZLinkActorSessionRelocationState = {
        ...claim,
        acceptedHighWater,
        phase: 'sealed',
        ready,
        terminal,
        resolveTerminal
      };
      actorQueue.seals.set(claim.sealId, state);
      actorQueue.activeSealId = claim.sealId;
      this.relocations.set(claim.actorId, actorQueue);
      await ready;
      return acceptedHighWater;
    }
  }

  relocationSnapshot(actorId: string, sealId: string): ZLinkActorSessionRelocationSnapshot | undefined {
    return this.relocations.get(actorId)?.seals.get(sealId);
  }

  retainRelocationOutbound(
    actorId: string,
    operation: ZLinkActorSessionRetainedOutbound
  ): ServiceSessionBindingAdmissionResult {
    return this.retainRelocationOutboundCore(actorId, operation, 'legacy');
  }

  admitRelocationOutbound(
    claim: ServiceSessionBindingAdmissionClaim,
    operation: ZLinkActorSessionRetainedOutbound
  ): ServiceSessionBindingAdmissionResult {
    const queue = this.relocations.get(claim.actorId);
    const state = queue?.activeSealId === undefined
      ? undefined
      : queue.seals.get(queue.activeSealId);
    const error = this.outboundAdmissionError(claim);
    if (error !== undefined) {
      failRetainedOutbound(operation, error);
      return 'rejected';
    }
    if (queue === undefined || state === undefined) {
      if (!this.matchesCurrentProducerProof(claim)) {
        failRetainedOutbound(
          operation,
          new Error(`Actor '${claim.actorId}' Session outbound admission was fenced by its current binding.`)
        );
        return 'rejected';
      }
      if (queue === undefined) return 'passThrough';
      return this.retainRelocationOutboundCore(
        claim.actorId,
        operation,
        'source',
        claim
      );
    }
    if (state.phase === 'sealed' || state.phase === 'applying') {
      const matchesSource = matchesRelocationSourceProof(state, claim);
      const matchesCurrent = this.matchesCurrentProducerProof(claim);
      if (
        !matchesSource
        && !matchesCurrent
        && matchesRelocationSourceNodeTenure(state, claim)
      ) {
        failRetainedOutbound(
          operation,
          new Error(`Actor '${claim.actorId}' Session source producer changed its authority fence.`)
        );
        return 'rejected';
      }
      const authorization = matchesSource || matchesCurrent
        ? 'source'
        : 'pendingTarget';
      if (
        authorization === 'pendingTarget'
        && claim.authorityOwnerGeneration <= state.actorOwnershipGeneration
      ) {
        failRetainedOutbound(
          operation,
          new Error(`Actor '${claim.actorId}' Session outbound candidate did not advance its producer authority.`)
        );
        return 'rejected';
      }
      return this.retainRelocationOutboundCore(
        claim.actorId,
        operation,
        authorization,
        claim
      );
    }
    const acceptedProof = state.acceptedProducerProof;
    if (
      acceptedProof === undefined
      || !matchesAcceptedProducerProof(acceptedProof, claim)
    ) {
      failRetainedOutbound(
        operation,
        new Error(`Actor '${claim.actorId}' Session outbound admission did not match its accepted producer proof.`)
      );
      return 'rejected';
    }
    return this.retainRelocationOutboundCore(
      claim.actorId,
      operation,
      'source',
      claim
    );
  }

  private retainRelocationOutboundCore(
    actorId: string,
    operation: ZLinkActorSessionRetainedOutbound,
    authorization: ZLinkActorSessionOutboundEntry['authorization'],
    claim?: ServiceSessionBindingAdmissionClaim
  ): ServiceSessionBindingAdmissionResult {
    const queue = this.relocations.get(actorId);
    const state = queue?.activeSealId === undefined
      ? undefined
      : queue.seals.get(queue.activeSealId);
    if (queue === undefined) return 'passThrough';
    if (state === undefined || state.phase === 'applied' || state.phase === 'terminal') {
      if (queue.drainPromise === undefined && queue.outbound.length === 0) return 'passThrough';
      if (!this.reserveOutbound(queue, operation)) return 'rejected';
      queue.outbound.push({
        operation,
        authorization,
        arrivalOrder: queue.nextArrivalOrder++,
        ...(claim === undefined ? {} : { claim }),
        released: true,
        settled: false
      });
      this.startOutboundDrain(queue);
      return 'retained';
    }
    if (!this.reserveOutbound(queue, operation)) return 'rejected';
    queue.outbound.push({
      sealId: state.sealId,
      operation,
      authorization,
      arrivalOrder: queue.nextArrivalOrder++,
      ...(claim === undefined ? {} : { claim }),
      released: false,
      settled: false
    });
    return 'retained';
  }

  discardRelocationOutbound(actorId: string, sealId: string, error: unknown): void {
    const queue = this.relocations.get(actorId);
    if (queue === undefined) return;
    for (let index = queue.outbound.length - 1; index >= 0; index--) {
      const entry = queue.outbound[index]!;
      if (entry.sealId !== sealId) continue;
      queue.outbound.splice(index, 1);
      this.failOutbound(queue, entry, error);
    }
    this.deleteEmptyRelocationQueue(queue);
  }

  advanceRelocationOwner(
    actorId: string,
    sealId: string,
    previousOwnershipGeneration: bigint,
    previousOwnerLeaseGeneration: bigint,
    targetOwnershipGeneration: bigint,
    targetOwnerLeaseGeneration: bigint
  ): void {
    const state = this.relocations.get(actorId)?.seals.get(sealId);
    if (state === undefined || state.phase !== 'sealed') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actorId}' ownership update did not match an active Session seal.`,
        true
      );
    }
    const matchesPrevious = state.actorOwnershipGeneration === previousOwnershipGeneration
      && state.ownerLeaseGeneration === previousOwnerLeaseGeneration;
    const alreadyAdvanced = state.actorOwnershipGeneration === targetOwnershipGeneration
      && state.ownerLeaseGeneration === targetOwnerLeaseGeneration;
    if (!matchesPrevious && !alreadyAdvanced) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actorId}' ownership update was fenced by its retained Session seal.`,
        true
      );
    }
    state.actorOwnershipGeneration = targetOwnershipGeneration;
    state.ownerLeaseGeneration = targetOwnerLeaseGeneration;
  }

  async applyRelocation(
    actorId: string,
    sealId: string,
    acceptedHighWater: bigint,
    applyFingerprint: string,
    action: 'commit' | 'abort',
    commitOwnerTransition: () => Promise<void>,
    acceptedProducerProof?: ZLinkActorSessionAcceptedProducerProof
  ): Promise<void> {
    const queue = this.relocations.get(actorId);
    const state = queue?.seals.get(sealId);
    if (queue === undefined || state === undefined || state.acceptedHighWater !== acceptedHighWater) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actorId}' route apply did not match its exact Session seal.`,
        true
      );
    }
    if (state.phase !== 'sealed') {
      if (state.applyFingerprint !== applyFingerprint || state.applyPromise === undefined) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorLocationStale,
          `Actor '${actorId}' Session seal received a conflicting route apply.`,
          true
        );
      }
      await state.applyPromise;
      return;
    }
    if (
      action === 'commit'
      && acceptedProducerProof !== undefined
      && !validAcceptedTargetProof(state, acceptedProducerProof)
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actorId}' command 44 target producer proof did not match its Session seal.`,
        true
      );
    }
    if (action === 'abort' && acceptedProducerProof !== undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actorId}' abort route supplied a target producer proof.`,
        true
      );
    }

    state.phase = 'applying';
    state.applyFingerprint = applyFingerprint;
    const applyPromise = Promise.resolve().then(async () => {
      try {
        await commitOwnerTransition();
        const proof = action === 'commit'
          ? acceptedProducerProof ?? this.currentProducerProof(actorId)
          : relocationSourceProof(state);
        state.acceptedProducerProof = proof;
        for (let index = queue.outbound.length - 1; index >= 0; index--) {
          const entry = queue.outbound[index]!;
          if (entry.sealId !== sealId) continue;
          if (entry.authorization !== 'pendingTarget') {
            entry.released = true;
            continue;
          }
          if (
            action === 'commit'
            && proof !== undefined
            && entry.claim !== undefined
            && matchesAcceptedProducerProof(proof, entry.claim)
          ) {
            entry.released = true;
            continue;
          }
          queue.outbound.splice(index, 1);
          this.failOutbound(
            queue,
            entry,
            new Error(`Actor '${actorId}' pending Session producer did not match command 44 proof.`)
          );
        }
        this.startOutboundDrain(queue);
        state.phase = 'applied';
      } catch (error) {
        state.phase = 'sealed';
        state.applyFingerprint = undefined;
        state.applyPromise = undefined;
        state.acceptedProducerProof = undefined;
        throw error;
      }
    });
    state.applyPromise = applyPromise;
    await applyPromise;
  }

  private currentProducerProof(
    actorId: string
  ): ZLinkActorSessionAcceptedProducerProof | undefined {
    const route = this.routes.get(actorId);
    const actorRef = (route?.actor as TActor & { readonly ref?: {
      readonly actorId?: unknown;
      readonly objectGeneration?: unknown;
      readonly generation?: unknown;
      readonly nodeRid?: unknown;
      readonly bindingGeneration?: unknown;
    } }).ref;
    const authority = route?.authorityFence;
    const sessionIdentity = route?.sessionIdentity;
    if (
      route === undefined
      || actorRef === undefined
      || authority?.ownerNodeGeneration === undefined
      || sessionIdentity === undefined
    ) return undefined;
    return {
      actorId,
      objectGeneration: BigInt(
        actorRef.objectGeneration as bigint | number | string | boolean | undefined
          ?? actorRef.generation as bigint | number | string | boolean | undefined
          ?? -1
      ),
      actorNodeRid: String(actorRef.nodeRid ?? ''),
      actorNodeGeneration: authority.ownerNodeGeneration,
      authorityOwnerGeneration: authority.authorityOwnerGeneration,
      ownerLeaseGeneration: authority.ownerLeaseGeneration,
      sessionIdentity: String(sessionIdentity),
      bindingGeneration: BigInt(
        actorRef.bindingGeneration as bigint | number | string | boolean | undefined ?? -1
      )
    };
  }

  observeRelocationTerminal(
    actorId: string,
    sealId: string,
    acceptedHighWater: bigint,
    applyFingerprint: string
  ): void {
    const queue = this.relocations.get(actorId);
    const state = queue?.seals.get(sealId);
    if (
      queue === undefined
      || state === undefined
      || state.acceptedHighWater !== acceptedHighWater
      || state.applyFingerprint !== applyFingerprint
      || (state.phase !== 'applied' && state.phase !== 'terminal')
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actorId}' Session relocation terminal did not match its exact route apply.`,
        true
      );
    }
    if (state.phase === 'terminal') {
      this.rememberTerminalRelocation(state);
      return;
    }
    state.phase = 'terminal';
    if (queue.activeSealId === sealId) queue.activeSealId = undefined;
    state.resolveTerminal();
    this.rememberTerminalRelocation(state);
  }

  clearRelocation(actorId: string, error: unknown): void {
    const queue = this.relocations.get(actorId);
    if (queue === undefined) return;
    this.relocations.delete(actorId);
    for (const entry of queue.outbound.splice(0)) this.failOutbound(queue, entry, error);
    for (const state of queue.seals.values()) {
      this.terminalRelocations.delete(state);
      if (state.phase !== 'terminal') state.resolveTerminal();
    }
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
      let settled = false;
      const cleanup = () => signal?.removeEventListener('abort', onAbort);
      const resolveWaiter = () => {
        if (settled) return;
        settled = true;
        cleanup();
        resolve();
      };
      const rejectWaiter = (error: unknown) => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(error);
      };
      const waiter = { bindingToken, resolve: resolveWaiter, reject: rejectWaiter };
      waiters.add(waiter);
      this.sealWaiters.set(actorId, waiters);
      const onAbort = () => {
        waiters.delete(waiter);
        if (waiters.size === 0) this.sealWaiters.delete(actorId);
        rejectWaiter(createAbortError());
      };
      if (signal === undefined) return;
      if (signal.aborted) {
        onAbort();
        return;
      }
      signal.addEventListener('abort', onAbort, { once: true });
    });
  }

  private async waitForActiveFrames(actorId: string, signal?: AbortSignal): Promise<void> {
    const route = this.requireRoute(actorId);
    if (route.activeFrames === 0) return;
    await new Promise<void>((resolve, reject) => {
      const waiters = this.activeFrameWaiters.get(actorId) ?? new Set();
      let settled = false;
      const cleanup = () => signal?.removeEventListener('abort', onAbort);
      const waiter: ZLinkActorSessionActiveFrameWaiter = {
        resolve: () => {
          if (settled) return;
          settled = true;
          cleanup();
          resolve();
        },
        reject: (error) => {
          if (settled) return;
          settled = true;
          cleanup();
          reject(error);
        }
      };
      const onAbort = () => {
        waiters.delete(waiter);
        if (waiters.size === 0) this.activeFrameWaiters.delete(actorId);
        waiter.reject(createAbortError());
      };
      waiters.add(waiter);
      this.activeFrameWaiters.set(actorId, waiters);
      if (signal === undefined) return;
      if (signal.aborted) onAbort();
      else signal.addEventListener('abort', onAbort, { once: true });
    });
  }

  private resolveActiveFrameWaiters(actorId: string): void {
    const waiters = this.activeFrameWaiters.get(actorId);
    if (waiters === undefined) return;
    this.activeFrameWaiters.delete(actorId);
    for (const waiter of waiters) waiter.resolve();
  }

  private rejectActiveFrameWaiters(actorId: string, error: unknown): void {
    const waiters = this.activeFrameWaiters.get(actorId);
    if (waiters === undefined) return;
    this.activeFrameWaiters.delete(actorId);
    for (const waiter of waiters) waiter.reject(error);
  }

  private startOutboundDrain(queue: ZLinkActorSessionRelocationQueue): void {
    if (queue.drainPromise !== undefined) return;
    const drain = Promise.resolve().then(async () => {
      for (;;) {
        const entry = queue.outbound.at(0);
        if (entry === undefined || !entry.released) return;
        if (entry.settled) {
          queue.outbound.shift();
          continue;
        }
        try {
          const delivered = await entry.operation.deliver();
          if (delivered) this.settleOutbound(queue, entry);
          else this.failOutbound(
            queue,
            entry,
            new Error(`Actor '${queue.actorId}' retained Session outbound delivery was rejected.`)
          );
        } catch (error) {
          this.failOutbound(queue, entry, error);
        }
        if (queue.outbound[0] === entry) queue.outbound.shift();
      }
    }).finally(() => {
      if (queue.drainPromise === drain) queue.drainPromise = undefined;
      if (queue.outbound[0]?.released === true) this.startOutboundDrain(queue);
      else this.deleteEmptyRelocationQueue(queue);
    });
    queue.drainPromise = drain;
  }

  private reserveOutbound(
    queue: ZLinkActorSessionRelocationQueue,
    operation: ZLinkActorSessionRetainedOutbound
  ): boolean {
    if (queue.outboundCount < this.outboundCapacity) {
      queue.outboundCount++;
      return true;
    }
    failRetainedOutbound(
      operation,
      new Error(`Actor '${queue.actorId}' Session relocation outbound capacity was exhausted.`)
    );
    return false;
  }

  private settleOutbound(
    queue: ZLinkActorSessionRelocationQueue,
    entry: ZLinkActorSessionOutboundEntry
  ): void {
    if (entry.settled) return;
    entry.settled = true;
    queue.outboundCount--;
  }

  private failOutbound(
    queue: ZLinkActorSessionRelocationQueue,
    entry: ZLinkActorSessionOutboundEntry,
    error: unknown
  ): void {
    if (entry.settled) return;
    this.settleOutbound(queue, entry);
    failRetainedOutbound(entry.operation, error);
  }

  private outboundAdmissionError(
    claim: ServiceSessionBindingAdmissionClaim
  ): Error | undefined {
    if (
      claim.actorId.length === 0
      || claim.objectGeneration <= 0n
      || claim.actorNodeRid.length === 0
      || claim.actorNodeGeneration <= 0n
      || claim.authorityOwnerGeneration <= 0n
      || claim.ownerLeaseGeneration <= 0n
      || claim.producerNodeRid.length === 0
      || claim.producerNodeGeneration <= 0n
      || claim.bindingGeneration <= 0n
      || claim.sessionIdentity.length === 0
      || !routingIdsEqual(claim.producerNodeRid, claim.actorNodeRid)
      || claim.producerNodeGeneration !== claim.actorNodeGeneration
    ) {
      return new Error(`Actor '${claim.actorId}' Session outbound admission claim is invalid.`);
    }
    const route = this.routes.get(claim.actorId);
    const ref = route?.actor as (TActor & { readonly ref?: {
      readonly actorId?: unknown;
      readonly objectGeneration?: unknown;
      readonly generation?: unknown;
      readonly nodeRid?: unknown;
      readonly bindingGeneration?: unknown;
      readonly ownershipGeneration?: unknown;
      readonly ownerLeaseGeneration?: unknown;
    } }) | undefined;
    const actorRef = ref?.ref;
    const routeSessionIdentity = route?.sessionIdentity;
    if (
      route === undefined
      || actorRef === undefined
      || String(actorRef.actorId ?? claim.actorId) !== claim.actorId
      || BigInt(
        actorRef.objectGeneration as bigint | number | string | boolean | undefined
          ?? actorRef.generation as bigint | number | string | boolean | undefined
          ?? -1
      ) !== claim.objectGeneration
      || BigInt(
        actorRef.bindingGeneration as bigint | number | string | boolean | undefined ?? -1
      ) !== claim.bindingGeneration
      || String(routeSessionIdentity ?? '') !== claim.sessionIdentity
    ) {
      return new Error(`Actor '${claim.actorId}' Session outbound admission was fenced by its current binding.`);
    }
    return undefined;
  }

  private matchesCurrentProducerProof(claim: ServiceSessionBindingAdmissionClaim): boolean {
    const route = this.routes.get(claim.actorId);
    const actorRef = (route?.actor as TActor & { readonly ref?: {
      readonly nodeRid?: unknown;
    } }).ref;
    return route !== undefined
      && actorRef !== undefined
      && routingIdsEqual(String(actorRef.nodeRid ?? ''), claim.actorNodeRid)
      && route.authorityFence?.ownerNodeGeneration === claim.actorNodeGeneration
      && route.authorityFence.authorityOwnerGeneration === claim.authorityOwnerGeneration
      && route.authorityFence.ownerLeaseGeneration === claim.ownerLeaseGeneration;
  }

  private rememberTerminalRelocation(state: ZLinkActorSessionRelocationState): void {
    this.terminalRelocations.delete(state);
    this.terminalRelocations.set(state, true);
    while (this.terminalRelocations.size > this.terminalRelocationCapacity) {
      const oldest = this.terminalRelocations.keys().next().value as
        ZLinkActorSessionRelocationState | undefined;
      if (oldest === undefined) return;
      this.terminalRelocations.delete(oldest);
      const queue = this.relocations.get(oldest.actorId);
      if (queue?.seals.get(oldest.sealId) === oldest) {
        queue.seals.delete(oldest.sealId);
        this.deleteEmptyRelocationQueue(queue);
      }
    }
  }

  private deleteEmptyRelocationQueue(queue: ZLinkActorSessionRelocationQueue): void {
    if (
      queue.activeSealId === undefined
      && queue.seals.size === 0
      && queue.outbound.length === 0
      && queue.drainPromise === undefined
      && this.relocations.get(queue.actorId) === queue
    ) {
      this.relocations.delete(queue.actorId);
    }
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

function assertRelocationClaim(
  state: ZLinkActorSessionRelocationState,
  claim: ZLinkActorSessionRelocationClaim
): void {
  if (
    state.actorGeneration !== claim.actorGeneration
    || state.actorOwnershipGeneration !== claim.actorOwnershipGeneration
    || state.bindingGeneration !== claim.bindingGeneration
    || state.ownerLeaseGeneration !== claim.ownerLeaseGeneration
    || state.sessionIdentity !== claim.sessionIdentity
    || state.actorNodeRid !== claim.actorNodeRid
    || state.actorNodeGeneration !== claim.actorNodeGeneration
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorLocationStale,
      `Actor '${claim.actorId}' repeated Session seal changed its exact identity.`,
      true
    );
  }
}

function matchesRelocationSourceProof(
  state: ZLinkActorSessionRelocationState,
  claim: ServiceSessionBindingAdmissionClaim
): boolean {
  return state.actorId === claim.actorId
    && state.actorGeneration === claim.objectGeneration
    && state.actorOwnershipGeneration === claim.authorityOwnerGeneration
    && state.ownerLeaseGeneration === claim.ownerLeaseGeneration
    && state.bindingGeneration === claim.bindingGeneration
    && state.sessionIdentity === claim.sessionIdentity
    && state.actorNodeRid !== undefined
    && routingIdsEqual(state.actorNodeRid, claim.actorNodeRid)
    && state.actorNodeGeneration === claim.actorNodeGeneration;
}

function matchesRelocationSourceNodeTenure(
  state: ZLinkActorSessionRelocationState,
  claim: ServiceSessionBindingAdmissionClaim
): boolean {
  return state.actorNodeRid !== undefined
    && routingIdsEqual(state.actorNodeRid, claim.actorNodeRid)
    && state.actorNodeGeneration === claim.actorNodeGeneration;
}

function matchesAcceptedProducerProof(
  proof: ZLinkActorSessionAcceptedProducerProof,
  claim: ServiceSessionBindingAdmissionClaim
): boolean {
  return proof.actorId === claim.actorId
    && proof.objectGeneration === claim.objectGeneration
    && routingIdsEqual(proof.actorNodeRid, claim.actorNodeRid)
    && proof.actorNodeGeneration === claim.actorNodeGeneration
    && proof.authorityOwnerGeneration === claim.authorityOwnerGeneration
    && proof.ownerLeaseGeneration === claim.ownerLeaseGeneration
    && proof.sessionIdentity === claim.sessionIdentity
    && proof.bindingGeneration === claim.bindingGeneration;
}

function validAcceptedTargetProof(
  state: ZLinkActorSessionRelocationState,
  proof: ZLinkActorSessionAcceptedProducerProof
): boolean {
  return proof.actorId === state.actorId
    && proof.objectGeneration === state.actorGeneration
    && proof.actorNodeRid.length > 0
    && proof.actorNodeGeneration > 0n
    && proof.authorityOwnerGeneration > state.actorOwnershipGeneration
    && proof.ownerLeaseGeneration > 0n
    && proof.sessionIdentity === state.sessionIdentity
    && proof.bindingGeneration === state.bindingGeneration;
}

function relocationSourceProof(
  state: ZLinkActorSessionRelocationState
): ZLinkActorSessionAcceptedProducerProof | undefined {
  if (
    state.sessionIdentity === undefined
    || state.actorNodeRid === undefined
    || state.actorNodeGeneration === undefined
  ) return undefined;
  return {
    actorId: state.actorId,
    objectGeneration: state.actorGeneration,
    actorNodeRid: state.actorNodeRid,
    actorNodeGeneration: state.actorNodeGeneration,
    authorityOwnerGeneration: state.actorOwnershipGeneration,
    ownerLeaseGeneration: state.ownerLeaseGeneration,
    sessionIdentity: state.sessionIdentity,
    bindingGeneration: state.bindingGeneration
  };
}

function failRetainedOutbound(
  operation: ZLinkActorSessionRetainedOutbound,
  error: unknown
): void {
  try {
    operation.fail(error);
  } catch {
    // The aggregate has already removed the entry. A failure observer cannot
    // retain or settle the same delivery a second time.
  }
}

function assertSuccessorSessionIdentity(
  predecessor: ZLinkActorSessionRelocationState,
  successor: ZLinkActorSessionRelocationClaim
): void {
  if (
    predecessor.actorGeneration !== successor.actorGeneration
    || predecessor.bindingGeneration !== successor.bindingGeneration
    || predecessor.sessionIdentity !== successor.sessionIdentity
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorLocationStale,
      `Actor '${successor.actorId}' successor Session seal changed its exact Session identity.`,
      true
    );
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

function sessionIdentityFromContext<
  TActor extends ZLinkActorSessionBindingActor,
  TContext extends ZLinkActorSessionBindingContext<TActor>
>(context: TContext): string | undefined {
  return context.routingId === undefined ? undefined : String(context.routingId);
}
