import type {
  SpotId,
  Type,
  ZLinkEntrySpot,
  ZLinkSpot,
  ZLinkSpotTimerHandler,
  ZLinkTimer,
  ZLinkTimerOptions,
  ZLinkTimerTick
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import { ZLinkSpotEventKind } from '../diagnostics/internal-event-contracts';
import type {
  ZLinkEntrySpotTimerHandlerRegistration,
  ZLinkSpotTimerHandlerRegistration
} from '../../contracts/Configuration/RegistrationTypes';
import { ZLinkTimerOverrunPolicy } from '../../contracts';
import { validateTimerRegistration } from '../../contracts/Configuration/TimerRegistrationValidator';
import { throwIfAborted } from '../abort';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { ZLinkSpotSerialTurnExecutor } from './spot-serial-turn-executor';
import {
  prepareHandlerInstance,
  resolveLifecycleHandler
} from '../handlers/handler-instance-scope';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import type { ZLinkExecutionBarrier } from '../execution';
import { startOutsideStateLane, ZLinkStateLane } from '../execution/state-lane';

export interface ZLinkTimerClock {
  now(): number;
  utcNow(): number;
  setTimeout(callback: () => void, delayMs: number): unknown;
  clearTimeout(handle: unknown): void;
}

const systemTimerClock: ZLinkTimerClock = {
  now: () => performance.now(),
  utcNow: () => Date.now(),
  setTimeout: (callback, delayMs) => setTimeout(callback, delayMs),
  clearTimeout: (handle) => clearTimeout(handle as NodeJS.Timeout)
};

type ZLinkTimerOwnerSpot = ZLinkSpot | ZLinkEntrySpot;

interface ZLinkTimerFinalization {
  readonly promise: Promise<void>;
  readonly resolve: () => void;
  readonly reject: (cause: unknown) => void;
}

interface ZLinkTimerFinalizationStart {
  readonly timeout: unknown;
  readonly running: Promise<void> | undefined;
}

type ZLinkTimerFailureReporter = (
  tick: ZLinkTimerTick,
  cause: unknown,
  event?:
    ZLinkSpotEventKind.TimerHandlerFailed | ZLinkSpotEventKind.TimerStoppedAfterUnhandledException
) => Promise<void> | void;

export interface ZLinkTimerRelocationState {
  readonly name: string;
  readonly handlerType: string;
  readonly periodMs: number;
  readonly overrunPolicy: ZLinkTimerOverrunPolicy;
  readonly maxCatchUpTicks: number;
  readonly stopOnUnhandledException: boolean;
  readonly startedAtUnixMs: number;
  readonly deliveryIndex: bigint;
  readonly lastScheduledIndex: bigint;
  readonly nextDueAtUnixMs: number;
  readonly pendingTicks: readonly ZLinkTimerRelocationPendingTick[];
}

export interface ZLinkManagedTimerStart {
  /** Created paused; the owner starts it with `resume()`. */
  readonly paused?: boolean;
  /** Relocated logical schedule the timer continues from. */
  readonly cursor?: ZLinkTimerRelocationState;
}

export interface ZLinkTimerRelocationPendingTick {
  readonly deliveryIndex: bigint;
  readonly scheduledIndex: bigint;
  readonly scheduledAtUnixMs: number;
  readonly skippedTicks: bigint;
}

interface ZLinkTimerBinding {
  readonly handler: ZLinkSpotTimerHandler<ZLinkTimerOwnerSpot>;
  readonly spot: ZLinkTimerOwnerSpot;
  readonly executionSerial?: ZLinkSpotSerialTurnExecutor;
  readonly reportFailure?: ZLinkTimerFailureReporter;
}

interface ZLinkTimerBindingSlot {
  binding: ZLinkTimerBinding | undefined;
}

interface ZLinkTimerEntry {
  readonly generation: bigint;
  readonly handlerType: string;
  readonly timer: ZLinkManagedTimer;
  readonly handle: ZLinkRegisteredTimer;
  /** Restored from a relocation payload; Configure and OnInitialize reattach it. */
  readonly restored: boolean;
  readonly slot: ZLinkTimerBindingSlot;
}

type ZLinkTimerAddPreparation =
  | { readonly kind: 'reattached'; readonly handle: ZLinkTimer }
  | {
      readonly kind: 'registered';
      readonly previous: ZLinkTimerEntry | undefined;
      readonly generation: bigint;
    };

export class ZLinkSpotTimerRegistry {
  private readonly lane = new ZLinkStateLane();
  private readonly timers = new Map<string, ZLinkTimerEntry>();
  private readonly generations = new Map<string, bigint>();
  private executionBarrier: ZLinkExecutionBarrier | undefined;
  /** Frozen from the source capture or the target restore until abort or Ready. */
  private frozen = false;
  /**
   * The lifecycle owner opens this window around the target's Configure and
   * pre-Ready OnInitialize only (Spot timer §1). A registration captures it at
   * the call, so a registration started inside the window reattaches a
   * restored key even when its asynchronous part finishes after the window.
   */
  private initializing = false;
  /**
   * Handler constructors the target Spot announced with `addTimer(...)` during
   * Configure. Together with `handlers.addTimerHandler(...)` they form the
   * table that resolves a restored timer's handler by name.
   */
  private readonly configureTimerHandlers = new Set<Type>();
  private relocationHandlersResolved = false;

  constructor(
    _metrics?: import('../diagnostics').ZLinkRuntimeMetrics,
    private readonly flowCreationEnabled: () => boolean = () => true,
    private readonly executionSerialForTimer?: (
      name: string,
      fallback: ZLinkSpotSerialTurnExecutor
    ) => ZLinkSpotSerialTurnExecutor,
    private readonly executionAllowed: () => boolean = () => true,
    private readonly executeTimer?: <T>(
      name: string,
      operation: () => Promise<T> | T
    ) => Promise<T>,
    private readonly isTimerExecuting?: (name: string) => boolean,
    private readonly clock?: ZLinkTimerClock
  ) {}

  setExecutionBarrier(barrier: ZLinkExecutionBarrier): void {
    if (this.executionBarrier !== undefined && this.executionBarrier !== barrier) {
      throw new Error('ZLink timer registry already belongs to another execution barrier.');
    }
    this.executionBarrier = barrier;
  }

  get hasActiveTimers(): boolean {
    return this.timers.size > 0;
  }

  async add<TSpot extends ZLinkTimerOwnerSpot, THandler extends ZLinkSpotTimerHandler<TSpot>>(
    name: string,
    periodMs: number,
    options: ZLinkTimerOptions | undefined,
    handlerType: Type<THandler>,
    serial: ZLinkSpotSerialTurnExecutor,
    spot: TSpot,
    providerResolver?: ZLinkProviderResolver,
    signal?: AbortSignal,
    reportFailure?: ZLinkTimerFailureReporter
  ): Promise<ZLinkTimer> {
    validateTimerRegistration(name, periodMs, options);
    const initializing = this.initializing;
    if (initializing && !this.relocationHandlersResolved) {
      this.configureTimerHandlers.add(handlerType);
    }
    throwIfAborted(signal);
    prepareHandlerInstance(providerResolver, handlerType);
    const handler = await resolveLifecycleHandler(spot, handlerType, providerResolver);
    const executionSerial =
      this.executeTimer === undefined
        ? (this.executionSerialForTimer?.(name, serial) ?? serial)
        : undefined;
    if (this.executionBarrier !== undefined)
      executionSerial?.setExecutionBarrier(this.executionBarrier);
    const binding: ZLinkTimerBinding = {
      handler: handler as ZLinkSpotTimerHandler<ZLinkTimerOwnerSpot>,
      spot,
      executionSerial,
      reportFailure
    };
    const prepared = await this.lane.run(() =>
      this.prepareAddCore(name, handlerType.name, initializing, binding)
    );
    if (prepared.kind === 'reattached') return prepared.handle;
    if (prepared.previous !== undefined) await prepared.previous.timer.cancel(signal);
    const slot: ZLinkTimerBindingSlot = { binding };
    const timer = this.createTimer(name, prepared.generation, periodMs, options, slot);
    const handle = new ZLinkRegisteredTimer(this, name, prepared.generation, timer);
    const registered = await this.lane.run(() =>
      this.completeAddCore(name, {
        generation: prepared.generation,
        handlerType: handlerType.name,
        timer,
        handle,
        restored: false,
        slot
      })
    );
    if (!registered) await timer.cancel(signal);
    return handle;
  }

  /**
   * Stages the source payload on a fresh relocation target before Configure.
   * The restored registrations keep their schedule and stay frozen until Ready.
   */
  stageRelocation(states: readonly ZLinkTimerRelocationState[]): void {
    if (this.frozen || this.timers.size !== 0) {
      throw new Error('Timer relocation can only be restored into a fresh registry.');
    }
    const restored = states.map((state): [string, ZLinkTimerEntry] => {
      if (states.some((other) => other !== state && other.name === state.name)) {
        throw new Error(`Timer '${state.name}' appears twice in relocation state.`);
      }
      const options = {
        overrunPolicy: state.overrunPolicy,
        maxCatchUpTicks: state.maxCatchUpTicks,
        stopOnUnhandledException: state.stopOnUnhandledException
      };
      validateTimerRegistration(state.name, state.periodMs, options);
      const slot: ZLinkTimerBindingSlot = { binding: undefined };
      const timer = this.createTimer(state.name, 1n, state.periodMs, options, slot, state);
      return [
        state.name,
        {
          generation: 1n,
          handlerType: state.handlerType,
          timer,
          handle: new ZLinkRegisteredTimer(this, state.name, 1n, timer),
          restored: true,
          slot
        }
      ];
    });
    this.frozen = true;
    for (const [name, entry] of restored) {
      this.generations.set(name, entry.generation);
      this.timers.set(name, entry);
    }
  }

  /** Runs one relocation-target initialization callback inside the reattach window. */
  async runRelocationInitialization<T>(operation: () => Promise<T> | T): Promise<T> {
    if (!this.frozen || this.initializing) {
      throw new Error('Timer relocation initialization is not active.');
    }
    this.initializing = true;
    try {
      return await operation();
    } finally {
      this.initializing = false;
    }
  }

  /**
   * After Configure, resolves every restored timer's handler constructor by
   * name among the constructors the Spot announced in Configure: `addTimer(...)`
   * there or `handlers.addTimerHandler(...)`. A restored key that Configure did
   * not reattach is registered with the resolved constructor so it survives
   * Ready without re-registration. No match or more than one match fails the
   * target restore.
   */
  async resolveRelocationHandlers(
    declared: readonly Type[],
    register: (name: string, periodMs: number, handlerType: Type) => Promise<unknown>
  ): Promise<void> {
    if (!this.frozen || !this.initializing || this.relocationHandlersResolved) {
      throw new Error('Timer relocation handler resolution is not active.');
    }
    this.relocationHandlersResolved = true;
    const constructors = [...new Set([...declared, ...this.configureTimerHandlers])];
    const restored = await this.lane.run(() =>
      [...this.timers.entries()].filter(([, entry]) => entry.restored)
    );
    for (const [name, entry] of restored) {
      const matches = constructors.filter((type) => type.name === entry.handlerType);
      if (matches.length !== 1) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RelocationFailed,
          matches.length === 0
            ? `Timer '${name}' handler '${entry.handlerType}' is not declared. Register its timer with context.addTimer(...) in configure(), or declare it with context.handlers.addTimerHandler(Handler) in configure().`
            : `Timer '${name}' handler '${entry.handlerType}' is ambiguous: ${matches.length} declared constructors share the name.`
        );
      }
      if (entry.slot.binding === undefined) {
        await register(name, entry.timer.periodMs, matches[0]!);
      }
    }
  }

  /** Ready: starts the restored and newly registered timers. */
  async publishRelocation(): Promise<void> {
    await this.lane.run(() => {
      for (const [name, entry] of this.timers) {
        if (entry.slot.binding === undefined) {
          throw new Error(`Restored timer '${name}' has no resolved handler at Ready.`);
        }
      }
      this.frozen = false;
      for (const entry of this.timers.values()) {
        startOutsideStateLane(() => entry.timer.resume());
      }
    });
  }

  async dispose(): Promise<void> {
    const timers = await this.lane.run(() => {
      const active = [...this.timers.entries()].map(([name, entry]) => ({
        name,
        timer: entry.timer,
        wait: this.executeTimer === undefined ? true : this.isTimerExecuting?.(name) !== true
      }));
      this.timers.clear();
      return active;
    });
    const finalizations = timers.map(({ timer }) => timer.dispose());
    await Promise.all(finalizations.filter((_, index) => timers[index]!.wait));
  }

  async captureRelocation(): Promise<readonly ZLinkTimerRelocationState[]> {
    const timers = await this.lane.run(() => {
      this.frozen = true;
      return [...this.timers.entries()].sort(([left], [right]) => left.localeCompare(right));
    });
    const states: ZLinkTimerRelocationState[] = [];
    for (const [name, entry] of timers) {
      const state = await entry.timer.captureRelocation(entry.handlerType);
      if (state.name !== name) throw new Error('Timer relocation identity changed during capture.');
      states.push(state);
    }
    return states;
  }

  /** Source abort: the captured timers continue from their own unchanged cursor. */
  abortRelocation(): void {
    this.frozen = false;
    for (const entry of this.timers.values()) {
      startOutsideStateLane(() => entry.timer.resume());
    }
  }

  async commitRelocation(): Promise<void> {
    await this.dispose();
  }

  async cancel(
    name: string,
    generation: bigint,
    timer: ZLinkManagedTimer,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    await this.lane.run(() => this.cancelCore(name, generation, timer));
    await timer.cancel(signal);
  }

  private createTimer(
    name: string,
    generation: bigint,
    periodMs: number,
    options: ZLinkTimerOptions | undefined,
    slot: ZLinkTimerBindingSlot,
    cursor?: ZLinkTimerRelocationState
  ): ZLinkManagedTimer {
    let callbackStarted = false;
    const timer: ZLinkManagedTimer = startOutsideStateLane(
      () =>
        new ZLinkManagedTimer(
          name,
          periodMs,
          normalizeTimerOptions(options),
          async (tick) => {
            callbackStarted = false;
            const binding = slot.binding;
            if (binding === undefined) return;
            const timerFlow = createInboundFlow(
              undefined,
              'Timer',
              this.flowCreationEnabled(),
              undefined
            );
            const operation = () => {
              const current = this.timers.get(name);
              if (
                current === undefined ||
                current.generation !== generation ||
                current.timer !== timer
              ) {
                return undefined;
              }
              if (!this.executionAllowed()) return undefined;
              callbackStarted = true;
              return runWithFlow(timerFlow, () => binding.handler.handle(binding.spot, tick));
            };
            if (this.executeTimer !== undefined) {
              await this.executeTimer(name, operation);
            } else {
              await binding.executionSerial!.execute(operation);
            }
          },
          (tick, cause, event) => slot.binding?.reportFailure?.(tick, cause, event),
          () => callbackStarted,
          this.clock,
          { paused: true, cursor }
        )
    );
    return timer;
  }

  private prepareAddCore(
    name: string,
    handlerType: string,
    initializing: boolean,
    binding: ZLinkTimerBinding
  ): ZLinkTimerAddPreparation {
    if (this.frozen && !initializing) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `Timer '${name}' registration is sealed for relocation.`
      );
    }
    const previous = this.timers.get(name);
    if (initializing && previous?.restored === true) {
      if (previous.handlerType !== handlerType) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.InvalidOperation,
          `Timer '${name}' was restored with handler '${previous.handlerType}', not '${handlerType}'.`
        );
      }
      previous.slot.binding = binding;
      return { kind: 'reattached', handle: previous.handle };
    }
    this.timers.delete(name);
    const generation = (this.generations.get(name) ?? 0n) + 1n;
    this.generations.set(name, generation);
    return { kind: 'registered', previous, generation };
  }

  private completeAddCore(name: string, entry: ZLinkTimerEntry): boolean {
    if (this.generations.get(name) !== entry.generation) return false;
    this.timers.set(name, entry);
    if (!this.frozen) startOutsideStateLane(() => entry.timer.resume());
    return true;
  }

  private cancelCore(name: string, generation: bigint, timer: ZLinkManagedTimer): void {
    const current = this.timers.get(name);
    if (current?.generation === generation && current.timer === timer) {
      this.timers.delete(name);
    }
  }
}

