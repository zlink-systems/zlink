import { randomBytes } from 'node:crypto';
import type { ZLinkInstanceSpot, ZLinkInstanceSpotContext } from '../../contracts';
import { ZLinkPacket } from '../../contracts/Handlers/Attributes';
import type { ZLinkLocationOwnerToken } from '../locations/internal-location-contracts';
import {
  admissionEnqueueName,
  admissionNodeReportName,
  admissionAdmitPermitName,
  admissionReleasePermitName,
  admissionConnectionKey,
  admissionNodeKey,
  admissionSessionIdentity,
  readEnqueue,
  readReport,
  readAdmitPermit,
  readReleasePermit,
  type AdmissionConnection,
  type AdmissionDelivery,
  type AdmissionEnqueueReply,
  type AdmissionNodeIdentity,
  type AdmissionOwner,
  type AdmissionPosition,
  type AdmissionReport,
  type AdmissionReportReply,
  type AdmissionTicket,
  type AdmitPermitReply,
  type AdmitPermitRequest,
  type ReleasePermitMessage
} from './admission-queue-messages';
import { AdmissionTickets } from './admission-tickets';

const ticketLifetimeMs = 30_000;
const reportValidityMs = 3_000;
const positionPushIntervalMs = 5_000;
const maintenancePeriodMs = 1_000;

interface Waiter {
  readonly connection: AdmissionConnection;
  ticket?: AdmissionTicket;
  expiresAt?: number;
  ticketDelivery?: 'sent' | 'used';
  lastPosition?: AdmissionPosition;
  lastPositionAt?: number;
}

interface ReportState {
  readonly receivedAt: number;
  readonly available: number;
  issued: number;
}

interface AdmissionQueueSnapshot {
  readonly waiters: readonly Waiter[];
  readonly permits: ReadonlyMap<string, AdmitPermitRequest>;
}

export class AdmissionQueueSpot {
  readonly epoch: string;
  private readonly waiting: Map<string, Waiter>;
  private readonly reports = new Map<string, ReportState>();
  private readonly permits: Map<string, AdmitPermitRequest>;
  private issuedAt: number[] = [];

  constructor(
    private readonly tickets: AdmissionTickets,
    private readonly globalLimit?: number,
    epoch = AdmissionTickets.newEpoch(),
    private readonly now = () => performance.now(),
    private readonly utcNow = Date.now,
    initial?: AdmissionQueueSnapshot
  ) {
    this.epoch = String(epoch);
    this.waiting = new Map(
      initial?.waiters.map((waiter) => [admissionConnectionKey(waiter.connection), { ...waiter }])
    );
    this.permits = new Map(initial?.permits);
  }

  get runningPermits(): number {
    return this.permits.size;
  }

  get permitLimitReached(): boolean {
    return this.permits.size >= (this.globalLimit ?? 0);
  }

  waiters(): readonly Readonly<Waiter>[] {
    return [...this.waiting.values()].filter((waiter) => waiter.ticketDelivery !== 'used');
  }

  enqueue(connection: AdmissionConnection): AdmissionEnqueueReply {
    const now = this.now();
    const utcNow = this.utcNow();
    this.expire(now, utcNow);
    const key = admissionConnectionKey(connection);
    let waiter = this.waiting.get(key);
    if (waiter === undefined) {
      waiter = { connection };
      this.waiting.set(key, waiter);
      this.issue(now, utcNow);
    }
    const session = admissionSessionIdentity(connection);
    if (waiter.ticket !== undefined) {
      waiter.ticketDelivery ??= 'sent';
      return { epoch: this.epoch, delivery: { ...session, kind: 'ticket', ...waiter.ticket } };
    }
    const position = this.positionOf(waiter);
    waiter.lastPosition = position;
    waiter.lastPositionAt = now;
    return { epoch: this.epoch, delivery: { ...session, kind: 'position', ...position } };
  }

