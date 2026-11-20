import {
  ZLinkSerialExecutionQueue,
  type ZLinkSerialWorkLane,
  type ZLinkSerialWorkOptions,
  type ZLinkSerialWorkRecord
} from '../execution/serial-execution-queue';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import {
  bindApplicationJobPermit,
  hasApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';

export class ZLinkSessionSerialExecutor {
  private readonly scheduler = new ZLinkSerialExecutionQueue(
    async (record) => this.execute(record),
    {}
  );
  private closed = false;

  shutdownSnapshot(): Record<string, unknown> {
    return {
      serialClosed: this.closed,
      serialActive: this.scheduler.diagnosticActive,
      serialActiveLifecycle: this.scheduler.activeLifecycle !== undefined
    };
  }

  executeApplication(
    work: () => Promise<void>,
    options: Omit<ZLinkSerialWorkOptions, 'lane'> = {},
    onRejected?: (error?: unknown) => void
  ): boolean {
    return this.submit(work, 'application', options, onRejected);
  }

  executeControl(
    work: () => Promise<void>,
    options: Omit<ZLinkSerialWorkOptions, 'lane'> = {},
    onRejected?: (error?: unknown) => void
  ): boolean {
    return this.submit(work, 'lifecycle', options, onRejected);
  }

  executeFinal(work: () => Promise<void>): Promise<void> {
    if (this.closed) {
      return Promise.reject(
        createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
          'Session execution queue is closed.'
        )
      );
    }
    return hasApplicationJobPermit()
      ? this.scheduler.submitPreAdmitted(work, { lane: 'application' })
      : this.scheduler.submit(bindApplicationJobPermit(work), { lane: 'application' });
  }

  private submit(
    work: () => Promise<void>,
    lane: ZLinkSerialWorkLane,
    options: Omit<ZLinkSerialWorkOptions, 'lane'> = {},
    onRejected?: (error?: unknown) => void
  ): boolean {
    if (this.closed) return false;
    const serialOptions = { ...options, lane };
    const submitted = hasApplicationJobPermit()
      ? this.scheduler.submitPreAdmitted(work, serialOptions)
      : this.scheduler.submit(bindApplicationJobPermit(work), serialOptions);
    void submitted.catch((error) => onRejected?.(error));
    return true;
  }

  async dispose(): Promise<void> {
    this.closed = true;
    await this.scheduler.whenIdle();
  }

  private async execute(record: ZLinkSerialWorkRecord<unknown>): Promise<void> {
    try {
      record.resolve(await record.operation());
    } catch (error) {
      record.reject(error);
    } finally {
      record.release();
    }
  }
}
