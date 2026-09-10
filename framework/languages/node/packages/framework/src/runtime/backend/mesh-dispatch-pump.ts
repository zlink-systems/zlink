import { AsyncLocalStorage } from 'node:async_hooks';
import { availableParallelism } from 'node:os';
import { ZLINK_BACKEND_RECV_DONT_WAIT } from './runtime-values';
import {
  ReadyDomain,
  type ReadyRecord,
  type ReceiveRecord
} from '../foundation/service-runtime-contracts';
import type { ZLinkBackendMeshNode } from './contracts';
import { runZLinkExecutionArea } from '../execution';
import type {
  ApplicationJobPermitPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';
import { runWithApplicationJobPermit } from '../application-jobs/application-job-queue-scope';

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

class ZLinkMeshDispatchFailure extends Error {
  constructor(
    readonly context: ZLinkMeshDispatchFailureContext,
    readonly dispatchCause: unknown
  ) {
    super('Mesh record dispatch failed.', { cause: dispatchCause });
    this.name = 'ZLinkMeshDispatchFailure';
  }
}

export class ZLinkMeshDispatchPump {
  private pendingDomains: number = ReadyDomain.None;
  private infrastructureScheduled = false;
  private disposed = false;
  private infrastructureDrainPromise?: Promise<void>;
  private readonly activeDrains = new Set<Promise<void>>();
  private readonly idleApplicationWorkers = new Set<() => void>();
  private applicationWorkerCount = 0;
  private readonly capacityStop = new AbortController();
  private recordsSinceYield = 0;
  private yieldStartedAtMs = 0;

  constructor(
    private readonly node: ZLinkBackendMeshNode,
    private readonly options: ZLinkMeshDispatchPumpOptions
  ) {
  }

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

  async dispose(): Promise<void> {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.capacityStop.abort();
    this.pendingDomains = ReadyDomain.None;
    for (const wake of this.idleApplicationWorkers) wake();
    this.idleApplicationWorkers.clear();
    while (this.activeDrains.size > 0) {
      await Promise.all([...this.activeDrains]);
    }
  }

  private markReady(domains: number): void {
    if (domains === ReadyDomain.None || this.disposed) return;
    const previous = this.pendingDomains;
    this.pendingDomains |= domains;
    const startInfrastructure =
      (domains & ReadyDomain.Infrastructure) !== 0
      && (previous & ReadyDomain.Infrastructure) === 0;
    const startApplication =
      (domains & ReadyDomain.Application) !== 0
      && (previous & ReadyDomain.Application) === 0;
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
      runZLinkExecutionArea('application', () => this.runApplicationWorker()))
      .finally(() => {
        this.applicationWorkerCount -= 1;
        this.activeDrains.delete(worker);
      });
    this.activeDrains.add(worker);
  }

  private async runApplicationWorker(): Promise<void> {
    for (;;) {
      await this.waitForApplicationReady();
      if (this.disposed) return;
      try {
        await this.drainDomain(ReadyDomain.Application);
      } catch (error) {
        this.reportDispatchError(error);
      }
    }
  }

  private waitForApplicationReady(): Promise<void> {
    if (this.disposed) return Promise.resolve();
    if ((this.pendingDomains & ReadyDomain.Application) !== 0) {
      this.pendingDomains &= ~ReadyDomain.Application;
      return Promise.resolve();
    }
    return new Promise(resolve => this.idleApplicationWorkers.add(resolve));
  }

  private reportDispatchError(error: unknown): void {
    if (error instanceof ZLinkMeshDispatchFailure) {
      this.options.reportError?.(error.dispatchCause, error.context);
      return;
    }
    this.options.reportError?.(error);
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
      .then(() => detachedMeshDispatchScope(() =>
        runZLinkExecutionArea('infrastructure', () => this.drainInfrastructure())))
      .catch((error) => {
        if (error instanceof ZLinkMeshDispatchFailure) {
          this.options.reportError?.(error.dispatchCause, error.context);
          return;
        }
        this.options.reportError?.(error);
      });
    this.activeDrains.add(drain);
    this.infrastructureDrainPromise = drain;
    void drain.finally(() => {
      this.activeDrains.delete(drain);
      if (this.infrastructureDrainPromise === drain) {
        this.infrastructureDrainPromise = undefined;
        this.infrastructureScheduled = false;
        if (!this.disposed && (this.pendingDomains & ReadyDomain.Infrastructure) !== 0) {
          this.scheduleInfrastructure();
        }
      }
    });
  }

  private async drainInfrastructure(): Promise<void> {
    while (!this.disposed) {
      if ((this.pendingDomains & ReadyDomain.Infrastructure) === 0) {
        return;
      }
      this.pendingDomains &= ~ReadyDomain.Infrastructure;
      if ((this.pendingDomains & ReadyDomain.Application) !== 0) {
        setImmediate(() => this.wakeApplicationWorker());
      }
      const lifecycleBudgetExhausted = await this.drainDomain(
        ReadyDomain.Infrastructure,
        MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET
      );
      if (lifecycleBudgetExhausted) {
        this.pendingDomains |= ReadyDomain.Infrastructure;
        await yieldToEventLoop();
      }
    }
  }

  private async drainDomain(domain: number, claimBudget?: number): Promise<boolean> {
    const readyCapacity = domain === ReadyDomain.Application
      ? 1
      : claimBudget === undefined
        ? (this.options.readyCapacity ?? 32)
        : Math.min(this.options.readyCapacity ?? 32, claimBudget);
    const readyBatch = this.node.createReadyBatch(readyCapacity);
    let claimsDrained = 0;
    try {
      for (;;) {
        if (claimBudget !== undefined && claimsDrained >= claimBudget) {
          // Lifecycle work has priority, but a bounded turn lets application
          // work make progress when lifecycle records keep arriving.
          return true;
        }
        readyBatch.reset();
        const drained = this.node.drainReady(domain, readyBatch, ZLINK_BACKEND_RECV_DONT_WAIT);
        if (!drained.ok || drained.records.length === 0) {
          return false;
        }
        if (domain === ReadyDomain.Application && drained.hasResidue) {
          this.markReady(ReadyDomain.Application);
        }
        for (let index = 0; index < drained.records.length; index += 1) {
          const claim = readyBatch.takeClaim(index);
          claimsDrained += 1;
          const owner = drained.records[index];
          // Raw ingress already attached one host permit to each record.
          // A claim that reserves admission here may receive only that permit's record.
          const receiveBatch = this.node.createReceiveBatch(
            owner.ordinaryIngressPreAdmitted === true ? MESH_DISPATCH_RECEIVE_CAPACITY : 1,
            this.options.partCapacity ?? 256
          );
          try {
            receiveBatch.reset();
            for (;;) {
              const claimPermit = owner.terminalCompletion === true
                || owner.ordinaryIngressPreAdmitted === true
                || domain === ReadyDomain.Infrastructure
                ? undefined
                : await this.acquirePermit();
              if (claimPermit === undefined
                  && this.disposed
                  && owner.terminalCompletion !== true
                  && owner.ordinaryIngressPreAdmitted !== true) return false;
              const received = claim.recvBatch(receiveBatch, ZLINK_BACKEND_RECV_DONT_WAIT);
              if (!received.ok) {
                claimPermit?.releaseAfterInternalProcessing();
                break;
              }
              if (received.records.length === 0) {
                claimPermit?.releaseAfterInternalProcessing();
              }
              // This owner keeps its claim while it drains. Other owners and
              // infrastructure may progress if one of its handlers suspends.
              if (domain === ReadyDomain.Infrastructure) {
                this.infrastructureScheduled = false;
                this.infrastructureDrainPromise = undefined;
                if ((this.pendingDomains & ReadyDomain.Infrastructure) !== 0) {
                  this.scheduleInfrastructure();
                }
              }
              try {
                for (const record of received.records) {
                  if (this.disposed) break;
                  const permit = record.applicationJobPermit ?? claimPermit;
                  if (owner.ordinaryIngressPreAdmitted === true && permit === undefined) {
                    throw new Error('Pre-admitted raw ingress record lost its Application Job Queue permit.');
                  }
                  this.recordsSinceYield += 1;
                  const dispatch = async () => {
                    try {
                      await this.options.dispatch(drained.records[index], record);
                    } catch (error) {
                      throw new ZLinkMeshDispatchFailure(
                        meshDispatchFailureContext(record),
                        error
                      );
                    }
                    await record.onTerminalCompletion?.();
                  };
                  if (permit === undefined) {
                    await dispatch();
                  } else {
                    if (
                      domain === ReadyDomain.Application
                      && record.applicationJobPermit === undefined
                    ) permit.markApplicationQueued();
                    await runWithApplicationJobPermit(permit, dispatch);
                  }
                  const yieldTurn = this.yieldIfNeeded();
                  if (yieldTurn !== undefined) await yieldTurn;
                }
              } finally {
                // Batch ownership includes records whose dispatch never began,
                // including on failure or shutdown in the middle of the batch.
                for (const record of received.records) {
                  for (const part of record.parts) part.close();
                  record.releaseRetainedIngress?.();
                }
              }
              receiveBatch.reset();
            }
          } finally {
            receiveBatch.close();
            claim.release();
          }
        }
        if (claimBudget !== undefined && claimsDrained >= claimBudget) {
          this.wakeApplicationWorker();
          return true;
        }
        // Empty ready claims also count as work, so they cannot monopolize
        // the microtask queue without giving I/O and deadlines a turn.
        this.recordsSinceYield += 1;
        const yieldTurn = this.yieldIfNeeded();
        if (yieldTurn !== undefined) await yieldTurn;
        if (!drained.hasResidue) {
          return false;
        }
      }
    } finally {
      readyBatch.close();
    }
  }

  private yieldIfNeeded(): Promise<void> | undefined {
    const nowMs = this.nowMs();
    if (this.recordsSinceYield < MESH_DISPATCH_YIELD_RECORDS
        && nowMs - this.yieldStartedAtMs < MESH_DISPATCH_YIELD_INTERVAL_MS) return undefined;
    this.recordsSinceYield = 0;
    return yieldToTimers().then(() => {
      this.yieldStartedAtMs = this.nowMs();
    });
  }

  private nowMs(): number {
    return this.options.monotonicNowMs?.() ?? performance.now();
  }

  private async acquirePermit(): Promise<ApplicationJobPermitPort | undefined> {
    try {
      return await this.options.applicationJobQueue.acquire(this.capacityStop.signal);
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
  return bytes.byteLength >= 5 && bytes[0] === 0x5a && bytes[1] === 0x4d && bytes[2] === 1
    ? bytes[3]
    : undefined;
}

function yieldToEventLoop(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}


function yieldToTimers(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 0));
}
