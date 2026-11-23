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
  currentApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';

export class ZLinkActorSerialExecutor {
  readonly scheduler: ZLinkSerialExecutionQueue;

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

  execute<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions,
    capture?: () => Promise<T> | T,
    preparation?: ZLinkSerialWorkPreparation
  ): Promise<T> {
    const initial = currentApplicationJobPermit();
    const packetAdmission = capture === undefined ? undefined : bindApplicationJobPermit(capture);
    const boundOperation = () => runZLinkActorExecution(this.actorId, this.sourceSpotId, operation);
    return initial !== undefined
      ? this.scheduler.submitPreAdmitted(
          boundOperation,
          workOptions,
          undefined,
          packetAdmission,
          preparation
        )
      : this.scheduler.submit(boundOperation, workOptions, undefined, packetAdmission, preparation);
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

  /** Moves accepted packet admissions into the active handoff in source FIFO order. */
  capturePacketPrefix(): void {
    while (this.scheduler.application.records.length > 0) {
      const record = this.scheduler.application.records[0]!;
      if (record.packetAdmission === undefined) break;
      const admission = record.packetAdmission();
      record.preparationController?.abort(new Error('Packet transferred to the Actor handoff.'));
      record.preparation?.cancel();
      this.scheduler.takeSelected(record);
      void Promise.resolve(admission).then(record.resolve, record.reject).finally(record.release);
    }
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
