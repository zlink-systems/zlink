import {
  admissionConnectionKey,
  admissionSessionIdentity,
  readAdmitPermitReply,
  readEnqueueReply,
  readReportReply,
  type AdmissionConnection,
  type AdmissionEnqueueReply,
  type AdmissionSessionIdentity,
  type AdmissionNodeIdentity,
  type AdmissionOwner,
  type AdmissionReport,
  type AdmissionReportReply,
  type AdmitPermitRequest,
  type ReleasePermitMessage
} from './admission-queue-messages';
import type { AdmissionTickets } from './admission-tickets';

interface AdmissionQueueNodePorts {
  readonly local: () => AdmissionNodeIdentity | undefined;
  readonly owner: () => AdmissionOwner | undefined;
  readonly limit: () => number;
  readonly inFlight: () => number;
  readonly report: (report: AdmissionReport) => Promise<unknown>;
  readonly acquire: (request: AdmitPermitRequest) => Promise<unknown>;
  readonly release: (release: ReleasePermitMessage) => void;
  readonly error: (error: unknown) => void;
  readonly now?: () => number;
}

export type AdmissionResult =
  | { readonly kind: 'admitted'; readonly release: () => void }
  | { readonly kind: 'queueRequired' }
  | { readonly kind: 'unavailable' };

export class AdmissionQueueNode {
  private epoch?: string;
  private requestSeq = 0n;
  private readonly pending = new Set<bigint>();
  private readonly held = new Map<string, string>();
  private readonly used = new Map<string, AdmissionSessionIdentity>();
  private readonly ended = new Map<string, AdmissionSessionIdentity>();
  private reporting = false;

  constructor(
    private readonly tickets: AdmissionTickets,
    readonly globalLimitEnabled: boolean,
    private readonly ports: AdmissionQueueNodePorts
  ) {}

  connection(streamName: string, sessionRid: string): AdmissionConnection | undefined {
    const local = this.ports.local();
    return local === undefined ? undefined : { ...local, streamName, sessionRid };
  }

  verify(connection: AdmissionConnection, ticket: string | undefined): string | undefined {
    const epoch = this.epoch;
    if (epoch === undefined) return undefined;
    const now = this.ports.now?.() ?? Date.now();
    return this.tickets.verify(ticket, connection, BigInt(epoch), now) ? epoch : undefined;
  }

  async admit(connection: AdmissionConnection, epoch: string): Promise<AdmissionResult> {
    if (!this.globalLimitEnabled) return { kind: 'admitted', release: () => {} };
    const owner = this.ports.owner();
    if (owner === undefined) return { kind: 'unavailable' };
    const requestSeq = ++this.requestSeq;
    this.pending.add(requestSeq);
    try {
      const reply = readAdmitPermitReply(
        await this.ports.acquire({
          ...connection,
          ...owner,
          epoch,
          requestSeq: String(requestSeq)
        })
      );
      if (!reply.granted) return { kind: 'queueRequired' };
      const { permitId } = reply;
      if (this.epoch !== epoch) return { kind: 'queueRequired' };
      this.held.set(permitId, epoch);
      return {
        kind: 'admitted',
        release: () => {
          if (this.held.get(permitId) !== epoch) return;
          this.held.delete(permitId);
          this.ports.release({
            nodeRid: connection.nodeRid,
            nodeLifecycleGeneration: connection.nodeLifecycleGeneration,
            permitId
          });
        }
      };
    } catch (error) {
      this.ports.error(error);
      return { kind: 'unavailable' };
    } finally {
      this.pending.delete(requestSeq);
    }
  }

  ticketUsed(connection: AdmissionConnection): void {
    this.used.set(admissionConnectionKey(connection), admissionSessionIdentity(connection));
  }

  sessionEnded(connection: AdmissionConnection): void {
    this.ended.set(admissionConnectionKey(connection), admissionSessionIdentity(connection));
  }

  snapshot(): Pick<AdmissionReport, 'resolvedRequestSeq' | 'heldPermits'> {
    const firstPending = this.pending.keys().next().value;
    return {
      resolvedRequestSeq: String(firstPending === undefined ? this.requestSeq : firstPending - 1n),
      heldPermits: [...this.held.keys()]
    };
  }

  async enqueue(submit: () => Promise<unknown>): Promise<AdmissionEnqueueReply> {
    try {
      return readEnqueueReply(await submit());
    } catch (error) {
      this.epoch = undefined;
      throw error;
    }
  }

  async report(): Promise<AdmissionReportReply | undefined> {
    if (this.reporting) return undefined;
    const local = this.ports.local();
    if (local === undefined) {
      this.epoch = undefined;
      return undefined;
    }
    const used = [...this.used];
    const ended = [...this.ended];
    this.reporting = true;
    try {
      const reply = readReportReply(
        await this.ports.report({
          nodeRid: local.nodeRid,
          nodeLifecycleGeneration: local.nodeLifecycleGeneration,
          limit: this.ports.limit(),
          inFlight: this.ports.inFlight(),
          usedTickets: used.map(([, session]) => session),
          endedSessions: ended.map(([, session]) => session),
          ...this.snapshot()
        })
      );
      acknowledgeSessions(this.used, used);
      acknowledgeSessions(this.ended, ended);
      if (reply.epoch !== this.epoch) {
        for (const [id, epoch] of this.held) {
          if (epoch !== reply.epoch) this.held.delete(id);
        }
        this.epoch = reply.epoch;
      }
      return reply;
    } catch (error) {
      this.epoch = undefined;
      throw error;
    } finally {
      this.reporting = false;
    }
  }
}

function acknowledgeSessions(
  pending: Map<string, AdmissionSessionIdentity>,
  reported: ReadonlyArray<readonly [string, AdmissionSessionIdentity]>
): void {
  for (const [key, session] of reported) {
    if (pending.get(key) === session) pending.delete(key);
  }
}