class ZLinkRegisteredTimer implements ZLinkTimer {
  private finalization: Promise<void> | undefined;

  constructor(
    private readonly registry: ZLinkSpotTimerRegistry,
    private readonly name: string,
    private readonly generation: bigint,
    private readonly timer: ZLinkManagedTimer
  ) {}

  get isDisposed(): boolean {
    return this.timer.isDisposed;
  }

  cancel(signal?: AbortSignal): Promise<void> {
    if (this.finalization !== undefined) return this.finalization;
    try {
      throwIfAborted(signal);
    } catch (failure) {
      return Promise.reject(failure);
    }
    this.finalization = this.registry.cancel(this.name, this.generation, this.timer);
    return this.finalization;
  }

  dispose(): Promise<void> {
    return this.cancel();
  }
}

export class ZLinkManagedTimer implements ZLinkTimer {
  private readonly lane = new ZLinkStateLane();
  private disposed = false;
  private pausedForRelocation = false;
  private startedAtMs: number;
  private startedAtUnixMs: number;
  /** DeliveryIndex and ScheduledIndex of the last completed tick. */
  private deliveryIndex = 0n;
  private lastScheduledIndex = 0n;
  /**
   * The selected tick whose callback has not completed (Spot timer §1). It is
   * the only pending tick a relocation captures, and a restored timer delivers
   * it before it selects another tick.
   */
  private pendingScheduledIndex: bigint | undefined;
  private timeout: unknown;
  /** The dispatch in flight, from its timer callback until `completeFireCore`. */
  private running: Promise<void> | undefined;
  private dispatchFailure: { readonly cause: unknown } | undefined;
  private finalization: ZLinkTimerFinalization | undefined;

