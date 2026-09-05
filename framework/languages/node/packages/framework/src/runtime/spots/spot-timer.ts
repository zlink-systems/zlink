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
import { ZLinkSpotSerialTurnExecutor } from './spot-serial-turn-executor';
import { resolveLifecycleHandler } from '../handlers/handler-instance-scope';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import type { ZLinkExecutionBarrier } from '../execution';
import { ZLinkStateLane } from '../execution/state-lane';
import { AsyncResource } from 'node:async_hooks';

const detachedTimerStateLaneResource = new AsyncResource('zlink:spot-timer');

type ZLinkTimerOwnerSpot = ZLinkSpot | ZLinkEntrySpot;
type ZLinkTimerFailureReporter = (
  tick: ZLinkTimerTick,
  cause: unknown,
  event?: ZLinkSpotEventKind.TimerHandlerFailed | ZLinkSpotEventKind.TimerStoppedAfterUnhandledException
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

export interface ZLinkTimerRelocationPendingTick {
  readonly deliveryIndex: bigint;
  readonly scheduledIndex: bigint;
  readonly scheduledAtUnixMs: number;
  readonly skippedTicks: bigint;
}

export class ZLinkSpotTimerRegistry {
  private readonly lane = new ZLinkStateLane();
  private readonly timers = new Map<string, {
    readonly generation: bigint;
    readonly handlerType: string;
    readonly timer: ZLinkManagedTimer;
  }>();
  private readonly generations = new Map<string, bigint>();
  private executionBarrier: ZLinkExecutionBarrier | undefined;

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
    private readonly isTimerExecuting?: (name: string) => boolean
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
    throwIfAborted(signal);
    const handler = await resolveLifecycleHandler(spot, handlerType, providerResolver);
    const prepared = await this.lane.run(() => this.prepareAddCore(name));
    if (prepared.previous !== undefined) await prepared.previous.timer.cancel(signal);
    const executionSerial = this.executeTimer === undefined
      ? this.executionSerialForTimer?.(name, serial) ?? serial
      : undefined;
    if (this.executionBarrier !== undefined) executionSerial?.setExecutionBarrier(this.executionBarrier);
    const timer = startOutsideStateLane(() => new ZLinkManagedTimer(
      name,
      periodMs,
      normalizeTimerOptions(options),
      async (tick) => {
        const timerFlow = createInboundFlow(undefined, 'Timer', this.flowCreationEnabled());
        const operation = () => {
          const current = this.timers.get(name);
          if (current === undefined || current.generation !== prepared.generation || current.timer !== timer) {
            return undefined;
          }
          if (!this.executionAllowed()) return undefined;
          return runWithFlow(timerFlow, () => handler.handle(spot, tick));
        };
        if (this.executeTimer !== undefined) {
          await this.executeTimer(name, operation);
        } else {
          await executionSerial!.execute(operation);
        }
      },
      reportFailure,
      () => this.executeTimer === undefined
        ? !executionSerial!.isExecuting
        : this.isTimerExecuting?.(name) !== true
    ));
    const registered = await this.lane.run(() => this.completeAddCore(
      name,
      prepared.generation,
      handlerType.name,
      timer
    ));
    if (!registered) await timer.cancel(signal);
    return new ZLinkRegisteredTimer(this, name, prepared.generation, timer);
  }

  async dispose(): Promise<void> {
    const timers = await this.lane.run(() => {
      const active = [...this.timers.values()].map((entry) => entry.timer);
      this.timers.clear();
      return active;
    });
    for (const timer of timers) {
      await timer.dispose();
    }
  }

  async captureRelocation(): Promise<readonly ZLinkTimerRelocationState[]> {
    const timers = await this.lane.run(() => [...this.timers.entries()].sort(([left], [right]) =>
      left.localeCompare(right)));
    const states: ZLinkTimerRelocationState[] = [];
    for (const [name, entry] of timers) {
      const state = await entry.timer.captureRelocation(entry.handlerType);
      if (state.name !== name) throw new Error('Timer relocation identity changed during capture.');
      states.push(state);
    }
    return states;
  }

  restoreRelocation(states: readonly ZLinkTimerRelocationState[]): void {
    const byName = new Map(states.map(state => [state.name, state]));
    if (byName.size !== states.length || byName.size !== this.timers.size) {
      throw new Error('Timer relocation inventory does not match registered handlers.');
    }
    for (const [name, entry] of this.timers) {
      const state = byName.get(name);
      if (state === undefined) {
        throw new Error(`Timer '${name}' is missing from relocation state.`);
      }
      entry.timer.restoreRelocation(state, entry.handlerType);
    }
  }

  abortRelocation(states: readonly ZLinkTimerRelocationState[]): void {
    this.restoreRelocation(states);
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

  private prepareAddCore(name: string): {
    readonly previous: { readonly generation: bigint; readonly handlerType: string; readonly timer: ZLinkManagedTimer } | undefined;
    readonly generation: bigint;
  } {
    const previous = this.timers.get(name);
    this.timers.delete(name);
    const generation = (this.generations.get(name) ?? 0n) + 1n;
    this.generations.set(name, generation);
    return { previous, generation };
  }

  private completeAddCore(
    name: string,
    generation: bigint,
    handlerType: string,
    timer: ZLinkManagedTimer
  ): boolean {
    if (this.generations.get(name) !== generation) return false;
    this.timers.set(name, { generation, handlerType, timer });
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
    return this.registry.cancel(this.name, this.generation, this.timer, signal);
  }

  dispose(): Promise<void> {
    return this.cancel();
  }
}

export class ZLinkManagedTimer implements ZLinkTimer {
  private readonly lane = new ZLinkStateLane();
  private disposed = false;
  private pausedForRelocation = false;
  private startedAtMs = performance.now();
  private startedAtUnixMs = Date.now();
  private deliveryIndex = 0n;
  private lastScheduledIndex = 0n;
  private timeout: NodeJS.Timeout | undefined;
  private running: Promise<void> = Promise.resolve();

  constructor(
    private readonly name: string,
    private readonly periodMs: number,
    private readonly options: Required<ZLinkTimerOptions>,
    private readonly onTick: (tick: ZLinkTimerTick) => Promise<void>,
    private readonly onFailure?: ZLinkTimerFailureReporter,
    private readonly shouldWaitForRunningOnCancel: () => boolean = () => true
  ) {
    this.scheduleNext();
  }

  get isDisposed(): boolean {
    return this.disposed;
  }

  async cancel(_signal?: AbortSignal): Promise<void> {
    const prepared = await this.lane.run(() => this.cancelCore());
    await prepared.running;
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
      pendingTicks: []
    }));
  }

  restoreRelocation(state: ZLinkTimerRelocationState, handlerType = ''): void {
    if (this.disposed) throw new Error(`Timer '${this.name}' is disposed.`);
    if (
      state.name !== this.name
      || state.handlerType !== handlerType
      || state.periodMs !== this.periodMs
      || state.overrunPolicy !== this.options.overrunPolicy
      || state.maxCatchUpTicks !== this.options.maxCatchUpTicks
      || state.stopOnUnhandledException !== this.options.stopOnUnhandledException
      || !Number.isSafeInteger(state.startedAtUnixMs)
      || state.deliveryIndex < 0n
      || state.lastScheduledIndex < 0n
      || !Number.isSafeInteger(state.nextDueAtUnixMs)
      || state.nextDueAtUnixMs !== state.startedAtUnixMs
        + Number(state.lastScheduledIndex + 1n) * state.periodMs
      || state.pendingTicks.length !== 0
    ) {
      throw new Error(`Timer '${this.name}' relocation contract does not match its registration.`);
    }
    if (this.timeout !== undefined) clearTimeout(this.timeout);
    this.timeout = undefined;
    this.startedAtUnixMs = state.startedAtUnixMs;
    this.startedAtMs = performance.now() - Math.max(0, Date.now() - state.startedAtUnixMs);
    this.deliveryIndex = state.deliveryIndex;
    this.lastScheduledIndex = state.lastScheduledIndex;
    this.pausedForRelocation = false;
    this.scheduleNext();
  }

  private scheduleNext(): void {
    if (this.disposed || this.pausedForRelocation) {
      return;
    }

    const delayMs = this.options.overrunPolicy === ZLinkTimerOverrunPolicy.DelayNextTick
      ? this.periodMs
      : Math.max(0, Number(this.lastScheduledIndex + 1n) * this.periodMs - this.elapsedMs());
    this.timeout = setTimeout(() => {
      this.timeout = undefined;
      this.running = startOutsideStateLane(() => this.fire()).catch(() => undefined);
    }, delayMs);
  }

  private async fire(): Promise<void> {
    const prepared = await this.lane.run(() => this.prepareFireCore());
    if (prepared === undefined) return;

    let shouldContinue = true;
    try {
      await this.onTick(prepared.tick);
    } catch (cause) {
      shouldContinue = !this.options.stopOnUnhandledException;
      await this.onFailure?.(
        prepared.tick,
        cause,
        shouldContinue
          ? ZLinkSpotEventKind.TimerHandlerFailed
          : ZLinkSpotEventKind.TimerStoppedAfterUnhandledException
      );
    }

    await this.lane.run(() => this.completeFireCore(prepared.scheduledIndex, shouldContinue));
  }

  private cancelCore(): { readonly running: Promise<void> } {
    if (this.disposed) return { running: Promise.resolve() };
    this.disposed = true;
    if (this.timeout !== undefined) {
      clearTimeout(this.timeout);
      this.timeout = undefined;
    }
    return { running: this.shouldWaitForRunningOnCancel() ? this.running : Promise.resolve() };
  }

  private prepareCaptureCore(): { readonly running: Promise<void> } {
    if (this.disposed) throw new Error(`Timer '${this.name}' is disposed.`);
    this.pausedForRelocation = true;
    if (this.timeout !== undefined) {
      clearTimeout(this.timeout);
      this.timeout = undefined;
    }
    return { running: this.shouldWaitForRunningOnCancel() ? this.running : Promise.resolve() };
  }

  private prepareFireCore(): { readonly tick: ZLinkTimerTick; readonly scheduledIndex: bigint } | undefined {
    if (this.disposed) return undefined;
    const scheduledIndex = this.selectScheduledIndex();
    const skippedTicks = scheduledIndex - this.lastScheduledIndex - 1n;
    const startedElapsedMs = this.elapsedMs();
    const scheduledElapsedMs = Number(scheduledIndex) * this.periodMs;
    this.deliveryIndex += 1n;
    return {
      scheduledIndex,
      tick: {
        name: this.name,
        deliveryIndex: this.deliveryIndex,
        scheduledIndex,
        periodMs: this.periodMs,
        scheduledAt: new Date(this.startedAtUnixMs + scheduledElapsedMs),
        startedAt: new Date(),
        scheduledElapsedMs,
        startedElapsedMs,
        delayMs: startedElapsedMs - scheduledElapsedMs,
        skippedTicks
      }
    };
  }

  private completeFireCore(scheduledIndex: bigint, shouldContinue: boolean): void {
    this.lastScheduledIndex = scheduledIndex;
    if (!shouldContinue) {
      this.disposed = true;
      return;
    }
    this.scheduleNext();
  }

  private selectScheduledIndex(): bigint {
    if (this.options.overrunPolicy === ZLinkTimerOverrunPolicy.DelayNextTick) {
      return this.lastScheduledIndex + 1n;
    }

    const dueScheduledIndex = BigInt(Math.max(1, Math.floor(this.elapsedMs() / this.periodMs)));
    if (this.options.overrunPolicy === ZLinkTimerOverrunPolicy.SkipLateTicks) {
      return dueScheduledIndex;
    }

    const availableTicks = dueScheduledIndex - this.lastScheduledIndex;
    const maxCatchUpTicks = BigInt(this.options.maxCatchUpTicks);
    if (availableTicks > maxCatchUpTicks) {
      return dueScheduledIndex - maxCatchUpTicks + 1n;
    }

    return this.lastScheduledIndex + 1n;
  }

  private elapsedMs(): number {
    return performance.now() - this.startedAtMs;
  }
}

function startOutsideStateLane<T>(work: () => T): T {
  return detachedTimerStateLaneResource.runInAsyncScope(work);
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
    } catch {
    }
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
    if (
      handler.spotType === spotType
      || spotType.prototype instanceof handler.spotType
    ) {
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

function normalizeTimerOptions(options: ZLinkTimerOptions | undefined): Required<ZLinkTimerOptions> {
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
