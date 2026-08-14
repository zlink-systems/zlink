import {
  ZLinkBoundedSerialScheduler,
  type ZLinkSerialWorkLane,
  type ZLinkSerialWorkOptions,
  type ZLinkSerialWorkRecord
} from '../execution/serial-scheduler';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import {
  bindApplicationJobPermit,
  hasApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';

export class ZLinkStreamSessionSerialExecutor {
  private readonly scheduler = new ZLinkBoundedSerialScheduler(
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

  enqueue(
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

  run(work: () => Promise<void>): Promise<void> {
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