  constructor(
    private readonly name: string,
    readonly periodMs: number,
    private readonly options: Required<ZLinkTimerOptions>,
    private readonly onTick: (tick: ZLinkTimerTick) => Promise<void>,
    private readonly onFailure?: ZLinkTimerFailureReporter,
    private readonly shouldWaitForRunningOnCancel: () => boolean = () => true,
    private readonly clock: ZLinkTimerClock = systemTimerClock,
    start: ZLinkManagedTimerStart = {}
  ) {
    const cursor = start.cursor;
    if (cursor === undefined) {
      this.startedAtMs = clock.now();
      this.startedAtUnixMs = clock.utcNow();
    } else {
      validateRelocationCursor(name, periodMs, cursor);
      this.startedAtUnixMs = cursor.startedAtUnixMs;
      this.startedAtMs = clock.now() - Math.max(0, clock.utcNow() - cursor.startedAtUnixMs);
      this.deliveryIndex = cursor.deliveryIndex;
      this.lastScheduledIndex = cursor.lastScheduledIndex;
      this.pendingScheduledIndex = cursor.pendingTicks[0]?.scheduledIndex;
    }
    this.pausedForRelocation = start.paused === true;
    this.scheduleNext();
  }

  get isDisposed(): boolean {
    return this.disposed;
  }

