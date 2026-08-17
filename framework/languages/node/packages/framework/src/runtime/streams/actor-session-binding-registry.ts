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
  readonly activeFrames: ZLinkActorSessionActiveFrames;
  sealId?: string;
  authorityFence?: ZLinkActorSessionAuthorityFence;
}

export interface ZLinkActorSessionRelocationClaim {
  readonly actorId: string;
  readonly actorGeneration: bigint;
  readonly bindingGeneration: bigint;
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
  readonly sessionIdentity: string;
  readonly bindingGeneration: bigint;
}

export interface ZLinkActorSessionRelocationSnapshot extends ZLinkActorSessionRelocationClaim {
  readonly phase: 'sealed' | 'applying' | 'applied' | 'terminal';
  readonly applyFingerprint?: string;
}

interface ZLinkActorSessionFrameAdmission {
  complete(): void;
}

interface ZLinkActorSessionRequestFrameAdmission extends ZLinkActorSessionFrameAdmission {
  /**
   * Claims the one allowed submission attempt. A request captured by a seal
   * before this call waits for that exact seal to publish or abort its route;
   * a request captured after this call only observes its original terminal.
   */
  beginSubmission(signal?: AbortSignal): Promise<void> | undefined;
}

interface ZLinkActorSessionActiveFrames {
  count: number;
  readonly requests: Set<{
    captureBySeal(): void;
  }>;
}

interface ZLinkActorSessionActiveFrameWaiter {
  readonly resolve: () => void;
  readonly reject: (error: unknown) => void;
}

