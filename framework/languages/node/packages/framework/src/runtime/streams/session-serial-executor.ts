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
    {
      capacityError: (lane) => createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.WorkerQueueFull,
        `Stream Session ${lane} execution capacity is full.`,
        true
      )
    }
  );
  private closed = false;

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

  executeInfrastructure(
    work: () => Promise<void>,
    options: Omit<ZLinkSerialWorkOptions, 'lane'> = {},
    onRejected?: (error?: unknown) => void
  ): boolean {
    return this.submit(work, 'lifecycle', options, onRejected);
  }

  executeFinal(work: () => Promise<void>): Promise<void> {
    if (this.closed) {
      return Promise.reject(createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
        'Session execution queue is closed.'
      ));
    }
    const boundWork = bindApplicationJobPermit(work);
    return hasApplicationJobPermit()
      ? this.scheduler.submitPreAdmitted(boundWork, { lane: 'lifecycle' })
      : this.scheduler.submit(boundWork, { lane: 'lifecycle' });
  }

  private submit(
    work: () => Promise<void>,
    lane: ZLinkSerialWorkLane,
    options: Omit<ZLinkSerialWorkOptions, 'lane'> = {},
    onRejected?: (error?: unknown) => void
  ): boolean {
    if (this.closed) return false;
    const serialOptions = { ...options, lane };
    const boundWork = bindApplicationJobPermit(work);
    const submitted = hasApplicationJobPermit()
      ? this.scheduler.submitPreAdmitted(boundWork, serialOptions)
      : this.scheduler.submit(boundWork, serialOptions);
    void submitted
      .catch(error => onRejected?.(error));
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