  async report(
    report: AdmissionReport,
    reclaimEndedLeases: () => Promise<void>
  ): Promise<AdmissionReportReply> {
    let now = this.now();
    const node = admissionNodeKey(report);
    let utcNow = this.utcNow();
    this.expire(now, utcNow);
    this.reconcilePermits(report, node);
    this.reports.set(node, {
      receivedAt: now,
      available: Math.max(0, report.limit - report.inFlight),
      issued: 0
    });
    const { nodeRid, nodeLifecycleGeneration } = report;
    for (const session of report.usedTickets) {
      const key = admissionConnectionKey({ nodeRid, nodeLifecycleGeneration, ...session });
      const waiter = this.waiting.get(key);
      if (waiter?.ticket === undefined) {
        this.waiting.delete(key);
      } else {
        waiter.ticketDelivery = 'used';
      }
    }
    for (const session of report.endedSessions) {
      this.waiting.delete(admissionConnectionKey({ nodeRid, nodeLifecycleGeneration, ...session }));
    }
    if (this.issue(now, utcNow, this.permits.size > 0)) {
      await reclaimEndedLeases();
      now = this.now();
      utcNow = this.utcNow();
      this.expire(now, utcNow);
      this.issue(now, utcNow);
    }
    return { epoch: this.epoch, deliveries: this.takeDeliveries(node, now) };
  }

  admit(request: AdmitPermitRequest): AdmitPermitReply {
    const key = admissionConnectionKey(request);
    const waiter = this.waiting.get(key);
    if (waiter?.ticket === undefined || ticketExpired(waiter, this.utcNow())) {
      return { granted: false };
    }
    if (request.epoch !== this.epoch || this.permitLimitReached) {
      this.requeue(key, waiter.connection);
      return { granted: false };
    }
    // Release carries the node incarnation and permit ID, with no Spot epoch.
    let permitId: string;
    do {
      permitId = String(randomBytes(8).readBigUInt64BE());
    } while (permitId === '0' || this.permits.has(permitId));
    this.permits.set(permitId, request);
    return { granted: true, permitId };
  }

  release(release: ReleasePermitMessage): void {
    const permit = this.permits.get(release.permitId);
    if (permit !== undefined && admissionNodeKey(release) === admissionNodeKey(permit)) {
      this.permits.delete(release.permitId);
    }
  }

  owners(): readonly AdmissionOwner[] {
    const owners = new Map<string, AdmissionOwner>();
    for (const { ownerId, leaseGeneration } of this.permits.values()) {
      owners.set(JSON.stringify([ownerId, leaseGeneration]), { ownerId, leaseGeneration });
    }
    return [...owners.values()];
  }

  ownerLeaseEnded(owner: AdmissionOwner): void {
    for (const [id, permit] of this.permits) {
      if (sameOwner(permit, owner)) this.permits.delete(id);
    }
  }

  advance(): void {
    this.expire(this.now(), this.utcNow());
  }

  budget(node: AdmissionNodeIdentity, now = this.now()): number {
    return Math.min(
      this.remainingNodeBudget(admissionNodeKey(node), now),
      this.remainingGlobalBudget(this.utcNow())
    );
  }

  private remainingNodeBudget(node: string, now: number): number {
    const report = this.reports.get(node);
    return report === undefined || now - report.receivedAt > reportValidityMs
      ? 0
      : Math.max(0, report.available - report.issued);
  }

  private remainingGlobalBudget(utcNow: number): number {
    if (this.globalLimit === undefined) return Infinity;
    let remaining = Math.max(0, this.globalLimit - this.permits.size);
    if (remaining === 0) return 0;
    for (const waiter of this.waiting.values()) {
      if (
        waiter.ticket === undefined ||
        waiter.ticketDelivery === 'used' ||
        ticketExpired(waiter, utcNow)
      )
        continue;
      remaining--;
      if (remaining === 0) break;
    }
    return remaining;
  }