  cancel(_signal?: AbortSignal): Promise<void> {
    if (this.finalization !== undefined) return this.finalization.promise;

    let resolve!: () => void;
    let reject!: (cause: unknown) => void;
    const promise = new Promise<void>((complete, fail) => {
      resolve = complete;
      reject = fail;
    });
    const finalization = { promise, resolve, reject };
    this.finalization = finalization;

    try {
      const preparation = this.lane.run(() => this.cancelCore());
      void preparation.then((start) => {
        void this.completeFinalization(start, finalization).then(undefined, reject);
      }, reject);
    } catch (failure) {
      reject(failure);
    }
    return promise;
  }

  dispose(): Promise<void> {
    return this.cancel();
  }

  async captureRelocation(handlerType = ''): Promise<ZLinkTimerRelocationState> {
    const prepared = await this.lane.run(() => this.prepareCaptureCore());
    await prepared.running;
    return await this.lane.run(() => ({
      name: this.name,
      handlerType,
      periodMs: this.periodMs,
      overrunPolicy: this.options.overrunPolicy,
      maxCatchUpTicks: this.options.maxCatchUpTicks,
      stopOnUnhandledException: this.options.stopOnUnhandledException,
      startedAtUnixMs: this.startedAtUnixMs,
      deliveryIndex: this.deliveryIndex,
      lastScheduledIndex: this.lastScheduledIndex,
      nextDueAtUnixMs: this.startedAtUnixMs + Number(this.lastScheduledIndex + 1n) * this.periodMs,
      pendingTicks:
        this.pendingScheduledIndex === undefined
          ? []
          : [this.pendingTick(this.pendingScheduledIndex)]
    }));
  }

