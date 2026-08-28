import { ZLinkUserSpotExecutionMode } from '../../contracts';
import { ZLinkActorSerialExecutor } from '../actors';
import type { ZLinkExecutionBarrier } from '../execution';
import type {
  ZLinkSerialSchedulerOptions,
  ZLinkSerialWorkOptions
} from '../execution/serial-execution-queue';
import { ZLinkSpotSerialTurnExecutor } from './spot-serial-turn-executor';

/**
 * Owns every application serial unit that belongs to one Spot activation.
 *
 * Node owns these maps synchronously: no Promise boundary occurs while a
 * child is selected, created, or removed.
 */
export class ZLinkSpotSerialExecutor {
  private readonly actorSerials = new Map<string, ZLinkSpotSerialTurnExecutor>();
  private readonly timerSerials = new Map<string, ZLinkSpotSerialTurnExecutor>();
  private readonly actorExecutors = new Map<string, ZLinkActorSerialExecutor>();
  private executionBarrier: ZLinkExecutionBarrier | undefined;
  private closing = false;
  private closePromise: Promise<void> | undefined;

  constructor(
    private readonly spotSerial: ZLinkSpotSerialTurnExecutor,
    private readonly executionMode: ZLinkUserSpotExecutionMode,
    private readonly sourceSpotId: unknown,
    private readonly actorOptions?: ZLinkSerialSchedulerOptions
  ) {}

  setExecutionBarrier(barrier: ZLinkExecutionBarrier): void {
    if (this.executionBarrier !== undefined && this.executionBarrier !== barrier) {
      throw new Error('ZLink Spot serial executor already belongs to another execution barrier.');
    }
    this.executionBarrier = barrier;
    this.spotSerial.setExecutionBarrier(barrier);
    for (const serial of this.actorSerials.values()) serial.setExecutionBarrier(barrier);
    for (const serial of this.timerSerials.values()) serial.setExecutionBarrier(barrier);
  }

  executeSpot<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    return this.spotSerial.execute(operation, workOptions);
  }

  executeActor<T>(
    actorId: string,
    operation: (serial: ZLinkSpotSerialTurnExecutor) => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    return this.actorExecutor(actorId).execute(
      () => operation(this.actorSerial(actorId)),
      workOptions
    );
  }

  executeTimer<T>(
    timerName: string,
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    return this.timerSerial(timerName).execute(operation, workOptions);
  }

  isTimerExecuting(timerName: string): boolean {
    if (this.executionMode === ZLinkUserSpotExecutionMode.SpotWide) {
      return this.spotSerial.isExecuting;
    }
    return this.timerSerials.get(timerName)?.isExecuting ?? false;
  }

  executeLifecycle<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    return this.spotSerial.execute(operation, workOptions);
  }

  /**
   * Makes all child queues unreachable and completes them in one synchronous
   * ownership turn. Already accepted work remains drainable.
   */
  close(): Promise<void> {
    if (this.closePromise !== undefined) return this.closePromise;
    this.closing = true;
    const childSerials = [
      ...this.actorSerials.values(),
      ...this.timerSerials.values()
    ];
    const actorExecutors = [...this.actorExecutors.values()];
    this.actorSerials.clear();
    this.timerSerials.clear();
    this.actorExecutors.clear();
    this.closePromise = Promise.all([
      this.spotSerial.close(),
      ...childSerials.map(serial => serial.close()),
      ...actorExecutors.map(executor => executor.close())
    ]).then(() => undefined);
    return this.closePromise;
  }

  private actorExecutor(actorId: string): ZLinkActorSerialExecutor {
    this.throwIfClosing();
    let executor = this.actorExecutors.get(actorId);
    if (executor === undefined) {
      executor = new ZLinkActorSerialExecutor(actorId, this.sourceSpotId, this.actorOptions);
      this.actorExecutors.set(actorId, executor);
    }
    return executor;
  }

  private actorSerial(actorId: string): ZLinkSpotSerialTurnExecutor {
    if (this.executionMode === ZLinkUserSpotExecutionMode.SpotWide) {
      return this.spotSerial;
    }
    this.throwIfClosing();
    let serial = this.actorSerials.get(actorId);
    if (serial === undefined) {
      serial = new ZLinkSpotSerialTurnExecutor(false);
      if (this.executionBarrier !== undefined) serial.setExecutionBarrier(this.executionBarrier);
      this.actorSerials.set(actorId, serial);
    }
    return serial;
  }

  private timerSerial(timerName: string): ZLinkSpotSerialTurnExecutor {
    if (this.executionMode === ZLinkUserSpotExecutionMode.SpotWide) {
      return this.spotSerial;
    }
    this.throwIfClosing();
    let serial = this.timerSerials.get(timerName);
    if (serial === undefined) {
      serial = new ZLinkSpotSerialTurnExecutor(false);
      if (this.executionBarrier !== undefined) serial.setExecutionBarrier(this.executionBarrier);
      this.timerSerials.set(timerName, serial);
    }
    return serial;
  }

  private throwIfClosing(): void {
    if (this.closing) throw new Error('The Spot serial executor is closed.');
  }
}
