import type { ApplicationJobQueuePort } from './contracts';

type ApplicationJobPressureState = 'running' | 'paused';

interface ApplicationJobPressureTransition {
  readonly state: ApplicationJobPressureState;
  readonly sequence: bigint;
}

export interface ApplicationJobReceiveFlowTarget<TNativeState> {
  setReceiveFlowState(state: TNativeState): void;
}

interface ReceiveFlowTargetState {
  appliedSequence: bigint;
  appliedState?: ApplicationJobPressureState;
  pending?: ApplicationJobPressureTransition;
  applying: boolean;
}

class ReceiveFlowControllerClosedError extends Error {
  constructor() {
    super('Application job receive-flow controller is disposed.');
  }
}

/**
 * Host-local owner of absolute receive-flow propagation. A target is entered
 * in the registry before its initial snapshot is read, so a reentrant queue
 * transition is either applied immediately or fences the older snapshot by
 * sequence.
 */
export class ApplicationJobReceiveFlowController<TNativeState> {
  private readonly targets = new Map<
    ApplicationJobReceiveFlowTarget<TNativeState>,
    ReceiveFlowTargetState
  >();
  private readonly unregisterPressureListener: () => void;
  private latestTransition?: ApplicationJobPressureTransition;
  private disposed = false;

  constructor(
    private readonly queue: ApplicationJobQueuePort | undefined,
    private readonly toNativeState: (state: ApplicationJobPressureState) => TNativeState,
    private readonly failureSink?: (error: unknown) => void
  ) {
    this.unregisterPressureListener = queue?.onPressureStateChange?.(
      (state, sequence) => this.apply(state, sequence)
    ) ?? (() => undefined);
  }

  register(target: ApplicationJobReceiveFlowTarget<TNativeState>): boolean {
    if (this.disposed) throw new ReceiveFlowControllerClosedError();
    if (this.queue === undefined) return false;

    const targetState: ReceiveFlowTargetState = {
      appliedSequence: -1n,
      applying: false
    };
    this.targets.set(target, targetState);
    try {
      const current = this.currentTransition();
      if (current === undefined) {
        this.targets.delete(target);
        return false;
      }
      this.rememberLatest(current);
      this.enqueue(target, targetState, current, true);
      if (!this.isActiveTarget(target, targetState)) {
        throw new ReceiveFlowControllerClosedError();
      }
      return true;
    } catch (error) {
      this.unregister(target);
      if (!(error instanceof ReceiveFlowControllerClosedError)) {
        this.reportFailure(error);
      }
      throw error;
    }
  }

  unregister(target: ApplicationJobReceiveFlowTarget<TNativeState>): void {
    const targetState = this.targets.get(target);
    if (targetState === undefined) return;
    targetState.pending = undefined;
    this.targets.delete(target);
  }

  apply(state: ApplicationJobPressureState, sequence: bigint): void {
    if (this.disposed) return;
    const transition = { state, sequence } satisfies ApplicationJobPressureTransition;
    this.rememberLatest(transition);
    for (const [target, targetState] of this.targets) {
      this.enqueue(target, targetState, transition, false);
    }
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.unregisterPressureListener();
    for (const targetState of this.targets.values()) targetState.pending = undefined;
    this.targets.clear();
  }

  private currentTransition(): ApplicationJobPressureTransition | undefined {
    const snapshot = this.queue?.pressureTransition?.();
    const fallbackState = snapshot === undefined ? this.queue?.pressureState?.() : undefined;
    const current = snapshot ?? (fallbackState === undefined
      ? undefined
      : { state: fallbackState, sequence: 0n });
    if (this.latestTransition === undefined) return current;
    if (current === undefined || this.latestTransition.sequence > current.sequence) {
      return this.latestTransition;
    }
    return current;
  }

  private rememberLatest(transition: ApplicationJobPressureTransition): void {
    if (this.latestTransition === undefined
        || transition.sequence > this.latestTransition.sequence) {
      this.latestTransition = transition;
    }
  }

  private enqueue(
    target: ApplicationJobReceiveFlowTarget<TNativeState>,
    targetState: ReceiveFlowTargetState,
    transition: ApplicationJobPressureTransition,
    throwOnFailure: boolean
  ): void {
    if (this.targets.get(target) !== targetState
        || transition.sequence <= targetState.appliedSequence
        || (targetState.pending !== undefined
          && transition.sequence <= targetState.pending.sequence)) return;
    targetState.pending = transition;
    if (targetState.applying) return;

    targetState.applying = true;
    let firstFailure: unknown;
    try {
      while (this.targets.get(target) === targetState
          && targetState.pending !== undefined) {
        const next = targetState.pending;
        targetState.pending = undefined;
        if (next.sequence <= targetState.appliedSequence) continue;
        try {
          if (targetState.appliedState !== next.state) {
            target.setReceiveFlowState(this.toNativeState(next.state));
            targetState.appliedState = next.state;
          }
        } catch (error) {
          firstFailure ??= error;
          if (!throwOnFailure) this.reportFailure(error);
        } finally {
          targetState.appliedSequence = next.sequence;
        }
      }
    } finally {
      targetState.applying = false;
    }
    if (throwOnFailure && firstFailure !== undefined) throw firstFailure;
  }

  private reportFailure(error: unknown): void {
    this.recordFailure();
    try {
      this.failureSink?.(error);
    } catch {
      // Diagnostics cannot alter the absolute queue pressure state.
    }
  }

  private recordFailure(): void {
    try {
      this.queue?.recordFlowStateConfigFailure?.();
    } catch {
      // Metrics cannot alter receive-flow application or socket lifecycle.
    }
  }

  private isActiveTarget(
    target: ApplicationJobReceiveFlowTarget<TNativeState>,
    targetState: ReceiveFlowTargetState
  ): boolean {
    return !this.disposed && this.targets.get(target) === targetState;
  }
}
