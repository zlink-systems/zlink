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

export class ZLinkActorDispatchMailbox {
  private readonly scheduler: ZLinkSerialExecutionQueue;

  constructor(options?: ZLinkSerialSchedulerOptions) {
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

  submit<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    const boundOperation = bindApplicationJobPermit(operation);
    return hasApplicationJobPermit()
      ? this.scheduler.submitPreAdmitted(boundOperation, workOptions)
      : this.scheduler.submit(boundOperation, workOptions);
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

export class ZLinkActorDispatchMailboxSet {
  private readonly mailboxes = new Map<string, ZLinkActorDispatchMailbox>();

  constructor(
    private readonly sourceSpotId: unknown,
    private readonly options?: ZLinkSerialSchedulerOptions
  ) {}

  submit<T>(
    actorId: string,
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    let mailbox = this.mailboxes.get(actorId);
    if (mailbox === undefined) {
      mailbox = new ZLinkActorDispatchMailbox(this.options);
      this.mailboxes.set(actorId, mailbox);
    }
    return mailbox.submit(
      () => runZLinkActorExecution(actorId, this.sourceSpotId, operation),
      workOptions
    );
  }
}
