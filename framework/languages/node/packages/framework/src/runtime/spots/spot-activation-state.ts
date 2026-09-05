import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  RoutingId,
  Type,
  ZLinkActor,
  ZLinkSpot
} from '../../contracts';
import {
  ZLinkSpotCloseReason,
  ZLinkSpotRelocationCoordinationMode,
  ZLinkSpotRelocationReadyOutcome,
  ZLinkUserSpotExecutionMode
} from '../../contracts';
import type { ZLinkSpotRelocationReadyCall } from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';
import { createAbortError } from '../abort';
import type { ZLinkBackendSpot } from '../backend/contracts';
import type { ZLinkSpotActorHandlerRegistryRuntime } from '../actors';
import type { DefaultZLinkSpotHandlerRegistry } from './spot-handler-registry';
import { ZLinkSpotSerialTurnExecutor } from './spot-serial-turn-executor';
import { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import type { ZLinkSpotTimerRegistry } from './spot-timer';
import type { ZLinkSpotActorJoinDispatch } from './spot-actor-join-dispatch';
import {
  ZLinkExecutionBarrier,
  type ZLinkExecutionBarrierSeal
} from '../execution';
import type { ZLinkTimerRelocationState } from './spot-timer';

export type ZLinkSpotActivationDomain =
  | {
      readonly kind: 'user';
      readonly executionMode: ZLinkUserSpotExecutionMode;
      readonly relocationCoordinationMode: ZLinkSpotRelocationCoordinationMode;
    }
  | {
      readonly kind: 'instance';
      readonly objectGeneration: bigint;
    };

export interface ZLinkSpotActivationOptions {
  readonly meshName: string;
  readonly spotId: RoutingId;
  readonly domain: ZLinkSpotActivationDomain;
  readonly spotType: Type<ZLinkSpot>;
  readonly spot: ZLinkSpot;
  readonly serial: ZLinkSpotSerialTurnExecutor;
  readonly serialExecutor?: ZLinkSpotSerialExecutor;
  readonly timers: ZLinkSpotTimerRegistry;
  readonly actorHandlers: ZLinkSpotActorHandlerRegistryRuntime;
  readonly handlers: DefaultZLinkSpotHandlerRegistry;
  readonly externalActorCount?: () => number;
  readonly nativeSpot?: ZLinkBackendSpot;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly executionBarrier?: ZLinkExecutionBarrier;
  actorDispatch?: ZLinkSpotActorJoinDispatch;
}

export interface ZLinkSpotRelocationCapture {
  readonly seal: ZLinkExecutionBarrierSeal;
  readonly timers: readonly ZLinkTimerRelocationState[];
}

/**
 * Internal signal that a close seal reached quiescence but the activation
 * became occupied again while it waited (an actor join queued ahead of the
 * close finished after the eager pre-seal occupancy check). This is not a
 * close failure: the seal is released, the Spot stays open, and the caller
 * reports `close(...)` as `false` instead of throwing.
 */
export class ZLinkSpotCloseOccupiedError extends Error {
  constructor(spotId: RoutingId) {
    super(`Spot '${String(spotId)}' became occupied while its close seal reached quiescence.`);
  }
}

export class ZLinkSpotActivation {
  readonly meshName: string;
  readonly spotId: RoutingId;
  readonly domain: ZLinkSpotActivationDomain;
  readonly spotType: Type<ZLinkSpot>;
  readonly spot: ZLinkSpot;
  readonly serial: ZLinkSpotSerialTurnExecutor;
  readonly serialExecutor: ZLinkSpotSerialExecutor;
  readonly timers: ZLinkSpotTimerRegistry;
  readonly actorHandlers: ZLinkSpotActorHandlerRegistryRuntime;
  readonly handlers: DefaultZLinkSpotHandlerRegistry;
  readonly nativeSpot?: ZLinkBackendSpot;
  readonly executionBarrier: ZLinkExecutionBarrier;
  actorDispatch?: ZLinkSpotActorJoinDispatch;

  private readonly joinedActors = new Map<string, ZLinkActor>();
  private readonly departedActorIds = new Set<string>();
  private readonly externalActorCount: () => number;
  private closeRequested = false;
  private idleEvictionRequested = false;
  private relocationReadyWaiter?: {
    readonly resolve: () => void;
    readonly reject: (error: unknown) => void;
    readonly signal?: AbortSignal;
    readonly abort?: () => void;
    consumed: boolean;
  };
  private relocationBoundaryConsumed = false;
  private relocationReadyDeferredTurnId?: number;

  constructor(options: ZLinkSpotActivationOptions) {
    this.meshName = options.meshName;
    this.spotId = options.spotId;
    this.domain = options.domain;
    if (this.domain.kind === 'instance' && this.domain.objectGeneration <= 0n) {
      throw new RangeError('Instance Spot object generation must be positive.');
    }
    this.spotType = options.spotType;
    this.spot = options.spot;
    this.serial = options.serial;
    this.serialExecutor = options.serialExecutor ?? new ZLinkSpotSerialExecutor(
      this.serial,
      this.executionMode,
      options.spotId
    );
    this.executionBarrier = options.executionBarrier ?? new ZLinkExecutionBarrier();
    this.serialExecutor.setExecutionBarrier(this.executionBarrier);
    if (typeof options.timers.setExecutionBarrier === 'function') {
      options.timers.setExecutionBarrier(this.executionBarrier);
    }
    this.timers = options.timers;
    this.actorHandlers = options.actorHandlers;
    this.handlers = options.handlers;
    this.externalActorCount = options.externalActorCount ?? (() => 0);
    this.nativeSpot = options.nativeSpot;
    this.actorDispatch = options.actorDispatch;
  }

  get objectGeneration(): bigint | undefined {
    return this.domain.kind === 'instance'
      ? this.domain.objectGeneration
      : undefined;
  }

  get executionMode(): ZLinkUserSpotExecutionMode {
    return this.domain.kind === 'user'
      ? this.domain.executionMode
      : ZLinkUserSpotExecutionMode.SpotWide;
  }

  get relocationCoordinationMode(): ZLinkSpotRelocationCoordinationMode {
    return this.domain.kind === 'user'
      ? this.domain.relocationCoordinationMode
      : ZLinkSpotRelocationCoordinationMode.FrameworkManaged;
  }

  relocationReadyCall(): ZLinkSpotRelocationReadyCall {
    return {
      defer: () => {
        if (this.relocationCoordinationMode !== ZLinkSpotRelocationCoordinationMode.ApplicationSignaled) {
          throw new ZLinkConfigurationException(
            'relocationReady().defer() requires ApplicationSignaled relocation readiness.'
          );
        }
        if (!this.serial.isCurrentTurn) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.InvalidOperation,
            'relocationReady().defer() requires the current Spot handler turn.'
          );
        }
        const turnId = this.serial.activeTurnId;
        if (
          this.relocationReadyDeferredTurnId === turnId
          || this.relocationBoundaryConsumed
        ) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.InvalidOperation,
            'relocationReady().defer() was already called for this boundary.'
          );
        }
        this.relocationReadyDeferredTurnId = turnId;
        this.serial.currentTurn?.blockFrameworkOperations();
        const waiter = this.relocationReadyWaiter;
        if (waiter !== undefined && !waiter.consumed) {
          waiter.consumed = true;
          this.relocationBoundaryConsumed = true;
          if (waiter.abort !== undefined) waiter.signal?.removeEventListener('abort', waiter.abort);
          waiter.resolve();
          return;
        }
        const seal = this.executionBarrier.seal();
        void this.serial.postBarrierTurn(async () => {
          try {
            await this.notifyRelocationReadyCompleted(
              ZLinkSpotRelocationReadyOutcome.Continued
            );
          } finally {
            this.relocationReadyDeferredTurnId = undefined;
            this.executionBarrier.abort(seal);
          }
        }).catch(() => undefined);
      }
    };
  }

  ensureContextOperationAllowed(): void {
    try {
      this.serial.currentTurn?.ensureFrameworkOperationAllowed();
    } catch (error) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        error instanceof Error ? error.message : String(error)
      );
    }
  }

  async waitForRelocationBoundary(signal?: AbortSignal): Promise<boolean> {
    if (this.relocationCoordinationMode !== ZLinkSpotRelocationCoordinationMode.ApplicationSignaled) {
      return false;
    }
    if (this.relocationReadyWaiter !== undefined) {
      throw new Error(`Spot '${String(this.spotId)}' already has a relocation readiness waiter.`);
    }
    await new Promise<void>((resolve, reject) => {
      const waiter: {
        resolve: () => void;
        reject: (error: unknown) => void;
        signal?: AbortSignal;
        abort?: () => void;
        consumed: boolean;
      } = { resolve, reject, signal, consumed: false };
      if (signal !== undefined) {
        waiter.abort = () => {
          if (this.relocationReadyWaiter === waiter) this.relocationReadyWaiter = undefined;
          reject(createAbortError());
        };
        if (signal.aborted) {
          waiter.abort();
          return;
        }
        signal.addEventListener('abort', waiter.abort, { once: true });
      }
      this.relocationReadyWaiter = waiter;
    });
    this.relocationReadyWaiter = undefined;
    return true;
  }

  async completeConsumedRelocationBoundary(
    outcome: ZLinkSpotRelocationReadyOutcome
  ): Promise<void> {
    if (!this.relocationBoundaryConsumed) return;
    this.relocationBoundaryConsumed = false;
    this.relocationReadyDeferredTurnId = undefined;
    await this.serial.post(() => this.notifyRelocationReadyCompleted(outcome));
  }

  async notifyRelocatedBoundary(): Promise<void> {
    await this.serial.post(() => this.notifyRelocationReadyCompleted(
      ZLinkSpotRelocationReadyOutcome.Relocated
    ));
  }

  executeActor<T>(
    actorId: string,
    operation: (serial: ZLinkSpotSerialTurnExecutor) => Promise<T> | T
  ): Promise<T> {
    return this.serialExecutor.executeActor(actorId, operation);
  }

  /** @internal Restores a non-executing durable prefix into one Actor FIFO. */
  admitActorDurablePrefix(
    actorId: string,
    records: readonly {
      readonly operation: (
        executeChild: <TChild>(
          child: (serial: import('./spot-serial-turn-executor').ZLinkSpotSerialTurnExecutor) =>
            Promise<TChild> | TChild
        ) => Promise<TChild>
      ) => Promise<void>;
      readonly workOptions?: import('../execution/serial-execution-queue').ZLinkSerialWorkOptions;
    }[]
  ): readonly Promise<void>[] {
    return this.serialExecutor.admitActorDurablePrefix(actorId, records);
  }

  sealExecution(): ZLinkExecutionBarrierSeal {
    return this.executionBarrier.seal();
  }

  waitForExecutionQuiescence(
    seal: ZLinkExecutionBarrierSeal,
    signal?: AbortSignal
  ): Promise<void> {
    return this.executionBarrier.waitForQuiescence(seal, signal);
  }

  abortExecutionSeal(seal: ZLinkExecutionBarrierSeal): boolean {
    return this.executionBarrier.abort(seal);
  }

  commitExecutionSeal(seal: ZLinkExecutionBarrierSeal): boolean {
    return this.executionBarrier.commit(seal);
  }

  async captureRelocation(signal?: AbortSignal): Promise<ZLinkSpotRelocationCapture> {
    const seal = this.sealExecution();
    try {
      await this.waitForExecutionQuiescence(seal, signal);
      return {
        seal,
        timers: await this.timers.captureRelocation()
      };
    } catch (error) {
      this.abortExecutionSeal(seal);
      throw error;
    }
  }

  abortRelocation(capture: ZLinkSpotRelocationCapture): boolean {
    if (!this.executionBarrier.isCurrent(capture.seal)) return false;
    this.timers.abortRelocation(capture.timers);
    return this.abortExecutionSeal(capture.seal);
  }

  async commitRelocation(capture: ZLinkSpotRelocationCapture): Promise<boolean> {
    if (!this.commitExecutionSeal(capture.seal)) return false;
    await this.timers.commitRelocation();
    return true;
  }

  resolveJoinedActor(actorId: string): ZLinkActor | undefined {
    return this.joinedActors.get(actorId);
  }

  hasDepartedActor(actorId: string): boolean {
    return this.departedActorIds.has(actorId);
  }

  commitActorJoin(actor: ZLinkActor): () => void {
    const previousActor = this.joinedActors.get(actor.context.actorId);
    const wasDeparted = this.departedActorIds.has(actor.context.actorId);
    this.departedActorIds.delete(actor.context.actorId);
    this.joinedActors.set(actor.context.actorId, actor);
    return () => {
      if (previousActor === undefined) {
        this.joinedActors.delete(actor.context.actorId);
      } else {
        this.joinedActors.set(actor.context.actorId, previousActor);
      }
      if (wasDeparted) {
        this.departedActorIds.add(actor.context.actorId);
      } else {
        this.departedActorIds.delete(actor.context.actorId);
      }
    };
  }

  beginActorTransfer(actorId: string): void {
    this.departedActorIds.add(actorId);
  }

  cancelActorTransfer(actorId: string): void {
    if (this.joinedActors.has(actorId)) {
      this.departedActorIds.delete(actorId);
    }
  }

  commitActorDeparture(actorId: string): void {
    this.departedActorIds.add(actorId);
    this.joinedActors.delete(actorId);
  }

  canClose(reason = ZLinkSpotCloseReason.ExplicitClose): boolean {
    // Host shutdown closes scopes while membership remains valid (§14).
    if (reason === ZLinkSpotCloseReason.HostShutdown) return true;
    const forceClose = this.closeRequested && !this.idleEvictionRequested;
    return this.joinedActors.size === 0 && (forceClose || this.externalActorCount() === 0);
  }

  requestClose(): void {
    this.closeRequested = true;
  }

  beginIdleEviction(): boolean {
    if (this.idleEvictionRequested || this.closeRequested) return false;
    if (!this.isIdleFor(Date.now(), 0)) return false;
    this.idleEvictionRequested = true;
    this.closeRequested = true;
    return true;
  }

  abortIdleEviction(): void {
    if (!this.idleEvictionRequested) return;
    this.idleEvictionRequested = false;
    this.closeRequested = false;
  }

  get isIdleEvicting(): boolean {
    return this.idleEvictionRequested;
  }

  isIdleFor(nowMs: number, timeoutMs: number): boolean {
    return !this.idleEvictionRequested
      && !this.closeRequested
      && !this.serial.isExecuting
      && !this.serial.hasPendingWork
      && !this.timers.hasActiveTimers
      && nowMs - this.serial.lastActivityAt >= timeoutMs;
  }

  private async notifyRelocationReadyCompleted(
    outcome: ZLinkSpotRelocationReadyOutcome
  ): Promise<void> {
    await this.spot.onRelocationReadyCompleted?.({ outcome });
  }

}