  private requeue(key: string, connection: AdmissionConnection): void {
    const node = admissionNodeKey(connection);
    const entries = [...this.waiting];
    const first = entries.findIndex(
      ([candidate, waiter]) =>
        candidate === key ||
        (waiter.ticket === undefined && admissionNodeKey(waiter.connection) === node)
    );
    if (entries[first]![0] === key) {
      this.waiting.set(key, { connection });
      return;
    }
    const tail = entries.slice(first);
    for (const [candidate] of tail) this.waiting.delete(candidate);
    this.waiting.set(key, { connection });
    for (const [candidate, waiter] of tail) {
      if (candidate !== key) this.waiting.set(candidate, waiter);
    }
  }

  private reconcilePermits(report: AdmissionReport, node: string): void {
    if (this.permits.size === 0) return;
    const held = new Set(report.heldPermits);
    const resolved = BigInt(report.resolvedRequestSeq);
    for (const [id, permit] of this.permits) {
      if (
        admissionNodeKey(permit) === node &&
        BigInt(permit.requestSeq) <= resolved &&
        !held.has(id)
      ) {
        this.permits.delete(id);
      }
    }
  }

  private expire(now: number, utcNow: number): void {
    for (const [key, waiter] of this.waiting) {
      if (ticketExpired(waiter, utcNow)) this.waiting.delete(key);
    }
    for (const [key, report] of this.reports) {
      if (now - report.receivedAt > reportValidityMs) this.reports.delete(key);
    }
    const cutoff = now - ticketLifetimeMs;
    const firstRecent = this.issuedAt.findIndex((issuedAt) => issuedAt > cutoff);
    if (firstRecent !== 0)
      this.issuedAt = firstRecent === -1 ? [] : this.issuedAt.slice(firstRecent);
  }

  private issue(now: number, utcNow: number, detectBlockedWaiter = false): boolean {
    let globalBudget = this.remainingGlobalBudget(utcNow);
    for (const waiter of this.waiting.values()) {
      if (globalBudget === 0 && !detectBlockedWaiter) break;
      if (waiter.ticket !== undefined) continue;
      const node = admissionNodeKey(waiter.connection);
      if (this.remainingNodeBudget(node, now) === 0) continue;
      if (globalBudget === 0) return true;
      const expiresAt = utcNow + ticketLifetimeMs;
      waiter.ticket = {
        ticket: this.tickets.issue(waiter.connection, BigInt(this.epoch), expiresAt),
        expiresInMs: ticketLifetimeMs
      };
      waiter.expiresAt = expiresAt;
      this.reports.get(node)!.issued++;
      this.issuedAt.push(now);
      globalBudget--;
    }
    return false;
  }

  private positionOf(target: Waiter): AdmissionPosition {
    let position = 0;
    for (const waiter of this.waiting.values()) {
      if (waiter.ticket === undefined) position++;
      if (waiter === target) break;
    }
    return this.position(position);
  }

  private position(position: number): AdmissionPosition {
    return { position, estimatedWaitMs: estimateWaitMs(position, this.issuedAt.length) };
  }

  private takeDeliveries(node: string, now: number): AdmissionDelivery[] {
    const deliveries: AdmissionDelivery[] = [];
    let position = 0;
    for (const waiter of this.waiting.values()) {
      if (waiter.ticket === undefined) position++;
      if (admissionNodeKey(waiter.connection) !== node) continue;
      if (waiter.ticket !== undefined) {
        if (waiter.ticketDelivery === undefined) {
          waiter.ticketDelivery = 'sent';
          deliveries.push({
            ...admissionSessionIdentity(waiter.connection),
            kind: 'ticket',
            ...waiter.ticket
          });
        }
        continue;
      }
      const update = this.position(position);
      if (
        waiter.lastPosition?.position !== update.position ||
        waiter.lastPosition.estimatedWaitMs !== update.estimatedWaitMs ||
        now - (waiter.lastPositionAt ?? 0) >= positionPushIntervalMs
      ) {
        waiter.lastPosition = update;
        waiter.lastPositionAt = now;
        deliveries.push({
          ...admissionSessionIdentity(waiter.connection),
          kind: 'position',
          ...update
        });
      }
    }
    return deliveries;
  }
}