  /** Starts a timer created paused, or continues one paused by a relocation capture. */
  resume(): void {
    if (this.disposed || !this.pausedForRelocation) return;
    this.pausedForRelocation = false;
    this.scheduleNext();
  }

  private scheduleNext(): void {
    // The dispatch in flight schedules the next tick when it completes.
    if (this.disposed || this.pausedForRelocation || this.running !== undefined) {
      return;
    }

    const delayMs =
      this.pendingScheduledIndex !== undefined
        ? 0
        : this.options.overrunPolicy === ZLinkTimerOverrunPolicy.DelayNextTick
          ? this.periodMs
          : Math.max(0, Number(this.lastScheduledIndex + 1n) * this.periodMs - this.elapsedMs());
    this.timeout = startOutsideStateLane(() =>
      this.clock.setTimeout(() => {
        this.timeout = undefined;
        this.running = this.fire().then(
          () => undefined,
          (cause) =>
            this.lane.run(() => {
              this.dispatchFailure ??= { cause };
            })
        );
      }, delayMs)
    );
  }

  private async fire(): Promise<void> {
    const prepared = await this.lane.run(() => this.prepareFireCore());
    if (prepared === undefined) return;

    let shouldContinue = true;
    try {
      await this.onTick(prepared.tick);
    } catch (cause) {
      // A tick whose callback had not started when a relocation captured it
      // belongs to the capture: the target delivers it, so the source neither
      // reports nor completes it.
      const started = this.shouldWaitForRunningOnCancel();
      if (!started && (await this.lane.run(() => this.pausedForRelocation))) return;
      shouldContinue = !this.options.stopOnUnhandledException;
      await this.onFailure?.(
        prepared.tick,
        cause,
        shouldContinue
          ? ZLinkSpotEventKind.TimerHandlerFailed
          : ZLinkSpotEventKind.TimerStoppedAfterUnhandledException
      );
    }

    await this.lane.run(() => this.completeFireCore(shouldContinue));
  }

