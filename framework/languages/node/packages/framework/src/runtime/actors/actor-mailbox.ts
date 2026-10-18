import {
  ZLinkSerialExecutionQueue,
  type ZLinkSerialSchedulerOptions,
  type ZLinkSerialWorkPreparation,
  type ZLinkSerialWorkOptions,
  type ZLinkSerialWorkRecord
} from '../execution/serial-execution-queue';
import { runZLinkActorExecution } from './actor-execution-context';
import {
  bindApplicationJobPermit,
  currentApplicationJobPermit,
  runWithApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';
import type { ApplicationJobPermitPort } from '../application-jobs/contracts';

export class ZLinkActorSerialExecutor {
  private readonly scheduler: ZLinkSerialExecutionQueue;

  constructor(
    private readonly actorId: string,
    private readonly sourceSpotId: unknown,
    options?: ZLinkSerialSchedulerOptions,
    sharedOwner?: ZLinkSerialExecutionQueue
  ) {
    this.scheduler = new ZLinkSerialExecutionQueue(
      (record) => this.runRecord(record),
      options,
      sharedOwner
    );
  }

  execute<T>(operation: () => Promise<T> | T, workOptions?: ZLinkSerialWorkOptions): Promise<T> {
    const initial = currentApplicationJobPermit();
    if (
      initial !== undefined &&
      workOptions?.lane !== 'lifecycle' &&
      this.scheduler.hasPendingDurableApplication
    ) {
      let prepared: ApplicationJobPermitPort | undefined;
      const cancel = (): void => {
        prepared?.releaseAfterInternalProcessing();
        prepared = undefined;
      };
      const terminal = this.scheduler.admitDurablePrefix(
        () =>
          runWithApplicationJobPermit(prepared!, () =>
            runZLinkActorExecution(this.actorId, this.sourceSpotId, operation)
          ),
        workOptions,
        undefined,
        {
          prepare: async (signal) => {
            if (initial.reacquire === undefined) {
              throw new Error('Durable Actor ingress requires its host permit source.');
            }
            prepared = await initial.reacquire(signal);
            if (signal.aborted) {
              cancel();
              throw signal.reason;
            }
            prepared.markApplicationQueued();
          },
          cancel
        }
      );
      initial.releaseAfterInternalProcessing();
      return terminal;
    }
    const boundOperation = bindApplicationJobPermit(() =>
      runZLinkActorExecution(this.actorId, this.sourceSpotId, operation)
    );
    return initial !== undefined
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
        executeChild: <TChild>(child: () => Promise<TChild> | TChild) => Promise<TChild>
      ) => Promise<void>;
      readonly preparation?: ZLinkSerialWorkPreparation;
      readonly workOptions?: ZLinkSerialWorkOptions;
    }[]
  ): readonly Promise<void>[] {
    // No await occurs in this map: the whole durable prefix is appended in
    // one event-loop turn before later ingress can observe the released hold.
    return records.map((record) =>
      this.scheduler.admitDurablePrefix(
        () =>
          record.operation((child) =>
            runZLinkActorExecution(this.actorId, this.sourceSpotId, child)
          ),
        record.workOptions,
        undefined,
        record.preparation
      )
    );
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