interface ZLinkActorSessionRelocationState {
  readonly actorId: string;
  readonly actorGeneration: bigint;
  readonly bindingGeneration: bigint;
  readonly sessionIdentity?: string;
  readonly actorNodeRid?: string;
  readonly actorNodeGeneration?: bigint;
  readonly sealId: string;
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
    private readonly outboundCapacity = 4096,
    //  Spec 06 — SessionRelocationSealTimeout bounds how long a session
    //  relocation seal may hold ingress (default 3,000 ms, finite only).
    //  Every wait on the seal (accept paths) and on active-frame drain
    //  (the sealing side) must observe that bound; an unbounded wait here
    //  turns a lost control or a lifecycle interlock into a silent stall
    //  (spec 48:205 — transport/deadline limits apply during relocation).
    private readonly sealWaitTimeoutMs = 3_000
  ) {
    if (!Number.isSafeInteger(terminalRelocationCapacity) || terminalRelocationCapacity <= 0) {
      throw new RangeError('Session relocation terminal capacity must be a positive safe integer.');
    }
    if (!Number.isSafeInteger(outboundCapacity) || outboundCapacity <= 0) {
      throw new RangeError('Session relocation outbound capacity must be a positive safe integer.');
    }
    if (!Number.isSafeInteger(sealWaitTimeoutMs) || sealWaitTimeoutMs <= 0) {
      throw new RangeError('Session relocation seal timeout must be a positive safe integer.');
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
      activeFrames: { count: 0, requests: new Set() },
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
    authorityFence?: ZLinkActorSessionAuthorityFence,
    sessionIdentity?: string
  ): void {
    if (previous.sealId !== sealId) {
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

  accept(actorId: string, bindingToken: string): void {
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
  ): Promise<void> {
    for (;;) {
      throwIfAborted(signal);
      const route = this.requireRoute(actorId);
      this.requireCurrentToken(actorId, bindingToken);
      if (route.sealId === undefined) {
        try {
          this.accept(actorId, bindingToken);
          return;
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
        route.activeFrames.count++;
        let completed = false;
        return {
          complete: () => {
            if (completed) return;
            completed = true;
            route.activeFrames.count--;
            if (
              route.activeFrames.count === 0
              && this.routes.get(actorId)?.activeFrames === route.activeFrames
            ) {
              this.resolveActiveFrameWaiters(actorId);
            }
          }
        };
      }
      await this.waitForSealRelease(actorId, bindingToken, signal);
    }
  }

  async beginAcceptedRequestFrameWhenReady(
    actorId: string,
    bindingToken: string,
    signal?: AbortSignal
  ): Promise<ZLinkActorSessionRequestFrameAdmission> {
    const frame = await this.beginAcceptedFrameWhenReady(actorId, bindingToken, signal);
    const activeFrames = this.requireRoute(actorId).activeFrames;
    let submissionStarted = false;
    let capturedBySeal = false;
    let finished = false;
    const request = {
      captureBySeal: () => {
        if (finished || capturedBySeal) return;
        capturedBySeal = true;
        // Spec 20:389-396,408-412 — command 42 captures the Session-side
        // frame, while the request correlation and its detached terminal
        // remain alive. This removes the 42 -> request terminal -> 44 -> 42
        // cycle without turning the capture into another submission.
        frame.complete();
      }
    };
    activeFrames.requests.add(request);
    return {
      beginSubmission: (submissionSignal) => {
        if (submissionStarted) return undefined;
        // Claim before the asynchronous seal wait. A second continuation
        // must not observe an unclaimed request while the first is waiting
        // for command 44 to publish the route.
        submissionStarted = true;
        if (!capturedBySeal) {
          // From here the request may already have reached the remote peer.
          // Spec 32:153-157 forbids a second logical-target submission.
          return undefined;
        }
        return this.acceptWhenReady(actorId, bindingToken, submissionSignal);
      },
      complete: () => {
        if (finished) return;
        finished = true;
        activeFrames.requests.delete(request);
        frame.complete();
      }
    };
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

  seal(actorId: string, sealId: string, expected: ZLinkActorSessionRouteFence): void {
    const route = this.requireRoute(actorId);
    if (route.sealId !== undefined) {
      if (route.sealId === sealId && routeMatchesFence(route, expected)) {
        return;
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
    // A pre-seal REQUEST is captured as part of installing command 42. Its
    // Session frame no longer gates the seal; its single submission attempt
    // and terminal continue under the request admission state above.
    for (const request of route.activeFrames.requests) {
      request.captureBySeal();
    }
  }

  async sealAndWait(
    actorId: string,
    sealId: string,
    expected: ZLinkActorSessionRouteFence,
    signal?: AbortSignal
  ): Promise<void> {
    this.seal(actorId, sealId, expected);
    await this.waitForActiveFrames(actorId, signal);
  }

  async sealRelocation(
    claim: ZLinkActorSessionRelocationClaim,
    expected: ZLinkActorSessionRouteFence,
    signal?: AbortSignal
  ): Promise<void> {
    for (;;) {
      throwIfAborted(signal);
      const queue = this.relocations.get(claim.actorId);
      const existing = queue?.seals.get(claim.sealId);
      if (existing !== undefined) {
        assertRelocationClaim(existing, claim);
        await waitForSessionRelocation(existing.ready, signal);
        return;
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
        await waitForSessionRelocation(predecessor.terminal, signal);
        continue;
      }

      this.seal(claim.actorId, claim.sealId, expected);
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
        phase: 'sealed',
        ready,
        terminal,
        resolveTerminal
      };
      actorQueue.seals.set(claim.sealId, state);
      actorQueue.activeSealId = claim.sealId;
      this.relocations.set(claim.actorId, actorQueue);
      await ready;
      return;
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
      if (!this.matchesCurrentProducerNode(claim)) {
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
      const matchesCurrent = this.matchesCurrentProducerNode(claim);
      const authorization = matchesSource || matchesCurrent
        ? 'source'
        : 'pendingTarget';
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

  async applyRelocation(
    actorId: string,
    sealId: string,
    applyFingerprint: string,
    action: 'commit' | 'abort',
    commitOwnerTransition: () => Promise<void>,
    acceptedProducerProof?: ZLinkActorSessionAcceptedProducerProof
  ): Promise<void> {
    const queue = this.relocations.get(actorId);
    const state = queue?.seals.get(sealId);
    if (queue === undefined || state === undefined) {
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
      readonly ownerNodeGeneration?: unknown;
      readonly bindingGeneration?: unknown;
    } }).ref;
    const sessionIdentity = route?.sessionIdentity;
    if (
      route === undefined
      || actorRef === undefined
      || sessionIdentity === undefined
      || actorRef.ownerNodeGeneration === undefined
    ) return undefined;
    return {
      actorId,
      objectGeneration: BigInt(
        actorRef.objectGeneration as bigint | number | string | boolean | undefined
          ?? actorRef.generation as bigint | number | string | boolean | undefined
          ?? -1
      ),
      actorNodeRid: String(actorRef.nodeRid ?? ''),
      actorNodeGeneration: BigInt(
        actorRef.ownerNodeGeneration as bigint | number | string | boolean
      ),
      sessionIdentity: String(sessionIdentity),
      bindingGeneration: BigInt(
        actorRef.bindingGeneration as bigint | number | string | boolean | undefined ?? -1
      )
    };
  }

  observeRelocationTerminal(
    actorId: string,
    sealId: string,
    applyFingerprint: string
  ): void {
    const queue = this.relocations.get(actorId);
    const state = queue?.seals.get(sealId);
    if (
      queue === undefined
      || state === undefined
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

  validateSeal(actorId: string, sealId: string): boolean {
    const route = this.routes.get(actorId);
    return route !== undefined
      && route.sealId === sealId;
  }

  /** True while a relocation seal currently holds this actor's ingress. */
  isSealed(actorId: string): boolean {
    return this.routes.get(actorId)?.sealId !== undefined;
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
      let timer: ReturnType<typeof setTimeout> | undefined;
      const cleanup = () => {
        if (timer !== undefined) clearTimeout(timer);
        signal?.removeEventListener('abort', onAbort);
      };
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
      const removeWaiter = () => {
        waiters.delete(waiter);
        if (waiters.size === 0) this.sealWaiters.delete(actorId);
      };
      timer = setTimeout(() => {
        removeWaiter();
        rejectWaiter(createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          `Actor '${actorId}' session seal was not released within the relocation seal timeout.`,
          true
        ));
      }, this.sealWaitTimeoutMs);
      timer.unref?.();
      const onAbort = () => {
        removeWaiter();
        rejectWaiter(signal?.reason ?? createAbortError());
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
    if (route.activeFrames.count === 0) return;
    await new Promise<void>((resolve, reject) => {
      const waiters = this.activeFrameWaiters.get(actorId) ?? new Set();
      let settled = false;
      let timer: ReturnType<typeof setTimeout> | undefined;
      const cleanup = () => {
        if (timer !== undefined) clearTimeout(timer);
        signal?.removeEventListener('abort', onAbort);
      };
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
        waiter.reject(signal?.reason ?? createAbortError());
      };
      waiters.add(waiter);
      this.activeFrameWaiters.set(actorId, waiters);
      //  Spec 48:205 — transport/deadline limits keep applying during
      //  relocation: the sealing side must not wait forever for active
      //  frames that themselves may be waiting on this relocation.
      timer = setTimeout(() => {
        waiters.delete(waiter);
        if (waiters.size === 0) this.activeFrameWaiters.delete(actorId);
        waiter.reject(createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          `Actor '${actorId}' session seal timed out waiting for active frames.`,
          true
        ));
      }, this.sealWaitTimeoutMs);
      timer.unref?.();
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
      readonly bindingGeneration?: unknown;
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

  private matchesCurrentProducerNode(claim: ServiceSessionBindingAdmissionClaim): boolean {
    const route = this.routes.get(claim.actorId);
    const actorRef = (route?.actor as TActor & { readonly ref?: {
      readonly nodeRid?: unknown;
    } }).ref;
    return route !== undefined
      && actorRef !== undefined
      && routingIdsEqual(String(actorRef.nodeRid ?? ''), claim.actorNodeRid);
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
    || state.bindingGeneration !== claim.bindingGeneration
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
    && state.bindingGeneration === claim.bindingGeneration
    && state.sessionIdentity === claim.sessionIdentity
    && state.actorNodeRid !== undefined
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
  readonly bindingGeneration: bigint;
}

function routeMatchesFence<
  TContext extends ZLinkActorSessionBindingContext<TActor>,
  TActor extends ZLinkActorSessionBindingActor
>(route: ZLinkActorSessionRoute<TContext, TActor>, expected: ZLinkActorSessionRouteFence): boolean {
  const ref = (route.actor as TActor & { readonly ref?: unknown }).ref as {
    readonly objectGeneration?: bigint;
    readonly generation?: bigint;
    readonly bindingGeneration?: bigint;
  } | undefined;
  return ref !== undefined
    && BigInt(ref.objectGeneration ?? ref.generation ?? -1n) === expected.objectGeneration
    && ref.bindingGeneration === expected.bindingGeneration;
}

function sessionIdentityFromContext<
  TActor extends ZLinkActorSessionBindingActor,
  TContext extends ZLinkActorSessionBindingContext<TActor>
>(context: TContext): string | undefined {
  return context.routingId === undefined ? undefined : String(context.routingId);
}

function waitForSessionRelocation(
  operation: Promise<void>,
  signal?: AbortSignal
): Promise<void> {
  if (signal === undefined) return operation;
  if (signal.aborted) return Promise.reject(signal.reason ?? createAbortError());
  return new Promise<void>((resolve, reject) => {
    const onAbort = () => {
      signal.removeEventListener('abort', onAbort);
      reject(signal.reason ?? createAbortError());
    };
    signal.addEventListener('abort', onAbort, { once: true });
    operation.then(
      () => {
        signal.removeEventListener('abort', onAbort);
        resolve();
      },
      error => {
        signal.removeEventListener('abort', onAbort);
        reject(error);
      }
    );
  });
}
