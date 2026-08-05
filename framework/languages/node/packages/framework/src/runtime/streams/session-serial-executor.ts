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
    onRejected?: () => void
  ): boolean {
    if (this.closed) return false;
    void this.scheduler.submit(work, { ...options, lane }).catch(() => onRejected?.());
    return true;
  }

  run(work: () => Promise<void>): Promise<void> {
    if (this.closed) {
      return Promise.reject(createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
        'Session execution queue is closed.'
      ));
    }
    return this.scheduler.submit(work, { lane: 'lifecycle' });
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
