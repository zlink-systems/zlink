import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import {
  ZLinkBoundedSerialScheduler,
  type ZLinkSerialSchedulerOptions,
  type ZLinkSerialWorkOptions,
  type ZLinkSerialWorkRecord
} from '../execution/serial-scheduler';

export class ZLinkActorDispatchMailbox {
  private readonly scheduler: ZLinkBoundedSerialScheduler;

  constructor(options?: ZLinkSerialSchedulerOptions) {
    this.scheduler = new ZLinkBoundedSerialScheduler(
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
    return this.scheduler.submit(operation, workOptions);
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

  constructor(private readonly options?: ZLinkSerialSchedulerOptions) {}

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
    return mailbox.submit(operation, workOptions);
  }
}
