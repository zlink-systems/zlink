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

interface ZLinkStreamSerialWorkContext {
  releaseIngressReservation(): void;
}

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
    onRejected?: () => void,
    onReservationReleased?: () => void
  ): boolean {
    if (this.closed) return false;
    let ingressReservationReleased = false;
    const releaseIngressReservation = (): void => {
      if (ingressReservationReleased) return;
      ingressReservationReleased = true;
      onReservationReleased?.();
    };
    void this.scheduler.submit(work, { ...options, lane }, {
      releaseIngressReservation
    } satisfies ZLinkStreamSerialWorkContext).catch(() => {
      try {
        onRejected?.();
      } finally {
        // Admission failures do not produce a record for execute() to release.
        releaseIngressReservation();
      }
    });
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
      try {
        record.release();
      } finally {
        (record.context as ZLinkStreamSerialWorkContext | undefined)
          ?.releaseIngressReservation();
      }
    }
  }
}
