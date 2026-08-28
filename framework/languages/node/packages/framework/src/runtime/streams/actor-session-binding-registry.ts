import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { createAbortError, throwIfAborted } from '../abort';
import { AsyncResource } from 'node:async_hooks';
import { ZLinkStateLane } from '../execution/state-lane';
import { routingIdsEqual } from '../routing-id';
import type {
  ServiceSessionBindingAdmissionClaim,
  ServiceSessionBindingAdmissionResult
} from '../foundation/service-session-binding-ingress-port';

const detachedStateLaneResource = new AsyncResource('zlink:actor-session-binding-registry');
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
  complete(): Promise<void>;
  completeCore(): void;
}

interface ZLinkActorSessionRequestFrameAdmission {
  complete(): Promise<void>;
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
  private readonly lane = new ZLinkStateLane();
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

  async bind(
    context: TContext,
    actor: TActor,
    bindingToken: string,
    authorityFence?: ZLinkActorSessionAuthorityFence,
    sessionIdentity?: string
  ): Promise<void> {
    await this.lane.run(() => this.bindCore(context, actor, bindingToken, authorityFence, sessionIdentity));
  }

  private bindCore(
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

  async replace(
    previous: ZLinkActorSessionRoute<TContext, TActor>,
    context: TContext,
    actor: TActor,
    bindingToken: string,
    authorityFence?: ZLinkActorSessionAuthorityFence,
    sessionIdentity?: string
  ): Promise<void> {
    await this.lane.run(() => this.replaceCore(
      previous, context, actor, bindingToken, authorityFence, sessionIdentity
    ));
  }

  private replaceCore(
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

  async replaceAndReleaseSeal(
    previous: ZLinkActorSessionRoute<TContext, TActor>,
    context: TContext,
    actor: TActor,
    bindingToken: string,
    sealId: string,
    authorityFence?: ZLinkActorSessionAuthorityFence,
    sessionIdentity?: string
  ): Promise<void> {
    await this.lane.run(() => this.replaceAndReleaseSealCore(
      previous, context, actor, bindingToken, sealId, authorityFence, sessionIdentity
    ));
  }

  private replaceAndReleaseSealCore(
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
    this.replaceCore(previous, context, actor, bindingToken, authorityFence, sessionIdentity);
    if (!this.abortSealCore(actor.actorId, sealId)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${actor.actorId}' route switch lost its relocation seal.`,
        true
      );
    }
  }

  async find(actorId: string): Promise<TActor | undefined> {
    return await this.lane.run(() => this.findCore(actorId));
  }

  private findCore(actorId: string): TActor | undefined {
    return this.routes.get(actorId)?.actor;
  }

  async route(actorId: string): Promise<ZLinkActorSessionRoute<TContext, TActor> | undefined> {
    return await this.lane.run(() => this.routeCore(actorId));
  }

  private routeCore(actorId: string): ZLinkActorSessionRoute<TContext, TActor> | undefined {
    return this.routes.get(actorId);
  }

  async capturePendingReplyClaim(
    actorId: string,
    actor: ZLinkActorSessionBindingActor,
    bindingToken: string
  ): Promise<{ readonly context: TContext } | undefined> {
    return await this.lane.run(() => this.capturePendingReplyClaimCore(actorId, actor, bindingToken));
  }

  private capturePendingReplyClaimCore(
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

  async unbind(actorId: string, context: TContext, bindingToken: string): Promise<void> {
    await this.lane.run(() => this.unbindCore(actorId, context, bindingToken));
  }

  private unbindCore(actorId: string, context: TContext, bindingToken: string): void {
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
    this.clearRouteRelocation(
      actorId,
      route,
      new Error(`Actor '${actorId}' session binding was removed.`)
    );
  }

  async unbindActor(actorId: string): Promise<void> {
    await this.lane.run(() => this.unbindActorCore(actorId));
  }

  private unbindActorCore(actorId: string): void {
    const route = this.routes.get(actorId);
    if (route === undefined) {
      return;
    }
    this.unbindCore(actorId, route.context, route.bindingToken);
  }

  async cleanup(context: TContext): Promise<void> {
    await this.lane.run(() => this.cleanupCore(context));
  }

  private cleanupCore(context: TContext): void {
    for (const route of [...this.routes.values()]) {
      if (route.context === context) {
        this.unbindCore(route.actor.actorId, context, route.bindingToken);
      }
    }
  }

  async requireRoute(actorId: string): Promise<ZLinkActorSessionRoute<TContext, TActor>> {
    return await this.lane.run(() => this.requireRouteCore(actorId));
  }

  private requireRouteCore(actorId: string): ZLinkActorSessionRoute<TContext, TActor> {
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

  async requireCurrentToken(actorId: string, bindingToken: string): Promise<void> {
    await this.lane.run(() => this.requireCurrentTokenCore(actorId, bindingToken));
  }

  private requireCurrentTokenCore(actorId: string, bindingToken: string): void {
    const route = this.requireRouteCore(actorId);
    if (route.bindingToken === bindingToken) {
      return;
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
      `Actor '${actorId}' session binding is stale.`,
      true
    );
  }

  async accept(actorId: string, bindingToken: string): Promise<void> {
    await this.lane.run(() => this.acceptCore(actorId, bindingToken));
  }

  private acceptCore(actorId: string, bindingToken: string): void {
    const route = this.requireRouteCore(actorId);
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
      const accepted = await this.lane.run(() => this.acceptWhenReadyCore(actorId, bindingToken));
      if (accepted) return;
      await this.waitForSealRelease(actorId, bindingToken, signal);
    }
  }

  private acceptWhenReadyCore(actorId: string, bindingToken: string): boolean {
    const route = this.requireRouteCore(actorId);
    this.requireCurrentTokenCore(actorId, bindingToken);
    if (route.sealId === undefined) {
      try {
        this.acceptCore(actorId, bindingToken);
        return true;
      } catch (error) {
        // A new seal can race the check above. Re-enter the wait only for
        // that relocation fence; unrelated binding failures stay visible.
        if (this.routes.get(actorId)?.sealId === undefined) throw error;
      }
    }
    return false;
  }

  async beginAcceptedFrameWhenReady(
    actorId: string,
    bindingToken: string,
    signal?: AbortSignal
  ): Promise<ZLinkActorSessionFrameAdmission> {
    for (;;) {
      throwIfAborted(signal);
      const admission = await this.lane.run(() => this.beginAcceptedFrameCore(actorId, bindingToken));
      if (admission !== undefined) return admission;
      await this.waitForSealRelease(actorId, bindingToken, signal);
    }
  }

  private beginAcceptedFrameCore(
    actorId: string,
    bindingToken: string
  ): ZLinkActorSessionFrameAdmission | undefined {
    const route = this.requireRouteCore(actorId);
    this.requireCurrentTokenCore(actorId, bindingToken);
    if (route.sealId !== undefined) return undefined;
    route.activeFrames.count++;
    let completed = false;
    const completeCore = () => {
      if (completed) return;
      completed = true;
      route.activeFrames.count--;
      if (
        route.activeFrames.count === 0
        && this.routes.get(actorId)?.activeFrames === route.activeFrames
      ) {
        this.resolveActiveFrameWaiters(actorId);
      }
    };
    return {
      complete: async () => {
        await this.lane.run(completeCore);
      },
      completeCore
    };
  }

  async beginAcceptedRequestFrameWhenReady(
    actorId: string,
    bindingToken: string,
    signal?: AbortSignal
  ): Promise<ZLinkActorSessionRequestFrameAdmission> {
    const frame = await this.beginAcceptedFrameWhenReady(actorId, bindingToken, signal);
    return await this.lane.run(() => this.beginAcceptedRequestFrameCore(actorId, bindingToken, frame));
  }

  private beginAcceptedRequestFrameCore(
    actorId: string,
    bindingToken: string,
    frame: ZLinkActorSessionFrameAdmission
  ): ZLinkActorSessionRequestFrameAdmission {
    const activeFrames = this.requireRouteCore(actorId).activeFrames;
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
        frame.completeCore();
      }
    };
    activeFrames.requests.add(request);
    return {
      beginSubmission: async (submissionSignal) => {
        const captured = await this.lane.run(() => {
          if (submissionStarted) return false;
          // Claim before the asynchronous seal wait. A second continuation
          // must not observe an unclaimed request while the first is waiting
          // for command 44 to publish the route.
          submissionStarted = true;
          return capturedBySeal;
        });
        if (!captured) return;
        await this.acceptWhenReady(actorId, bindingToken, submissionSignal);
      },
      complete: async () => {
        await this.lane.run(() => {
          if (finished) return;
          finished = true;
          activeFrames.requests.delete(request);
          frame.completeCore();
        });
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
      await admission.complete();
    }
  }

  async seal(actorId: string, sealId: string, expected: ZLinkActorSessionRouteFence): Promise<void> {
    await this.lane.run(() => this.sealCore(actorId, sealId, expected));
  }

  private sealCore(actorId: string, sealId: string, expected: ZLinkActorSessionRouteFence): void {
    const route = this.requireRouteCore(actorId);
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
    await this.lane.run(() => this.sealCore(actorId, sealId, expected));
    await this.waitForActiveFrames(actorId, signal);
  }

  async sealRelocation(
    claim: ZLinkActorSessionRelocationClaim,
    expected: ZLinkActorSessionRouteFence,
    signal?: AbortSignal
  ): Promise<void> {
    for (;;) {
      throwIfAborted(signal);
      const step = await this.lane.run(() => this.prepareRelocationSeal(claim, expected, signal));
      if (step.kind === 'existing') {
        await waitForSessionRelocation(step.wait, signal);
        return;
      }
      if (step.kind === 'predecessor') {
        await waitForSessionRelocation(step.wait, signal);
        continue;
      }
      await step.wait;
      return;
    }
  }

  private prepareRelocationSeal(
    claim: ZLinkActorSessionRelocationClaim,
    expected: ZLinkActorSessionRouteFence,
    signal?: AbortSignal
  ):
    | { readonly kind: 'existing' | 'predecessor'; readonly wait: Promise<void> }
    | { readonly kind: 'new'; readonly wait: Promise<void> } {
    const queue = this.relocations.get(claim.actorId);
    const existing = queue?.seals.get(claim.sealId);
    if (existing !== undefined) {
      assertRelocationClaim(existing, claim);
      return { kind: 'existing', wait: existing.ready };
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
      return { kind: 'predecessor', wait: predecessor.terminal };
    }

    this.sealCore(claim.actorId, claim.sealId, expected);
    let resolveTerminal!: () => void;
    const terminal = new Promise<void>((resolve) => { resolveTerminal = resolve; });
    const ready = this.waitForActiveFramesCore(claim.actorId, signal);
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
    return { kind: 'new', wait: ready };
  }

  async relocationSnapshot(
    actorId: string,
    sealId: string
  ): Promise<ZLinkActorSessionRelocationSnapshot | undefined> {
    return await this.lane.run(() => this.relocationSnapshotCore(actorId, sealId));
  }

  private relocationSnapshotCore(
    actorId: string,
    sealId: string
  ): ZLinkActorSessionRelocationSnapshot | undefined {
    return this.relocations.get(actorId)?.seals.get(sealId);
  }

  async retainRelocationOutbound(
    actorId: string,
    operation: ZLinkActorSessionRetainedOutbound,
    sealId?: string
  ): Promise<ServiceSessionBindingAdmissionResult> {
    return await this.lane.run(() => this.retainRelocationOutboundCorePublic(actorId, operation, sealId));
  }

  private retainRelocationOutboundCorePublic(
    actorId: string,
    operation: ZLinkActorSessionRetainedOutbound,
    sealId?: string
  ): ServiceSessionBindingAdmissionResult {
    if (sealId !== undefined) {
      const queue = this.relocations.get(actorId);
      const state = queue?.seals.get(sealId);
      if (
        queue === undefined
        || state === undefined
        || (queue.activeSealId !== sealId
          && !(queue.activeSealId === undefined && state.phase === 'terminal'))
      ) {
        failRetainedOutbound(
          operation,
          new Error(`Actor '${actorId}' Session outbound did not match its exact relocation seal.`)
        );
        return 'rejected';
      }
    }
    return this.retainRelocationOutboundCore(actorId, operation, 'legacy');
  }

  async admitRelocationOutbound(
    claim: ServiceSessionBindingAdmissionClaim,
    operation: ZLinkActorSessionRetainedOutbound
  ): Promise<ServiceSessionBindingAdmissionResult> {
    return await this.lane.run(() => this.admitRelocationOutboundCore(claim, operation));
  }

  private admitRelocationOutboundCore(
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

  async discardRelocationOutbound(actorId: string, sealId: string, error: unknown): Promise<void> {
    await this.lane.run(() => this.discardRelocationOutboundCore(actorId, sealId, error));
  }

  private discardRelocationOutboundCore(actorId: string, sealId: string, error: unknown): void {
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
    const prepared = await this.lane.run(() => this.prepareRelocationApply(
      actorId,
      sealId,
      applyFingerprint,
      action,
      acceptedProducerProof
    ));
    if (prepared.applyPromise !== undefined) {
      await prepared.applyPromise;
      return;
    }
    startOutsideStateLane(() => {
      void (async () => {
        try {
          await commitOwnerTransition();
          await this.lane.run(() => this.completeRelocationApply(prepared));
          prepared.resolve!();
        } catch (error) {
          await this.lane.run(() => this.failRelocationApply(prepared));
          prepared.reject!(error);
        }
      })();
    });
    await prepared.promise!;
  }

  private prepareRelocationApply(
    actorId: string,
    sealId: string,
    applyFingerprint: string,
    action: 'commit' | 'abort',
    acceptedProducerProof?: ZLinkActorSessionAcceptedProducerProof
  ): {
    readonly queue: ZLinkActorSessionRelocationQueue;
    readonly state: ZLinkActorSessionRelocationState;
    readonly applyPromise?: Promise<void>;
    readonly action?: 'commit' | 'abort';
    readonly acceptedProducerProof?: ZLinkActorSessionAcceptedProducerProof;
    readonly promise?: Promise<void>;
    readonly resolve?: () => void;
    readonly reject?: (error: unknown) => void;
    readonly actorId?: string;
    readonly sealId?: string;
  } {
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
      return { queue, state, applyPromise: state.applyPromise };
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
    let resolve!: () => void;
    let reject!: (error: unknown) => void;
    const promise = new Promise<void>((complete, fail) => {
      resolve = complete;
      reject = fail;
    });
    state.applyPromise = promise;
    return {
      queue,
      state,
      action,
      acceptedProducerProof,
      promise,
      resolve,
      reject,
      actorId,
      sealId
    };
  }

  private completeRelocationApply(prepared: {
    readonly queue: ZLinkActorSessionRelocationQueue;
    readonly state: ZLinkActorSessionRelocationState;
    readonly action?: 'commit' | 'abort';
    readonly acceptedProducerProof?: ZLinkActorSessionAcceptedProducerProof;
    readonly actorId?: string;
    readonly sealId?: string;
  }): void {
    const { queue, state, action, acceptedProducerProof, actorId, sealId } = prepared;
    const proof = action === 'commit'
      ? acceptedProducerProof ?? this.currentProducerProof(actorId!)
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
  }

  private failRelocationApply(prepared: {
    readonly state: ZLinkActorSessionRelocationState;
  }): void {
    prepared.state.phase = 'sealed';
    prepared.state.applyFingerprint = undefined;
    prepared.state.applyPromise = undefined;
    prepared.state.acceptedProducerProof = undefined;
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

  async observeRelocationTerminal(
    actorId: string,
    sealId: string,
    applyFingerprint: string
  ): Promise<void> {
    await this.lane.run(() => this.observeRelocationTerminalCore(actorId, sealId, applyFingerprint));
  }

  private observeRelocationTerminalCore(
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

  /**
   * Actor-wide relocation teardown: every seal (active and terminal-retained)
   * and every held outbound entry for `actorId` is discarded unconditionally.
   * Reserved for paths where the Actor itself is gone or being torn down
   * (destroy, ownership clear, relocation-seal expiry/failure) — never for a
   * single physical route's disconnect, which must not disturb a relocation
   * seal it does not own (see `clearRouteRelocation`).
   */
  async clearRelocation(actorId: string, error: unknown): Promise<void> {
    await this.lane.run(() => this.clearRelocationCore(actorId, error));
  }

  private clearRelocationCore(actorId: string, error: unknown): void {
    const queue = this.relocations.get(actorId);
    if (queue === undefined) return;
    this.relocations.delete(actorId);
    for (const entry of queue.outbound.splice(0)) this.failOutbound(queue, entry, error);
    for (const state of queue.seals.values()) {
      this.terminalRelocations.delete(state);
      if (state.phase !== 'terminal') state.resolveTerminal();
    }
  }

  /**
   * Spec 48 §137: a relocation seal's lifecycle is independent of any single
   * physical route's binding. Unbinding one physical route (a disconnect)
   * must retire only the still-open (nonterminal) relocation seal that this
   * exact route's Session owns, and the outbound held against that seal — it
   * must never reach into a seal that has already reached 'terminal' (its
   * bounded retention exists precisely so a late same-seal relay can still
   * pass on a successor route) nor into a different Session's active seal
   * (a concurrent successor that already re-sealed before this stale route
   * noticed the disconnect).
   */
  private clearRouteRelocation(
    actorId: string,
    route: ZLinkActorSessionRoute<TContext, TActor>,
    error: unknown
  ): void {
    const queue = this.relocations.get(actorId);
    if (queue === undefined) return;
    const activeSealId = queue.activeSealId;
    const activeState = activeSealId === undefined ? undefined : queue.seals.get(activeSealId);
    const ownsActiveSeal = activeState !== undefined
      && activeState.phase !== 'terminal'
      && activeState.sessionIdentity === route.sessionIdentity;
    if (!ownsActiveSeal) {
      // Nothing this exact route still holds open — leave terminal
      // retention and any other Session's active seal untouched.
      this.deleteEmptyRelocationQueue(queue);
      return;
    }
    for (let index = queue.outbound.length - 1; index >= 0; index--) {
      const entry = queue.outbound[index]!;
      if (entry.sealId !== activeSealId) continue;
      queue.outbound.splice(index, 1);
      this.failOutbound(queue, entry, error);
    }
    queue.activeSealId = undefined;
    queue.seals.delete(activeSealId!);
    this.terminalRelocations.delete(activeState);
    activeState.resolveTerminal();
    this.deleteEmptyRelocationQueue(queue);
  }

  async abortSeal(actorId: string, sealId: string): Promise<boolean> {
    return await this.lane.run(() => this.abortSealCore(actorId, sealId));
  }

  private abortSealCore(actorId: string, sealId: string): boolean {
    const route = this.routes.get(actorId);
    if (route === undefined || route.sealId !== sealId) return false;
    route.sealId = undefined;
    this.resolveSealWaiters(actorId, sealId);
    return true;
  }

  async updateAuthorityFence(
    actorId: string,
    authorityFence: ZLinkActorSessionAuthorityFence
  ): Promise<void> {
    await this.lane.run(() => this.updateAuthorityFenceCore(actorId, authorityFence));
  }

  private updateAuthorityFenceCore(actorId: string, authorityFence: ZLinkActorSessionAuthorityFence): void {
    const route = this.requireRouteCore(actorId);
    route.authorityFence = authorityFence;
  }

  async validateSeal(actorId: string, sealId: string): Promise<boolean> {
    return await this.lane.run(() => this.validateSealCore(actorId, sealId));
  }

  private validateSealCore(actorId: string, sealId: string): boolean {
    const route = this.routes.get(actorId);
    return route !== undefined
      && route.sealId === sealId;
  }

  /** True while a relocation seal currently holds this actor's ingress. */
  async isSealed(actorId: string): Promise<boolean> {
    return await this.lane.run(() => this.isSealedCore(actorId));
  }

  private isSealedCore(actorId: string): boolean {
    return this.routes.get(actorId)?.sealId !== undefined;
  }

  private async waitForSealRelease(
    actorId: string,
    bindingToken: string,
    signal?: AbortSignal
  ): Promise<void> {
    const prepared = await this.lane.run(() => ({
      wait: this.waitForSealReleaseCore(actorId, bindingToken, signal)
    }));
    await prepared.wait;
  }

  private waitForSealReleaseCore(
    actorId: string,
    bindingToken: string,
    signal?: AbortSignal
  ): Promise<void> {
    const route = this.requireRouteCore(actorId);
    this.requireCurrentTokenCore(actorId, bindingToken);
    if (route.sealId === undefined) return Promise.resolve();
    return new Promise<void>((resolve, reject) => {
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
      timer = startOutsideStateLane(() => setTimeout(() => {
        this.lane.tryPost(() => {
          removeWaiter();
          rejectWaiter(createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            `Actor '${actorId}' session seal was not released within the relocation seal timeout.`,
            true
          ));
        });
      }, this.sealWaitTimeoutMs));
      timer.unref();
      const onAbort = () => {
        this.lane.tryPost(() => {
          removeWaiter();
          rejectWaiter(signal?.reason ?? createAbortError());
        });
      };
      if (signal === undefined) return;
      if (signal.aborted) {
        onAbort();
        return;
      }
      startOutsideStateLane(() => signal.addEventListener('abort', onAbort, { once: true }));
    });
  }

  private async waitForActiveFrames(actorId: string, signal?: AbortSignal): Promise<void> {
    const prepared = await this.lane.run(() => ({
      wait: this.waitForActiveFramesCore(actorId, signal)
    }));
    await prepared.wait;
  }

  private waitForActiveFramesCore(actorId: string, signal?: AbortSignal): Promise<void> {
    const route = this.requireRouteCore(actorId);
    if (route.activeFrames.count === 0) return Promise.resolve();
    return new Promise<void>((resolve, reject) => {
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
        this.lane.tryPost(() => {
          waiters.delete(waiter);
          if (waiters.size === 0) this.activeFrameWaiters.delete(actorId);
          waiter.reject(signal?.reason ?? createAbortError());
        });
      };
      waiters.add(waiter);
      this.activeFrameWaiters.set(actorId, waiters);
      //  Spec 48:205 — transport/deadline limits keep applying during
      //  relocation: the sealing side must not wait forever for active
      //  frames that themselves may be waiting on this relocation.
      timer = startOutsideStateLane(() => setTimeout(() => {
        this.lane.tryPost(() => {
          waiters.delete(waiter);
          if (waiters.size === 0) this.activeFrameWaiters.delete(actorId);
          waiter.reject(createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            `Actor '${actorId}' session seal timed out waiting for active frames.`,
            true
          ));
        });
      }, this.sealWaitTimeoutMs));
      timer.unref();
      if (signal === undefined) return;
      if (signal.aborted) onAbort();
      else startOutsideStateLane(() => signal.addEventListener('abort', onAbort, { once: true }));
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
    const drain = startOutsideStateLane(async () => {
      for (;;) {
        const entry = await this.lane.run(() => {
          const current = queue.outbound.at(0);
          if (current === undefined || !current.released) return undefined;
          if (current.settled) {
            queue.outbound.shift();
            return null;
          }
          return current;
        });
        if (entry === undefined) return;
        if (entry === null) continue;
        try {
          const delivered = await entry.operation.deliver();
          await this.lane.run(() => {
            if (delivered) this.settleOutbound(queue, entry);
            else this.failOutbound(
              queue,
              entry,
              new Error(`Actor '${queue.actorId}' retained Session outbound delivery was rejected.`)
            );
            if (queue.outbound[0] === entry) queue.outbound.shift();
          });
        } catch (error) {
          await this.lane.run(() => {
            this.failOutbound(queue, entry, error);
            if (queue.outbound[0] === entry) queue.outbound.shift();
          });
        }
      }
    });
    startOutsideStateLane(() => {
      void drain.finally(async () => await this.lane.run(() => {
        if (queue.drainPromise === drain) queue.drainPromise = undefined;
        if (queue.outbound[0]?.released === true) this.startOutboundDrain(queue);
        else this.deleteEmptyRelocationQueue(queue);
      }));
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

function startOutsideStateLane<T>(work: () => T): T {
  return detachedStateLaneResource.runInAsyncScope(work);
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
