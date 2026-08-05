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
import { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import { resolveLifecycleHandler } from '../handlers/handler-instance-scope';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import type { ZLinkExecutionBarrier } from '../execution';

type ZLinkTimerOwnerSpot = ZLinkSpot | ZLinkEntrySpot;
type ZLinkTimerFailureReporter = (
  tick: ZLinkTimerTick,
  cause: unknown,
  event?: ZLinkSpotEventKind.TimerHandlerFailed | ZLinkSpotEventKind.TimerStoppedAfterUnhandledException
) => Promise<void> | void;

export interface ZLinkTimerRelocationState {
  readonly name: string;
  readonly periodMs: number;
  readonly overrunPolicy: ZLinkTimerOverrunPolicy;
  readonly maxCatchUpTicks: number;
  readonly stopOnUnhandledException: boolean;
  readonly startedAtUnixMs: number;
  readonly deliveryIndex: bigint;
  readonly lastScheduledIndex: bigint;
  readonly pendingTicks: number;
}

export class ZLinkSpotTimerRegistry {
  private readonly timers = new Map<string, {
    readonly generation: bigint;
    readonly timer: ZLinkManagedTimer;
  }>();
  private readonly generations = new Map<string, bigint>();
  private executionBarrier: ZLinkExecutionBarrier | undefined;

  constructor(
    _metrics?: import('../diagnostics').ZLinkRuntimeMetrics,
    private readonly flowCreationEnabled: () => boolean = () => true,
    private readonly executionSerialForTimer?: (
      name: string,
      fallback: ZLinkSpotSerialExecutor
    ) => ZLinkSpotSerialExecutor
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
    serial: ZLinkSpotSerialExecutor,
    spot: TSpot,
    providerResolver?: ZLinkProviderResolver,
    signal?: AbortSignal,
    reportFailure?: ZLinkTimerFailureReporter
  ): Promise<ZLinkTimer> {
    validateTimerRegistration(name, periodMs, options);
    throwIfAborted(signal);
    const handler = await resolveLifecycleHandler(spot, handlerType, providerResolver);
    const previous = this.timers.get(name);
    if (previous !== undefined) {
      this.timers.delete(name);
      await previous.timer.cancel(signal);
    }
    const generation = (this.generations.get(name) ?? 0n) + 1n;
    this.generations.set(name, generation);
    const executionSerial =
      this.executionSerialForTimer?.(name, serial) ?? serial;
    if (this.executionBarrier !== undefined) {
      executionSerial.setExecutionBarrier(this.executionBarrier);
    }
    const timer = new ZLinkManagedTimer(
      name,
      periodMs,
      normalizeTimerOptions(options),
      async (tick) => {
        const timerFlow = createInboundFlow(undefined, 'Timer', this.flowCreationEnabled());
        await executionSerial.execute(() => {
          const current = this.timers.get(name);
          if (current?.generation !== generation || current.timer !== timer) {
            return undefined;
          }
          return runWithFlow(timerFlow, () => handler.handle(spot, tick));
        });
      },
      reportFailure,
      () => !executionSerial.isExecuting
    );
    this.timers.set(name, { generation, timer });
    return new ZLinkRegisteredTimer(this, name, generation, timer);
  }

  async dispose(): Promise<void> {
    const timers = [...this.timers.values()].map((entry) => entry.timer);
    this.timers.clear();
    for (const timer of timers) {
      await timer.dispose();
    }
  }

  async captureRelocation(): Promise<readonly ZLinkTimerRelocationState[]> {
    const states: ZLinkTimerRelocationState[] = [];
    for (const [name, entry] of [...this.timers.entries()].sort(([left], [right]) =>
      left.localeCompare(right))) {
      const state = await entry.timer.captureRelocation();
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
      entry.timer.restoreRelocation(state);
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
    const current = this.timers.get(name);
    if (current?.generation === generation && current.timer === timer) {
      this.timers.delete(name);
    }
    await timer.cancel(signal);
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
  private disposed = false;
  private pausedForRelocation = false;
  private startedAtMs = Date.now();
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
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    if (this.timeout !== undefined) {
      clearTimeout(this.timeout);
      this.timeout = undefined;
    }
    if (this.shouldWaitForRunningOnCancel()) {
      await this.running;
    }
  }

  dispose(): Promise<void> {
    return this.cancel();
  }

  async captureRelocation(): Promise<ZLinkTimerRelocationState> {
    if (this.disposed) throw new Error(`Timer '${this.name}' is disposed.`);
    this.pausedForRelocation = true;
    if (this.timeout !== undefined) {
      clearTimeout(this.timeout);
      this.timeout = undefined;
    }
    if (this.shouldWaitForRunningOnCancel()) await this.running;
    return {
      name: this.name,
      periodMs: this.periodMs,
      overrunPolicy: this.options.overrunPolicy,
      maxCatchUpTicks: this.options.maxCatchUpTicks,
      stopOnUnhandledException: this.options.stopOnUnhandledException,
      startedAtUnixMs: this.startedAtMs,
      deliveryIndex: this.deliveryIndex,
      lastScheduledIndex: this.lastScheduledIndex,
      pendingTicks: 0
    };
  }

  restoreRelocation(state: ZLinkTimerRelocationState): void {
    if (this.disposed) throw new Error(`Timer '${this.name}' is disposed.`);
    if (
      state.name !== this.name
      || state.periodMs !== this.periodMs
      || state.overrunPolicy !== this.options.overrunPolicy
      || state.maxCatchUpTicks !== this.options.maxCatchUpTicks
      || state.stopOnUnhandledException !== this.options.stopOnUnhandledException
      || !Number.isSafeInteger(state.startedAtUnixMs)
      || state.deliveryIndex < 0n
      || state.lastScheduledIndex < 0n
      || state.pendingTicks !== 0
    ) {
      throw new Error(`Timer '${this.name}' relocation contract does not match its registration.`);
    }
    if (this.timeout !== undefined) clearTimeout(this.timeout);
    this.timeout = undefined;
    this.startedAtMs = state.startedAtUnixMs;
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
      this.running = this.fire().catch(() => undefined);
    }, delayMs);
  }

  private async fire(): Promise<void> {
    if (this.disposed) {
      return;
    }

    const scheduledIndex = this.selectScheduledIndex();
    const skippedTicks = scheduledIndex - this.lastScheduledIndex - 1n;
    const startedElapsedMs = this.elapsedMs();
    const scheduledElapsedMs = Number(scheduledIndex) * this.periodMs;
    this.deliveryIndex += 1n;
    const tick: ZLinkTimerTick = {
      name: this.name,
      deliveryIndex: this.deliveryIndex,
      scheduledIndex,
      periodMs: this.periodMs,
      scheduledAt: new Date(this.startedAtMs + scheduledElapsedMs),
      startedAt: new Date(this.startedAtMs + startedElapsedMs),
      scheduledElapsedMs,
      startedElapsedMs,
      delayMs: startedElapsedMs - scheduledElapsedMs,
      skippedTicks
    };

    let shouldContinue = true;
    try {
      await this.onTick(tick);
    } catch (cause) {
      shouldContinue = !this.options.stopOnUnhandledException;
      await this.onFailure?.(
        tick,
        cause,
        shouldContinue
          ? ZLinkSpotEventKind.TimerHandlerFailed
          : ZLinkSpotEventKind.TimerStoppedAfterUnhandledException
      );
    }

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
    return Date.now() - this.startedAtMs;
  }
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
  serial: ZLinkSpotSerialExecutor,
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
  serial: ZLinkSpotSerialExecutor,
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