  private cancelCore(): ZLinkTimerFinalizationStart {
    this.disposed = true;
    const timeout = this.timeout;
    this.timeout = undefined;
    return {
      timeout,
      running: this.shouldWaitForRunningOnCancel() ? this.running : undefined
    };
  }

  private async completeFinalization(
    start: ZLinkTimerFinalizationStart,
    finalization: ZLinkTimerFinalization
  ): Promise<void> {
    const failures: unknown[] = [];
    if (start.timeout !== undefined) {
      try {
        this.clock.clearTimeout(start.timeout);
      } catch (failure) {
        failures.push(failure);
      }
    }
    try {
      await start.running;
    } catch (failure) {
      failures.push(failure);
    }
    const dispatchFailure = await this.lane.run(() => this.dispatchFailure);
    if (dispatchFailure !== undefined) failures.push(dispatchFailure.cause);

    if (failures.length === 0) {
      finalization.resolve();
    } else if (failures.length === 1) {
      finalization.reject(failures[0]);
    } else {
      finalization.reject(new AggregateError(failures, `Timer '${this.name}' cleanup failed.`));
    }
  }

  private prepareCaptureCore(): { readonly running: Promise<void> | undefined } {
    if (this.disposed) throw new Error(`Timer '${this.name}' is disposed.`);
    this.pausedForRelocation = true;
    if (this.timeout !== undefined) {
      this.clock.clearTimeout(this.timeout);
      this.timeout = undefined;
    }
    return { running: this.shouldWaitForRunningOnCancel() ? this.running : undefined };
  }

