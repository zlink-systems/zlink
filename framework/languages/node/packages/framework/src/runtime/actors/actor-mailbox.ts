import {
  ZLinkSerialExecutionQueue,
  type ZLinkSerialSchedulerOptions,
  type ZLinkSerialWorkPreparation,
  type ZLinkSerialWorkOptions,
  type ZLinkSerialWorkRecord
} from '../execution/serial-execution-queue';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { runZLinkActorExecution } from './actor-execution-context';
import {
  bindApplicationJobPermit,
  hasApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';

export class ZLinkActorSerialExecutor {
  private readonly scheduler: ZLinkSerialExecutionQueue;

  constructor(
    private readonly actorId: string,
    private readonly sourceSpotId: unknown,
    options?: ZLinkSerialSchedulerOptions
  ) {
    this.scheduler = new ZLinkSerialExecutionQueue(
      (record) => this.runRecord(record),
      {
        ...options,
        capacityError: options?.capacityError ?? (() =>
          createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.WorkerQueueFull,
            'Actor execution queue capacity was exceeded.'
          ))
      }
    );
  }

  execute<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    const boundOperation = bindApplicationJobPermit(() =>
      runZLinkActorExecution(this.actorId, this.sourceSpotId, operation));
    return hasApplicationJobPermit()
      ? this.scheduler.submitPreAdmitted(boundOperation, workOptions)
      : this.scheduler.submit(boundOperation, workOptions);
  }

  /**
   * Restores one non-executing durable prefix at the current Actor FIFO tail.
   * Child callbacks enter Actor execution independently after the prefix
   * reaches the head; the prefix itself never owns an application permit.
   *
   * @internal
   */
  admitDurablePrefix(
    records: readonly {
      readonly operation: (
        executeChild: <TChild>(
          child: () => Promise<TChild> | TChild
        ) => Promise<TChild>
      ) => Promise<void>;
      readonly preparation?: ZLinkSerialWorkPreparation;
      readonly workOptions?: ZLinkSerialWorkOptions;
    }[]
  ): readonly Promise<void>[] {
    // No await occurs in this map: the whole durable prefix is appended in
    // one event-loop turn before later ingress can observe the released hold.
    return records.map(record => this.scheduler.admitDurablePrefix(
      () => record.operation(child =>
        runZLinkActorExecution(this.actorId, this.sourceSpotId, child)),
      record.workOptions,
      undefined,
      record.preparation
    ));
  }

  close(): Promise<void> {
    return this.scheduler.close();
  }

  private async runRecord(record: ZLinkSerialWorkRecord<unknown>): Promise<void> {
    try {
      record.resolve(await record.operation());
    } catch (error) {
      record.reject(error);
    } finally {
      record.release();
    }
  }
}
