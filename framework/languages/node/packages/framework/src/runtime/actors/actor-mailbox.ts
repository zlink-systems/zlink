import {
  ZLinkSerialExecutionQueue,
  type ZLinkSerialSchedulerOptions,
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