  private prepareFireCore(): { readonly tick: ZLinkTimerTick } | undefined {
    if (this.disposed) return undefined;
    this.pendingScheduledIndex ??= this.selectScheduledIndex();
    const pending = this.pendingTick(this.pendingScheduledIndex);
    const startedElapsedMs = this.elapsedMs();
    const scheduledElapsedMs = Number(pending.scheduledIndex) * this.periodMs;
    return {
      tick: {
        name: this.name,
        deliveryIndex: pending.deliveryIndex,
        scheduledIndex: pending.scheduledIndex,
        periodMs: this.periodMs,
        scheduledAt: new Date(pending.scheduledAtUnixMs),
        startedAt: new Date(this.clock.utcNow()),
        scheduledElapsedMs,
        startedElapsedMs,
        delayMs: startedElapsedMs - scheduledElapsedMs,
        skippedTicks: pending.skippedTicks
      }
    };
  }

  /** The tick info of the pending tick at `scheduledIndex` after the completed cursor. */
  private pendingTick(scheduledIndex: bigint): ZLinkTimerRelocationPendingTick {
    return relocationPendingTick(
      this.startedAtUnixMs,
      this.periodMs,
      this.deliveryIndex,
      this.lastScheduledIndex,
      scheduledIndex
    );
  }

  private completeFireCore(shouldContinue: boolean): void {
    this.deliveryIndex += 1n;
    this.lastScheduledIndex = this.pendingScheduledIndex!;
    this.pendingScheduledIndex = undefined;
    this.running = undefined;
    if (!shouldContinue) {
      this.disposed = true;
      return;
    }
    this.scheduleNext();
  }

  private selectScheduledIndex(): bigint {
    const nextScheduledIndex = this.lastScheduledIndex + 1n;
    if (this.options.overrunPolicy === ZLinkTimerOverrunPolicy.DelayNextTick) {
      return nextScheduledIndex;
    }

    // The due tick is the latest nominal tick the monotonic clock has reached, but never
    // one that was already delivered: the platform timer truncates the delay to whole
    // milliseconds and may start the callback a fraction before the nominal due time.
    const elapsedIndex = BigInt(Math.floor(this.elapsedMs() / this.periodMs));
    const dueScheduledIndex = elapsedIndex > nextScheduledIndex ? elapsedIndex : nextScheduledIndex;
    if (this.options.overrunPolicy === ZLinkTimerOverrunPolicy.SkipLateTicks) {
      return dueScheduledIndex;
    }

    const availableTicks = dueScheduledIndex - this.lastScheduledIndex;
    const maxCatchUpTicks = BigInt(this.options.maxCatchUpTicks);
    if (availableTicks > maxCatchUpTicks) {
      return dueScheduledIndex - maxCatchUpTicks + 1n;
    }

    return nextScheduledIndex;
  }

  private elapsedMs(): number {
    return this.clock.now() - this.startedAtMs;
  }
}

function validateRelocationCursor(
  name: string,
  periodMs: number,
  state: ZLinkTimerRelocationState
): void {
  if (
    state.name !== name ||
    state.periodMs !== periodMs ||
    !Number.isSafeInteger(state.startedAtUnixMs) ||
    state.deliveryIndex < 0n ||
    state.lastScheduledIndex < 0n ||
    !Number.isSafeInteger(state.nextDueAtUnixMs) ||
    state.nextDueAtUnixMs !==
      state.startedAtUnixMs + Number(state.lastScheduledIndex + 1n) * state.periodMs ||
    state.pendingTicks.length > 1 ||
    state.pendingTicks.some((pending) => {
      const expected = relocationPendingTick(
        state.startedAtUnixMs,
        state.periodMs,
        state.deliveryIndex,
        state.lastScheduledIndex,
        pending.scheduledIndex
      );
      return (
        pending.scheduledIndex <= state.lastScheduledIndex ||
        pending.deliveryIndex !== expected.deliveryIndex ||
        pending.scheduledAtUnixMs !== expected.scheduledAtUnixMs ||
        pending.skippedTicks !== expected.skippedTicks
      );
    })
  ) {
    throw new Error(`Timer '${name}' relocation state is invalid.`);
  }
}

/**
 * Spot timer §1: the tick info that follows the completed cursor. A timer holds
 * at most one pending tick, so its DeliveryIndex is the next one.
 */
function relocationPendingTick(
  startedAtUnixMs: number,
  periodMs: number,
  deliveryIndex: bigint,
  lastScheduledIndex: bigint,
  scheduledIndex: bigint
): ZLinkTimerRelocationPendingTick {
  return {
    deliveryIndex: deliveryIndex + 1n,
    scheduledIndex,
    scheduledAtUnixMs: startedAtUnixMs + Number(scheduledIndex) * periodMs,
    skippedTicks: scheduledIndex - lastScheduledIndex - 1n
  };
}

