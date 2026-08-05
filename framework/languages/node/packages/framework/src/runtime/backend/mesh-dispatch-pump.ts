import { ZLINK_BACKEND_RECV_DONT_WAIT } from './runtime-values';
import {
  operationRequiresReply,
  ReadyDomain,
  type ReadyRecord,
  type ReceiveRecord
} from '../foundation/service-runtime-contracts';
import type { ZLinkBackendMeshNode } from './contracts';
import type { ZLinkInboundDispatchBudget } from '../dispatch/inbound-dispatch-budget';

const MESH_DISPATCH_TIMER_YIELD_BATCHES = 16;
const MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET = 4;

//  The pump awaits between the two reads and the budget can pause in that gap.
//  Reading through a function keeps the later check honest; an inline read
//  stays narrowed by the first one.
function budgetPaused(budget?: { readonly receivePaused: boolean }): boolean {
  return budget?.receivePaused === true;
}


export interface ZLinkMeshDispatchPumpOptions {
  readonly readyCapacity?: number;
  readonly messageCapacity?: number;
  readonly partCapacity?: number;
  readonly byteCapacity?: number;
  readonly dispatch: (owner: ReadyRecord, record: ReceiveRecord) => void | Promise<void>;
  readonly inboundDispatchBudget?: ZLinkInboundDispatchBudget;
  readonly reportError?: (error: unknown) => void;
}

export class ZLinkMeshDispatchPump {
  private pendingDomains: number = ReadyDomain.None;
  private scheduled = false;
  private disposed = false;
  private drainPromise?: Promise<void>;
  private readonly activeDrains = new Set<Promise<void>>();
  private readonly removeResumeListener?: () => void;

  constructor(
    private readonly node: ZLinkBackendMeshNode,
    private readonly options: ZLinkMeshDispatchPumpOptions
  ) {
    this.removeResumeListener = options.inboundDispatchBudget?.onResume(() => {
      if (this.disposed) return;
      this.pendingDomains |= ReadyDomain.Application;
      this.schedule();
    });
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
    this.pendingDomains = ReadyDomain.None;
    this.removeResumeListener?.();
    while (this.activeDrains.size > 0) {
      await Promise.all([...this.activeDrains]);
    }
  }

  private schedule(): void {
    if (this.scheduled) {
      return;
    }
    this.scheduled = true;
    const drain = yieldToEventLoop()
      .then(() => this.drain())
      .catch((error) => this.options.reportError?.(error));
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

  private async drain(): Promise<void> {
    while (!this.disposed) {
      const domains = this.pendingDomains;
      this.pendingDomains = ReadyDomain.None;
      if (domains === ReadyDomain.None) {
        return;
      }
      await this.drainDomains(domains);
    }
  }

  private async drainDomains(domains: number): Promise<void> {
    let lifecycleBudgetExhausted = false;
    if ((domains & ReadyDomain.Infrastructure) !== 0) {
      lifecycleBudgetExhausted = await this.drainDomain(
        ReadyDomain.Infrastructure,
        MESH_DISPATCH_LIFECYCLE_CLAIM_BUDGET
      );
    }
    if ((domains & ReadyDomain.Application) !== 0) {
      await this.drainDomain(ReadyDomain.Application);
    }
    if (lifecycleBudgetExhausted) {
      this.pendingDomains |= ReadyDomain.Infrastructure;
    }
  }

  private async drainDomain(domain: number, claimBudget?: number): Promise<boolean> {
    const applicationBudget = domain === ReadyDomain.Application
      ? this.options.inboundDispatchBudget
      : undefined;
    if (budgetPaused(applicationBudget)) return false;
    const readyCapacity = claimBudget === undefined
      ? (this.options.readyCapacity ?? 32)
      : Math.min(this.options.readyCapacity ?? 32, claimBudget);
    const readyBatch = this.node.createReadyBatch(readyCapacity);
    const receiveBatch = this.node.createReceiveBatch(
      applicationBudget === undefined ? (this.options.messageCapacity ?? 64) : 1,
      this.options.partCapacity ?? 256,
      this.options.byteCapacity ?? (1 << 20)
    );
    let receiveBatchesSinceTimerYield = 0;
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
        for (let index = 0; index < drained.records.length; index += 1) {
          const claim = readyBatch.takeClaim(index);
          claimsDrained += 1;
          try {
            receiveBatch.reset();
            for (;;) {
              if (budgetPaused(applicationBudget)) return false;
              const received = claim.recvBatch(receiveBatch, ZLINK_BACKEND_RECV_DONT_WAIT);
              if (!received.ok) {
                break;
              }
              for (const record of received.records) {
                let payloadByteCount = 0;
                for (const part of record.parts) {
                  payloadByteCount += part.size();
                }
                const payloadBytes = BigInt(payloadByteCount);
                applicationBudget?.enqueue(payloadBytes);
                let started = false;
                let releaseCompletion: (() => void) | undefined;
                try {
                  releaseCompletion = operationRequiresReply(record.operationKind)
                    ? await applicationBudget?.acquireCompletionSend()
                    : undefined;
                  // A handler may synchronously submit an operation whose
                  // control/completion is owned by this same MeshNode.
                  this.scheduled = false;
                  this.drainPromise = undefined;
                  if (this.pendingDomains !== ReadyDomain.None) {
                    this.schedule();
                  }
                  applicationBudget?.start(payloadBytes);
                  started = true;
                  await this.options.dispatch(drained.records[index], record);
                  await record.onTerminalCompletion?.();
                } finally {
                  releaseCompletion?.();
                  if (started) {
                    applicationBudget?.complete(payloadBytes);
                  } else {
                    applicationBudget?.cancelQueued(payloadBytes);
                  }
                  for (const part of record.parts) {
                    part.close();
                  }
                }
              }
              // A continuously readable owner must not keep the timers phase
              // from running, but a timer turn for every single record adds a
              // Promise, closure, and timer allocation to the hot path.
              receiveBatchesSinceTimerYield += 1;
              if (receiveBatchesSinceTimerYield >= MESH_DISPATCH_TIMER_YIELD_BATCHES) {
                receiveBatchesSinceTimerYield = 0;
                await yieldToTimers();
              }
              receiveBatch.reset();
            }
          } finally {
            claim.release();
          }
        }
        await yieldToTimers();
        receiveBatchesSinceTimerYield = 0;
        if (!drained.hasResidue) {
          return false;
        }
      }
    } finally {
      receiveBatch.close();
      readyBatch.close();
    }
  }
}

function yieldToEventLoop(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function yieldToTimers(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 0));
}
