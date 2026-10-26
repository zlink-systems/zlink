import { awaitWithAbort } from '../abort';
import { availableParallelism } from 'node:os';
import {
  enterApplicationJobPermitScope,
  leaveApplicationJobPermitScope,
  releaseApplicationJobPermitTurn
} from '../application-jobs/application-job-queue-scope';
import type {
  ApplicationJobPermitPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';
import { detachedMeshDispatchScope, runZLinkExecutionArea } from '../execution';
import {
  ReadyDomain,
  type Claim,
  type ReadyBatch,
  type ReadyRecord,
  type ReceiveBatch,
  type ReceiveRecord
} from '../foundation/service-runtime-contracts';
import {
  SERVICE_WIRE_COMMAND_OFFSET,
  SERVICE_WIRE_MAJOR_OFFSET,
  SERVICE_WIRE_PREFIX_SIZE
} from '../foundation/service-wire-binary-primitives';
import {
  SERVICE_WIRE_MAGIC,
  SERVICE_WIRE_MAJOR
} from '../foundation/service-wire-constants.generated';
import type { ZLinkBackendMeshNode } from './contracts';
import { ZLINK_BACKEND_RECV_DONT_WAIT } from './runtime-values';
const DEFAULT_MESH_READY_CAPACITY = 32;

const DEFAULT_MESH_PART_CAPACITY = 256;

const MESH_DISPATCH_YIELD_RECORDS = 16;
const MESH_DISPATCH_YIELD_INTERVAL_MS = 2;
const MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET = 4;
const MESH_DISPATCH_RECEIVE_CAPACITY = 64;
const MESH_DISPATCH_MAX_APPLICATION_WORKERS = Math.max(2, availableParallelism());

export interface ZLinkMeshDispatchPumpOptions {
  readonly readyCapacity?: number;
  readonly partCapacity?: number;
  readonly dispatch: (owner: ReadyRecord, record: ReceiveRecord) => void | Promise<void>;
  readonly applicationJobQueue: ApplicationJobQueuePort;
  readonly reportError?: (error: unknown, context?: ZLinkMeshDispatchFailureContext) => void;
  readonly monotonicNowMs?: () => number;
}

export interface ZLinkMeshDispatchFailureContext {
  readonly receiveKind: number;
  readonly operationKind: number;
  readonly packetName?: string;
  readonly sourceNodeRid?: string;
  readonly commandId?: number;
}

interface MeshClaimDrainState {
  readonly suppliedReceiveBatch: ReceiveBatch | undefined;
  receiveBatch: ReceiveBatch | undefined;
  domain: number;
  owner: ReadyRecord | undefined;
  claim: Claim | undefined;
  claimPermit: ApplicationJobPermitPort | undefined;
  records: readonly ReceiveRecord[] | undefined;
  index: number;
  readonly receive: (permit?: ApplicationJobPermitPort) => Promise<void> | undefined;
  readonly dispatchRecords: () => Promise<void> | undefined;
  readonly finish: () => void;
}

export class ZLinkMeshDispatchPump {
  private pendingDomains: number = ReadyDomain.None;
  private infrastructureScheduled = false;
  private disposed = false;
  private readonly activeDrains = new Set<Promise<void>>();
  private readonly idleApplicationWorkers: Array<() => void> = [];
  private applicationWorkerCount = 0;
  private readonly capacityStop = new AbortController();
  private recordsSinceYield = 0;
  private yieldStartedAtMs = 0;
  /** Drains in progress; the batch budget begins when work starts on an idle pump. */
  private draining = 0;
  /** The macrotask boundary every drain waits for once the shared budget is spent. */
  private pendingYield?: Promise<void>;

  constructor(
    private readonly node: ZLinkBackendMeshNode,
    private readonly options: ZLinkMeshDispatchPumpOptions
  ) {}

  start(): void {
    if (this.applicationWorkerCount === 0) this.startApplicationWorker();
    this.node.setReadyHandler((domains) => {
      if (this.disposed) {
        return ReadyDomain.None;
      }
      this.markReady(domains);
      return domains;
    });
  }

  async dispose(signal?: AbortSignal): Promise<void> {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.capacityStop.abort();
    this.pendingDomains = ReadyDomain.None;
    for (const wake of this.idleApplicationWorkers) wake();
    this.idleApplicationWorkers.length = 0;
    while (this.activeDrains.size > 0) {
      await awaitWithAbort(Promise.all([...this.activeDrains]), signal);
    }
  }

  private markReady(domains: number): void {
    if (domains === ReadyDomain.None || this.disposed) return;
    const previous = this.pendingDomains;
    this.pendingDomains |= domains;
    const startInfrastructure =
      (domains & ReadyDomain.Infrastructure) !== 0 && (previous & ReadyDomain.Infrastructure) === 0;
    const startApplication =
      (domains & ReadyDomain.Application) !== 0 && (previous & ReadyDomain.Application) === 0;
    if (startInfrastructure && startApplication) {
      const startTurn = this.yieldIfNeeded() ?? Promise.resolve();
      this.scheduleInfrastructure(startTurn);
      return;
    }
    if (startInfrastructure) this.scheduleInfrastructure();
    if (startApplication) {
      if ((domains & ReadyDomain.Infrastructure) !== 0) {
        this.scheduleApplicationWorker();
      } else {
        this.wakeApplicationWorker();
      }
    }
  }

  private scheduleApplicationWorker(): void {
    setImmediate(() => this.wakeApplicationWorker());
  }

  private wakeApplicationWorker(): void {
    if (this.disposed || (this.pendingDomains & ReadyDomain.Application) === 0) return;
    const idle = this.idleApplicationWorkers.shift();
    if (idle !== undefined) {
      this.pendingDomains &= ~ReadyDomain.Application;
      idle();
      return;
    }
    if (this.applicationWorkerCount < MESH_DISPATCH_MAX_APPLICATION_WORKERS) {
      this.startApplicationWorker();
    }
  }

  private startApplicationWorker(): void {
    this.applicationWorkerCount += 1;
    let worker: Promise<void>;
    worker = detachedMeshDispatchScope(() =>
      runZLinkExecutionArea('application', () => this.runApplicationWorker())
    ).finally(() => {
      this.applicationWorkerCount -= 1;
      this.activeDrains.delete(worker);
    });
    this.activeDrains.add(worker);
  }

  private waitForApplicationReady(): Promise<void> {
    if (this.disposed) return Promise.resolve();
    if ((this.pendingDomains & ReadyDomain.Application) !== 0) {
      this.pendingDomains &= ~ReadyDomain.Application;
      return Promise.resolve();
    }
    return new Promise((resolve) => this.idleApplicationWorkers.push(resolve));
  }

  /**
   * Starts one record in this worker's turn; a permit's scope covers the
   * synchronous dispatch. Returns `false` when preparation rejects the record,
   * shutdown starts, or dispatch throws (already reported), `true` when dispatch
   * returns synchronously, or the pending preparation/handler completion,
   * which the draining turn awaits itself before
   * {@link finishRecord}.
   */
  private dispatchRecord(
    owner: ReadyRecord,
    record: ReceiveRecord,
    permit: ApplicationJobPermitPort | undefined
  ): boolean | PromiseLike<void | boolean> {
    if (record.prepareDispatch === undefined) return this.invokeRecord(owner, record, permit);
    try {
      const preparation = record.prepareDispatch(this.capacityStop.signal);
      return preparation instanceof Promise
        ? this.continuePreparingRecord(owner, record, permit, preparation)
        : this.invokeRecord(owner, record, permit, preparation);
    } catch (error) {
      this.failRecord(error, record, permit);
      return false;
    }
  }

  private continuePreparingRecord(
    owner: ReadyRecord,
    record: ReceiveRecord,
    permit: ApplicationJobPermitPort | undefined,
    preparation: Promise<boolean>
  ): Promise<void | boolean> {
    return preparation.then<void | boolean>((accepted) =>
      this.invokeRecord(owner, record, permit, accepted)
    );
  }

  private invokeRecord(
    owner: ReadyRecord,
    record: ReceiveRecord,
    permit: ApplicationJobPermitPort | undefined,
    accepted = true
  ): boolean | PromiseLike<void> {
    if (!accepted || this.capacityStop.signal.aborted) {
      releaseApplicationJobPermitTurn(permit);
      return false;
    }
    const previous = permit === undefined ? undefined : enterApplicationJobPermitScope(permit);
    let result: void | PromiseLike<void>;
    try {
      result = this.options.dispatch(owner, record);
    } catch (error) {
      this.failRecord(error, record, permit);
      return false;
    } finally {
      if (permit !== undefined) leaveApplicationJobPermitScope(previous);
    }
    return result ?? true;
  }

  private failRecord(
    error: unknown,
    record: ReceiveRecord,
    permit: ApplicationJobPermitPort | undefined
  ): void {
    releaseApplicationJobPermitTurn(permit);
    this.reportDispatchError(error, meshDispatchFailureContext(record));
  }

  /** Runs the record's terminal completion, then releases its permit turn. */
  private finishRecord(
    record: ReceiveRecord,
    permit: ApplicationJobPermitPort | undefined
  ): void | Promise<void> {
    let result: void | Promise<void> = undefined;
    try {
      result = record.onTerminalCompletion?.();
    } catch (error) {
      this.reportDispatchError(error, meshDispatchFailureContext(record));
    }
    if (result === undefined) {
      releaseApplicationJobPermitTurn(permit);
      return;
    }
    return this.finishPendingRecord(result, record, permit);
  }

  private finishPendingRecord(
    result: Promise<void>,
    record: ReceiveRecord,
    permit: ApplicationJobPermitPort | undefined
  ): Promise<void> {
    return Promise.resolve(result).then(
      () => releaseApplicationJobPermitTurn(permit),
      (error) => {
        this.reportDispatchError(error, meshDispatchFailureContext(record));
        releaseApplicationJobPermitTurn(permit);
      }
    );
  }

  private reportDispatchError(error: unknown, context?: ZLinkMeshDispatchFailureContext): void {
    this.options.reportError?.(error, context);
  }

  /**
   * Infrastructure controls must not wait for an application permit. During
   * source draining an application claim can legitimately remain parked at
   * `acquirePermit`; sharing that turn would strand Ready/State/Cutover in
   * the infrastructure mailbox and prevent the relocation coordinator from
   * completing the drain that releases the application work.
   */
  private scheduleInfrastructure(startTurn?: Promise<void>): void {
    if (this.infrastructureScheduled) {
      return;
    }
    this.infrastructureScheduled = true;
    const drain = (startTurn ?? this.yieldIfNeeded() ?? Promise.resolve())
      .then(() =>
        detachedMeshDispatchScope(() =>
          runZLinkExecutionArea('infrastructure', () => this.drainInfrastructure())
        )
      )
      .catch((error) => this.reportDispatchError(error));
    this.activeDrains.add(drain);
    void drain.finally(() => {
      this.activeDrains.delete(drain);
      this.infrastructureScheduled = false;
      if (!this.disposed && (this.pendingDomains & ReadyDomain.Infrastructure) !== 0) {
        this.scheduleInfrastructure();
      }
    });
  }

  private async drainInfrastructure(): Promise<void> {
    const readyBatch = this.node.createReadyBatch(
      Math.min(
        this.options.readyCapacity ?? DEFAULT_MESH_READY_CAPACITY,
        MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET
      )
    );
    try {
      while (!this.disposed) {
        if ((this.pendingDomains & ReadyDomain.Infrastructure) === 0) {
          return;
        }
        this.pendingDomains &= ~ReadyDomain.Infrastructure;
        if ((this.pendingDomains & ReadyDomain.Application) !== 0) {
          this.scheduleApplicationWorker();
        }
        const lifecycleBudgetExhausted = await this.drainDomain(readyBatch);
        if (lifecycleBudgetExhausted) {
          this.pendingDomains |= ReadyDomain.Infrastructure;
          await yieldToEventLoop();
        }
      }
    } finally {
      readyBatch.close();
    }
  }

  private async drainDomain(readyBatch: ReadyBatch): Promise<boolean> {
    let claimsDrained = 0;
    this.beginDrain();
    try {
      while (!this.disposed) {
        if (claimsDrained >= MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET) {
          // Lifecycle work has priority, but a bounded turn lets application
          // work make progress when lifecycle records keep arriving.
          return true;
        }
        readyBatch.reset();
        const drained = this.node.drainReady(
          ReadyDomain.Infrastructure,
          readyBatch,
          ZLINK_BACKEND_RECV_DONT_WAIT
        );
        if (!drained.ok || drained.records.length === 0) {
          return false;
        }
        for (let index = 0; index < drained.records.length; index += 1) {
          const claim = readyBatch.takeClaim(index);
          claimsDrained += 1;
          const owner = drained.records[index]!;
          this.beginDrain();
          const drain = this.drainClaim(ReadyDomain.Infrastructure, owner, claim);
          this.activeDrains.add(drain);
          void drain.finally(() => {
            this.draining -= 1;
            this.activeDrains.delete(drain);
          });
        }
        if (claimsDrained >= MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET) {
          this.wakeApplicationWorker();
          return true;
        }
        // Empty ready claims also count as work, so they cannot monopolize
        // the microtask queue without giving I/O and deadlines a turn.
        this.recordsSinceYield += 1;
        const yieldTurn = this.yieldIfNeeded();
        if (yieldTurn !== undefined) await yieldTurn;
      }
      return false;
    } finally {
      readyBatch.reset();
      this.draining -= 1;
    }
  }

  /**
   * Time the pump spent idle is not batch work: when work starts on an idle
   * pump, its batch budget starts with it (spec 08 §6).
   */
  private beginDrain(): void {
    if (this.draining === 0) {
      this.recordsSinceYield = 0;
      this.yieldStartedAtMs = this.nowMs();
    }
    this.draining += 1;
  }

  /** Each worker owns claim continuations for its entire lifetime. */
  private async runApplicationWorker(): Promise<void> {
    const readyBatch = this.node.createReadyBatch(1);
    const receiveBatch = this.node.createReceiveBatch(
      MESH_DISPATCH_RECEIVE_CAPACITY,
      this.options.partCapacity ?? DEFAULT_MESH_PART_CAPACITY
    );
    const claimDrain = this.createClaimDrain(receiveBatch);
    try {
      for (;;) {
        // This boundary also keeps the first handler off the receive stack.
        await this.waitForApplicationReady();
        this.beginDrain();
        try {
          while (!this.disposed) {
            readyBatch.reset();
            const drained = this.node.drainReady(
              ReadyDomain.Application,
              readyBatch,
              ZLINK_BACKEND_RECV_DONT_WAIT
            );
            if (!drained.ok || drained.records.length === 0) break;
            if (drained.hasResidue) this.markReady(ReadyDomain.Application);
            const pending = this.dispatchClaim(
              ReadyDomain.Application,
              drained.records[0]!,
              readyBatch.takeClaim(0),
              claimDrain
            );
            if (pending !== undefined) await pending;
            // Returning a claim remains an asynchronous worker boundary without
            // allocating a separate async drain task for a synchronous claim.
            await this.yieldIfNeeded();
            // Empty ready claims count toward the shared pump budget too.
            this.recordsSinceYield += 1;
            const nextTurn = this.yieldIfNeeded();
            if (nextTurn !== undefined) await nextTurn;
          }
        } catch (error) {
          this.reportDispatchError(error);
        } finally {
          readyBatch.reset();
          this.draining -= 1;
        }
        if (this.disposed) return;
      }
    } finally {
      receiveBatch.close();
      readyBatch.close();
    }
  }

  private async drainClaim(
    domain: number,
    owner: ReadyRecord,
    claim: Claim,
    receiveBatch?: ReceiveBatch
  ): Promise<void> {
    try {
      const pending = this.dispatchClaim(domain, owner, claim, this.createClaimDrain(receiveBatch));
      if (pending !== undefined) await pending;
      await this.yieldIfNeeded();
    } catch (error) {
      this.reportDispatchError(error);
    }
  }

  private createClaimDrain(suppliedReceiveBatch?: ReceiveBatch): MeshClaimDrainState {
    const state: MeshClaimDrainState = {
      suppliedReceiveBatch,
      receiveBatch: suppliedReceiveBatch,
      domain: ReadyDomain.None,
      owner: undefined,
      claim: undefined,
      claimPermit: undefined,
      records: undefined,
      index: 0,
      receive: (permit) => this.receiveClaim(state, permit),
      dispatchRecords: () => this.dispatchClaimRecords(state),
      finish: () => this.finishClaim(state)
    };
    return state;
  }

  /** A synchronous claim finishes without an async wrapper or a Promise. */
  private dispatchClaim(
    domain: number,
    owner: ReadyRecord,
    claim: Claim,
    state: MeshClaimDrainState = this.createClaimDrain()
  ): Promise<void> | undefined {
    state.domain = domain;
    state.owner = owner;
    state.claim = claim;
    state.index = 0;
    let pending: Promise<void> | undefined;
    try {
      state.receiveBatch ??= this.node.createReceiveBatch(
        MESH_DISPATCH_RECEIVE_CAPACITY,
        this.options.partCapacity ?? DEFAULT_MESH_PART_CAPACITY
      );
      pending =
        owner.terminalCompletion === true ||
        owner.ordinaryIngressPreAdmitted === true ||
        domain === ReadyDomain.Infrastructure
          ? state.receive()
          : this.acquirePermit().then(state.receive);
      if (pending !== undefined) return pending.finally(state.finish);
      return undefined;
    } finally {
      if (pending === undefined) state.finish();
    }
  }

  private receiveClaim(
    state: MeshClaimDrainState,
    permit?: ApplicationJobPermitPort
  ): Promise<void> | undefined {
    state.claimPermit = permit;
    if (this.capacityStop.signal.aborted) return undefined;
    const capacity =
      state.owner!.ordinaryIngressPreAdmitted === true ? MESH_DISPATCH_RECEIVE_CAPACITY : 1;
    state.receiveBatch!.reset(capacity);
    const received = state.claim!.recvBatch(state.receiveBatch!, ZLINK_BACKEND_RECV_DONT_WAIT);
    if (!received.ok) return undefined;
    state.records = received.records;
    return this.dispatchClaimRecords(state);
  }

  private dispatchClaimRecords(state: MeshClaimDrainState): Promise<void> | undefined {
    const records = state.records!;
    const owner = state.owner!;
    while (state.index < records.length && !this.disposed) {
      const record = records[state.index++]!;
      const permit = record.applicationJobPermit ?? state.claimPermit;
      if (owner.ordinaryIngressPreAdmitted === true && permit === undefined) {
        throw new Error('Pre-admitted raw ingress record lost its Application Job Queue permit.');
      }
      this.recordsSinceYield += 1;
      if (permit !== undefined) {
        if (state.domain === ReadyDomain.Application && record.applicationJobPermit === undefined)
          permit.markApplicationQueued();
        // Only reservations not transferred to a record remain claim-owned.
        if (permit === state.claimPermit) state.claimPermit = undefined;
      }
      const dispatched = this.dispatchRecord(owner, record, permit);
      if (dispatched === false) continue;
      if (dispatched !== true) {
        return this.continuePendingClaim(state, dispatched, record, permit);
      }
      const finished = this.finishRecord(record, permit);
      if (finished !== undefined) return finished.then(state.dispatchRecords);
    }
    return undefined;
  }

  private continuePendingClaim(
    state: MeshClaimDrainState,
    dispatched: PromiseLike<void | boolean>,
    record: ReceiveRecord,
    permit: ApplicationJobPermitPort | undefined
  ): Promise<void> {
    return Promise.resolve(dispatched).then(
      (accepted) => {
        if (accepted === false) return state.dispatchRecords();
        const finished = this.finishRecord(record, permit);
        return finished === undefined
          ? state.dispatchRecords()
          : finished.then(state.dispatchRecords);
      },
      (error) => {
        this.failRecord(error, record, permit);
        return state.dispatchRecords();
      }
    );
  }

  private finishClaim(state: MeshClaimDrainState): void {
    try {
      try {
        // Ownership includes records whose dispatch never began on failure or shutdown.
        const records = state.records;
        if (records !== undefined) {
          for (let recordIndex = 0; recordIndex < records.length; recordIndex += 1) {
            const record = records[recordIndex]!;
            for (let partIndex = 0; partIndex < record.parts.length; partIndex += 1)
              record.parts[partIndex]!.close();
            record.releaseRetainedIngress?.();
          }
        }
      } finally {
        state.claimPermit?.releaseAfterInternalProcessing();
      }
    } finally {
      try {
        state.claim!.release();
      } finally {
        try {
          if (state.suppliedReceiveBatch === undefined) state.receiveBatch?.close();
        } finally {
          // An idle worker must not retain the previous claim's payload or owner.
          state.records = undefined;
          state.owner = undefined;
          state.claim = undefined;
          state.claimPermit = undefined;
        }
      }
    }
  }

  /**
   * The batch budget is shared by every drain of this pump, so once it is
   * spent every drain waits for the same macrotask boundary; one drain that
   * keeps running would otherwise hold the event loop the yield was for.
   */
  private yieldIfNeeded(): Promise<void> | undefined {
    if (this.pendingYield !== undefined) return this.pendingYield;
    const nowMs = this.nowMs();
    if (
      this.recordsSinceYield < MESH_DISPATCH_YIELD_RECORDS &&
      nowMs - this.yieldStartedAtMs < MESH_DISPATCH_YIELD_INTERVAL_MS
    )
      return undefined;
    this.recordsSinceYield = 0;
    const pendingYield = yieldToTimers().then(() => {
      this.pendingYield = undefined;
      this.yieldStartedAtMs = this.nowMs();
    });
    this.pendingYield = pendingYield;
    return pendingYield;
  }

  private nowMs(): number {
    return this.options.monotonicNowMs?.() ?? performance.now();
  }

  private async acquirePermit(): Promise<ApplicationJobPermitPort | undefined> {
    try {
      return await this.options.applicationJobQueue.acquire(this.capacityStop.signal, 'remote');
    } catch (error) {
      if (this.disposed || this.capacityStop.signal.aborted) return undefined;
      throw error;
    }
  }
}

function meshDispatchFailureContext(record: ReceiveRecord): ZLinkMeshDispatchFailureContext {
  const commandId = serviceWireCommand(record);
  return {
    receiveKind: record.kind,
    operationKind: record.operationKind,
    ...(record.packetName === undefined ? {} : { packetName: record.packetName }),
    ...(record.sourceNodeRid === null ? {} : { sourceNodeRid: String(record.sourceNodeRid) }),
    ...(commandId === undefined ? {} : { commandId })
  };
}

function serviceWireCommand(record: ReceiveRecord): number | undefined {
  if (record.parts.length !== 1) return undefined;
  const bytes = record.parts[0]!.data();
  return bytes.byteLength >= SERVICE_WIRE_PREFIX_SIZE &&
    bytes[0] === SERVICE_WIRE_MAGIC[0] &&
    bytes[1] === SERVICE_WIRE_MAGIC[1] &&
    bytes[SERVICE_WIRE_MAJOR_OFFSET] === SERVICE_WIRE_MAJOR
    ? bytes[SERVICE_WIRE_COMMAND_OFFSET]
    : undefined;
}

function yieldToEventLoop(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function yieldToTimers(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 0));
}
