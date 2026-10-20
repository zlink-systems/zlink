import { AsyncLocalStorage } from 'node:async_hooks';
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
import { runZLinkExecutionArea } from '../execution';
import {
  ReadyDomain,
  type Claim,
  type ReadyBatch,
  type ReadyRecord,
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
// Captured while the runtime module is loaded, before any application owner
// exists. A ready callback may run inside a Spot turn; the shared Mesh pump
// must enter like an independent receive-loop task, without inheriting that
// owner's AsyncLocalStorage identity.
const detachedMeshDispatchScope = AsyncLocalStorage.snapshot();

export interface ZLinkMeshDispatchPumpOptions {
  readonly readyCapacity?: number;
  readonly messageCapacity?: number;
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

export class ZLinkMeshDispatchPump {
  private pendingDomains: number = ReadyDomain.None;
  private infrastructureScheduled = false;
  private disposed = false;
  private readonly activeDrains = new Set<Promise<void>>();
  private readonly idleApplicationWorkers = new Set<() => void>();
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
    this.idleApplicationWorkers.clear();
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
        setImmediate(() => this.wakeApplicationWorker());
      } else {
        this.wakeApplicationWorker();
      }
    }
  }

  private wakeApplicationWorker(): void {
    if (this.disposed || (this.pendingDomains & ReadyDomain.Application) === 0) return;
    const idle = this.idleApplicationWorkers.values().next().value as (() => void) | undefined;
    if (idle !== undefined) {
      this.idleApplicationWorkers.delete(idle);
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
      runZLinkExecutionArea('application', () => this.drainClaims(ReadyDomain.Application))
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
    return new Promise((resolve) => this.idleApplicationWorkers.add(resolve));
  }

  /**
   * Starts one record in this worker's turn; a permit's scope covers the
   * synchronous dispatch. Returns `false` when the dispatch threw (already
   * reported), `true` when it returned synchronously, or the handler's pending
   * completion, which the draining turn awaits itself before
   * {@link finishRecord}.
   */
  private dispatchRecord(
    owner: ReadyRecord,
    record: ReceiveRecord,
    permit: ApplicationJobPermitPort | undefined
  ): boolean | PromiseLike<void> {
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
          setImmediate(() => this.wakeApplicationWorker());
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
          const drain = this.drainClaims(ReadyDomain.Infrastructure, owner, claim);
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
  private async drainClaims(domain: number, owner?: ReadyRecord, claim?: Claim): Promise<void> {
    // Application workers claim one owner at a time; infrastructure supplies
    // the claim already obtained by its bounded lifecycle drain.
    const readyBatch =
      domain === ReadyDomain.Application ? this.node.createReadyBatch(1) : undefined;
    let receiveBatch =
      readyBatch === undefined
        ? undefined
        : this.node.createReceiveBatch(
            MESH_DISPATCH_RECEIVE_CAPACITY,
            this.options.partCapacity ?? DEFAULT_MESH_PART_CAPACITY
          );
    let claimedOwner = owner!;
    let claimed = claim!;
    try {
      do {
        if (readyBatch !== undefined) {
          // This boundary also keeps the first handler off the receive stack.
          await this.waitForApplicationReady();
          this.beginDrain();
        }
        try {
          for (;;) {
            if (readyBatch !== undefined) {
              if (this.disposed) break;
              readyBatch.reset();
              const drained = this.node.drainReady(
                ReadyDomain.Application,
                readyBatch,
                ZLINK_BACKEND_RECV_DONT_WAIT
              );
              if (!drained.ok || drained.records.length === 0) break;
              if (drained.hasResidue) this.markReady(ReadyDomain.Application);
              claimedOwner = drained.records[0]!;
              claimed = readyBatch.takeClaim(0);
            }
            try {
              receiveBatch ??= this.node.createReceiveBatch(
                MESH_DISPATCH_RECEIVE_CAPACITY,
                this.options.partCapacity ?? DEFAULT_MESH_PART_CAPACITY
              );
              const capacity =
                claimedOwner.ordinaryIngressPreAdmitted === true
                  ? MESH_DISPATCH_RECEIVE_CAPACITY
                  : 1;
              let claimPermit =
                claimedOwner.terminalCompletion === true ||
                claimedOwner.ordinaryIngressPreAdmitted === true ||
                domain === ReadyDomain.Infrastructure
                  ? undefined
                  : await this.acquirePermit();
              try {
                if (this.capacityStop.signal.aborted) return;
                receiveBatch.reset(capacity);
                const received = claimed.recvBatch(receiveBatch, ZLINK_BACKEND_RECV_DONT_WAIT);
                if (received.ok) {
                  // The bounded batch stays in claim order until its records finish.
                  try {
                    for (let index = 0; index < received.records.length; index += 1) {
                      const record = received.records[index]!;
                      if (this.disposed) break;
                      const permit = record.applicationJobPermit ?? claimPermit;
                      if (
                        claimedOwner.ordinaryIngressPreAdmitted === true &&
                        permit === undefined
                      ) {
                        throw new Error(
                          'Pre-admitted raw ingress record lost its Application Job Queue permit.'
                        );
                      }
                      this.recordsSinceYield += 1;
                      if (permit !== undefined) {
                        if (
                          domain === ReadyDomain.Application &&
                          record.applicationJobPermit === undefined
                        )
                          permit.markApplicationQueued();
                        // The scope now owns this reservation, including failure
                        // and detached handler handoff. Only unused permits remain
                        // owned by the receive attempt's finally block.
                        if (permit === claimPermit) claimPermit = undefined;
                      }
                      // The record's terminal and the permit turn release follow the
                      // handler in this turn's own continuation.
                      const dispatched = this.dispatchRecord(claimedOwner, record, permit);
                      if (dispatched === false) continue;
                      if (dispatched !== true) {
                        try {
                          await dispatched;
                        } catch (error) {
                          this.failRecord(error, record, permit);
                          continue;
                        }
                      }
                      const finished = this.finishRecord(record, permit);
                      if (finished !== undefined) await finished;
                    }
                  } finally {
                    // Batch ownership includes records whose dispatch never began,
                    // including on failure or shutdown in the middle of the batch.
                    for (let index = 0; index < received.records.length; index += 1) {
                      const record = received.records[index]!;
                      for (let partIndex = 0; partIndex < record.parts.length; partIndex += 1)
                        record.parts[partIndex]!.close();
                      record.releaseRetainedIngress?.();
                    }
                  }
                }
              } finally {
                claimPermit?.releaseAfterInternalProcessing();
              }
            } finally {
              claimed.release();
            }
            // Returning a claim remains an asynchronous worker boundary even
            // when its shared macrotask budget has not expired. The worker owns
            // that continuation directly, without a separate drain task.
            await this.yieldIfNeeded();
            if (readyBatch === undefined) return;
            // Empty ready claims count toward the shared pump budget too.
            this.recordsSinceYield += 1;
            const nextTurn = this.yieldIfNeeded();
            if (nextTurn !== undefined) await nextTurn;
          }
        } catch (error) {
          this.reportDispatchError(error);
        } finally {
          if (readyBatch !== undefined) {
            readyBatch.reset();
            this.draining -= 1;
          }
        }
        if (this.disposed) return;
      } while (readyBatch !== undefined);
    } finally {
      receiveBatch?.close();
      readyBatch?.close();
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