function ticketExpired(waiter: Waiter, utcNow: number): boolean {
  return waiter.expiresAt !== undefined && waiter.expiresAt <= utcNow;
}

function sameOwner(left: AdmissionOwner, right: AdmissionOwner): boolean {
  return left.ownerId === right.ownerId && left.leaseGeneration === right.leaseGeneration;
}

export function estimateWaitMs(position: number, ticketsLast30Seconds: number): number {
  const rate = ticketsLast30Seconds / 30;
  return Math.min(2_147_483_647, Math.ceil((position * 1_000) / (rate === 0 ? 1 : rate)));
}

interface AdmissionQueueInstance extends ZLinkInstanceSpot {
  readonly queue: AdmissionQueueSpot;
  readonly isOwnerLive: (owner: ZLinkLocationOwnerToken, signal?: AbortSignal) => Promise<boolean>;
}

export function admissionQueueInstanceFactory(
  tickets: AdmissionTickets,
  globalLimit: number | undefined,
  isOwnerLive: AdmissionQueueInstance['isOwnerLive']
): new (context: ZLinkInstanceSpotContext) => ZLinkInstanceSpot {
  return class AdmissionQueueInstanceSpot implements AdmissionQueueInstance {
    readonly queue = new AdmissionQueueSpot(tickets, globalLimit);
    readonly isOwnerLive = isOwnerLive;

    constructor(readonly context: ZLinkInstanceSpotContext) {}

    configure(): void {
      this.context.handlers
        .addPacket(EnqueueHandler)
        .addPacket(NodeReportHandler)
        .addPacket(AdmitPermitHandler)
        .addPacket(ReleasePermitHandler)
        .addTimerHandler(QueueMaintenance);
    }

    async onInitialize(signal?: AbortSignal): Promise<void> {
      await this.context.addTimer(
        'admission-queue',
        maintenancePeriodMs,
        QueueMaintenance,
        undefined,
        signal
      );
    }
  };
}

@ZLinkPacket(admissionEnqueueName)
class EnqueueHandler {
  async handle(spot: AdmissionQueueInstance, request: unknown): Promise<AdmissionEnqueueReply> {
    return spot.queue.enqueue(readEnqueue(request).connection);
  }
}

@ZLinkPacket(admissionNodeReportName)
class NodeReportHandler {
  async handle(spot: AdmissionQueueInstance, request: unknown): Promise<AdmissionReportReply> {
    return spot.queue.report(readReport(request), () => reclaimEndedLeases(spot));
  }
}

class QueueMaintenance {
  async handle(spot: AdmissionQueueInstance): Promise<void> {
    spot.queue.advance();
  }
}

@ZLinkPacket(admissionAdmitPermitName)
class AdmitPermitHandler {
  async handle(spot: AdmissionQueueInstance, request: unknown): Promise<AdmitPermitReply> {
    const permit = readAdmitPermit(request);
    if (spot.queue.permitLimitReached) await reclaimEndedLeases(spot);
    return spot.queue.admit(permit);
  }
}

async function reclaimEndedLeases(spot: AdmissionQueueInstance): Promise<void> {
  const owners = spot.queue.owners();
  if (owners.length === 0) return;
  const ended = await spot.context
    .runIoWorker(async (signal) => {
      const live = await Promise.all(
        owners.map(({ ownerId, leaseGeneration }) =>
          spot.isOwnerLive({ ownerId, leaseGeneration: BigInt(leaseGeneration) }, signal)
        )
      );
      return owners.filter((_, index) => live[index] === false);
    })
    .yield();
  for (const owner of ended) spot.queue.ownerLeaseEnded(owner);
}

@ZLinkPacket(admissionReleasePermitName)
class ReleasePermitHandler {
  async handle(spot: AdmissionQueueInstance, request: unknown): Promise<void> {
    spot.queue.release(readReleasePermit(request));
  }
}