export function createTimerDiagnostics(
  sourceName: string,
  spotId: SpotId,
  isEntrySpot: boolean,
  timerName: string,
  handlerType: Type,
  publisher: ZLinkRuntimeEventPublisher | undefined
): ZLinkTimerFailureReporter | undefined {
  if (publisher === undefined) {
    return undefined;
  }
  return async (tick, cause, event = ZLinkSpotEventKind.TimerHandlerFailed) => {
    try {
      await publisher.publish({
        sourceName,
        timestamp: new Date(),
        event,
        timerDiagnostic: {
          spotId,
          isEntrySpot,
          timerName,
          handlerType: handlerType.name,
          deliveryIndex: tick.deliveryIndex,
          scheduledIndex: tick.scheduledIndex,
          exceptionType: exceptionType(cause),
          exceptionMessage: exceptionMessage(cause)
        }
      });
    } catch {}
  };
}

interface ZLinkEntrySpotTimerRegistrationSet {
  readonly timerHandlers?: readonly ZLinkEntrySpotTimerHandlerRegistration[];
}

interface ZLinkUserSpotTimerRegistrationSet {
  readonly timerHandlers?: readonly ZLinkSpotTimerHandlerRegistration[];
}

export async function addEntrySpotTimerRegistrations(
  timers: ZLinkSpotTimerRegistry,
  entrySpotType: Type<ZLinkEntrySpot>,
  entrySpot: ZLinkEntrySpot,
  serial: ZLinkSpotSerialTurnExecutor,
  registrations: ZLinkEntrySpotTimerRegistrationSet,
  options: {
    readonly providerResolver?: ZLinkProviderResolver;
    readonly spotNodeName: string;
    readonly spotId: SpotId;
    readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  }
): Promise<void> {
  for (const handler of registrations.timerHandlers ?? []) {
    if (handler.entrySpotType === entrySpotType) {
      await timers.add(
        handler.name,
        handler.periodMs,
        handler.options,
        handler.handlerType as Type<ZLinkSpotTimerHandler<ZLinkEntrySpot>>,
        serial,
        entrySpot,
        options.providerResolver,
        undefined,
        createTimerDiagnostics(
          options.spotNodeName,
          options.spotId,
          true,
          handler.name,
          handler.handlerType,
          options.runtimeEventPublisher
        )
      );
    }
  }
}

export async function addSpotTimerRegistrations(
  timers: ZLinkSpotTimerRegistry,
  spotType: Type<ZLinkSpot>,
  spotId: SpotId,
  spot: ZLinkSpot,
  serial: ZLinkSpotSerialTurnExecutor,
  registrations: ZLinkUserSpotTimerRegistrationSet,
  options: {
    readonly providerResolver?: ZLinkProviderResolver;
    readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
    readonly signal?: AbortSignal;
  }
): Promise<void> {
  for (const handler of registrations.timerHandlers ?? []) {
    if (handler.spotType === spotType || spotType.prototype instanceof handler.spotType) {
      await timers.add(
        handler.name,
        handler.periodMs,
        handler.options,
        handler.handlerType as Type<ZLinkSpotTimerHandler<ZLinkSpot>>,
        serial,
        spot,
        options.providerResolver,
        options.signal,
        createTimerDiagnostics(
          String(spotId),
          spotId,
          false,
          handler.name,
          handler.handlerType,
          options.runtimeEventPublisher
        )
      );
    }
  }
}

function normalizeTimerOptions(
  options: ZLinkTimerOptions | undefined
): Required<ZLinkTimerOptions> {
  return {
    overrunPolicy: options?.overrunPolicy ?? ZLinkTimerOverrunPolicy.SkipLateTicks,
    maxCatchUpTicks: options?.maxCatchUpTicks ?? 1,
    stopOnUnhandledException: options?.stopOnUnhandledException ?? false
  };
}

function exceptionType(cause: unknown): string {
  return cause instanceof Error ? cause.name : typeof cause;
}

function exceptionMessage(cause: unknown): string {
  return cause instanceof Error ? cause.message : String(cause);
}
