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

const MESH_DISPATCH_TIMER_YIELD_BATCHES = 16;
const MESH_DISPATCH_TIMER_YIELD_INTERVAL_MS = 2;
const MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET = 4;


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
  private scheduled = false;
  private infrastructureScheduled = false;
  private disposed = false;
  private drainPromise?: Promise<void>;
  private infrastructureDrainPromise?: Promise<void>;
  private readonly activeDrains = new Set<Promise<void>>();
  private readonly capacityStop = new AbortController();

  constructor(
    private readonly node: ZLinkBackendMeshNode,
    private readonly options: ZLinkMeshDispatchPumpOptions
  ) {
  }

  start(): void {
    this.node.setReadyHandler((domains) => {
      if (this.disposed) {
        return ReadyDomain.None;
      }
      this.pendingDomains |= domains;
      this.schedule();
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
    while (this.activeDrains.size > 0) {
      await Promise.all([...this.activeDrains]);
    }
  }

  private schedule(): void {
    if ((this.pendingDomains & ReadyDomain.Infrastructure) !== 0) {
      this.scheduleInfrastructure();
    }
    if ((this.pendingDomains & ReadyDomain.Application) === 0) {
      return;
    }
    if (this.scheduled) {
      return;
    }
    this.scheduled = true;
    const drain = yieldToEventLoop()
      .then(() => this.drain())
      .catch((error) => {
        if (error instanceof ZLinkMeshDispatchFailure) {
          this.options.reportError?.(error.dispatchCause, error.context);
          return;
        }
        this.options.reportError?.(error);
      });
    this.activeDrains.add(drain);
    this.drainPromise = drain;
    void drain.finally(() => {
      this.activeDrains.delete(drain);
      if (this.drainPromise === drain) {
        this.drainPromise = undefined;
        this.scheduled = false;
        if (!this.disposed && this.pendingDomains !== ReadyDomain.None) {
          this.schedule();
        }
      }
    });
  }

  /**
   * Infrastructure controls must not wait for an application permit. During
   * source draining an application claim can legitimately remain parked at
   * `acquirePermit`; sharing that turn would strand Ready/State/Cutover in
   * the infrastructure mailbox and prevent the relocation coordinator from
   * completing the drain that releases the application work.
   */
  private scheduleInfrastructure(): void {
    if (this.infrastructureScheduled) {
      return;
    }
    this.infrastructureScheduled = true;
    const drain = yieldToEventLoop()
      .then(() => this.drainInfrastructure())
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

  private async drain(): Promise<void> {
    while (!this.disposed) {
      if ((this.pendingDomains & ReadyDomain.Application) === 0) {
        return;
      }
      this.pendingDomains &= ~ReadyDomain.Application;
      await this.drainDomain(ReadyDomain.Application);
    }
  }

  private async drainInfrastructure(): Promise<void> {
    while (!this.disposed) {
      if ((this.pendingDomains & ReadyDomain.Infrastructure) === 0) {
        return;
      }
      this.pendingDomains &= ~ReadyDomain.Infrastructure;
      const lifecycleBudgetExhausted = await this.drainDomain(
        ReadyDomain.Infrastructure,
        MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET
      );
      if (lifecycleBudgetExhausted) {
        this.pendingDomains |= ReadyDomain.Infrastructure;
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
    // One record per claim receive keeps one ordinary-ingress permit paired
    // with exactly one dispatch turn. Terminal completion claims bypass it.
    const receiveBatch = this.node.createReceiveBatch(1, this.options.partCapacity ?? 256);
    let receiveBatchesSinceTimerYield = 0;
    let timerYieldStartedAtMs = this.nowMs();
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
          this.pendingDomains |= ReadyDomain.Application;
        }
        for (let index = 0; index < drained.records.length; index += 1) {
          const claim = readyBatch.takeClaim(index);
          claimsDrained += 1;
          try {
            receiveBatch.reset();
            for (;;) {
              const owner = drained.records[index];
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
              for (const record of received.records) {
                const permit = record.applicationJobPermit ?? claimPermit;
                if (owner.ordinaryIngressPreAdmitted === true && permit === undefined) {
                  record.releaseRetainedIngress?.();
                  throw new Error('Pre-admitted raw ingress record lost its Application Job Queue permit.');
                }
                try {
                  // A handler may synchronously submit an operation whose
                  // control/completion is owned by this same MeshNode.
                  this.scheduled = false;
                  this.drainPromise = undefined;
                  if (this.pendingDomains !== ReadyDomain.None) {
                    this.schedule();
                  }
                  const dispatch = () => runZLinkExecutionArea(
                    domain === ReadyDomain.Infrastructure ? 'infrastructure' : 'application',
                    async () => {
                      try {
                        await this.options.dispatch(drained.records[index], record);
                      } catch (error) {
                        throw new ZLinkMeshDispatchFailure(
                          meshDispatchFailureContext(record),
                          error
                        );
                      }
                      await record.onTerminalCompletion?.();
                    }
                  );
                  if (permit === undefined) {
                    await dispatch();
                  } else {
                    if (
                      domain === ReadyDomain.Application
                      && record.applicationJobPermit === undefined
                    ) permit.markApplicationQueued();
                    await runWithApplicationJobPermit(permit, dispatch);
                  }
                } finally {
                  for (const part of record.parts) {
                    part.close();
                  }
                  record.releaseRetainedIngress?.();
                }
              }
              // A continuously readable owner must not keep the timers phase
              // from running, but a timer turn for every single record adds a
              // Promise, closure, and timer allocation to the hot path.
              receiveBatchesSinceTimerYield += 1;
              if (
                receiveBatchesSinceTimerYield >= MESH_DISPATCH_TIMER_YIELD_BATCHES
                || this.nowMs() - timerYieldStartedAtMs >= MESH_DISPATCH_TIMER_YIELD_INTERVAL_MS
              ) {
                receiveBatchesSinceTimerYield = 0;
                await yieldToTimers();
                timerYieldStartedAtMs = this.nowMs();
              }
              receiveBatch.reset();
            }
          } finally {
            claim.release();
          }
        }
        await yieldToTimers();
        receiveBatchesSinceTimerYield = 0;
        timerYieldStartedAtMs = this.nowMs();
        if (!drained.hasResidue) {
          return false;
        }
      }
    } finally {
      receiveBatch.close();
      readyBatch.close();
    }
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
