export type ServiceMailboxDomain = 'application' | 'infrastructure';

export interface ServiceMailboxRecord {
  readonly owner: string;
  readonly domain: ServiceMailboxDomain;
  readonly parts: readonly Uint8Array[];
  readonly sourceRoutingId?: string;
  readonly sourceRoute?: Uint8Array;
  readonly requestSequence?: bigint;
  readonly reply?: (parts: readonly Uint8Array[]) => void;
  readonly correlation?: bigint;
  readonly localReply?: (
    terminalResult: number,
    failureCode: number,
    payload?: {
      readonly packetName: string;
      readonly contentType: string;
      readonly payload: Uint8Array;
    }
  ) => void;
  readonly stateful?: unknown;
}

export interface ServiceMailboxClaim {
  readonly owner: string;
  readonly domain: ServiceMailboxDomain;
  readonly serial: bigint;
  readonly records: readonly ServiceMailboxRecord[];
}

export interface ServiceMailboxRelocationSeal {
  readonly owner: string;
  readonly serial: bigint;
  readonly captured: readonly ServiceMailboxRecord[];
}

interface OwnerQueue {
  readonly records: MailboxQueue<ServiceMailboxRecord>;
  bytes: number;
  claimed: boolean;
  claimSerial: bigint;
}

interface DomainState {
  readonly owners: Map<string, OwnerQueue>;
  readonly ready: MailboxQueue<string>;
  readonly indexed: Set<string>;
  messages: number;
  bytes: number;
  readonly messageBudget: number;
  readonly byteBudget: number;
}

export interface ServiceMailboxLimits {
  readonly applicationMessages: number;
  readonly applicationBytes: number;
  readonly infrastructureMessages: number;
  readonly infrastructureBytes: number;
}

const RELOCATION_HOLD_MESSAGE_LIMIT = 1024;
const RELOCATION_HOLD_BYTE_LIMIT = 16 * 1024 * 1024;

/** Bounded level-triggered queues with one active application claim per owner. */
export class ServiceMailbox {
  private readonly application: DomainState;
  private readonly infrastructure: DomainState;
  private readonly relocationSeals = new Map<string, {
    readonly serial: bigint;
    readonly captured: ServiceMailboxRecord[];
    readonly held: ServiceMailboxRecord[];
    heldBytes: number;
  }>();
  private readonly relocatedOwners = new Set<string>();
  private nextClaimSerial = 1n;
  private nextRelocationSerial = 1n;
  private closed = false;

  constructor(limits: ServiceMailboxLimits) {
    this.application = createDomain(limits.applicationMessages, limits.applicationBytes);
    this.infrastructure = createDomain(limits.infrastructureMessages, limits.infrastructureBytes);
  }

  tryEnqueue(record: ServiceMailboxRecord): boolean {
    if (record.owner.length === 0 || record.parts.length === 0) {
      throw new TypeError('Mailbox records require an owner and retained payload.');
    }
    const target = this.domain(record.domain);
    const bytes = retainedBytes(record);
    if (
      this.closed
      || target.messages >= target.messageBudget
      || bytes > target.byteBudget - target.bytes
    ) {
      return false;
    }
    if (record.domain === 'application') {
      if (this.relocatedOwners.has(record.owner)) return false;
      const relocation = this.relocationSeals.get(record.owner);
      if (relocation !== undefined) {
        if (relocation.held.length >= RELOCATION_HOLD_MESSAGE_LIMIT
          || bytes > RELOCATION_HOLD_BYTE_LIMIT - relocation.heldBytes) {
          return false;
        }
        relocation.held.push(retainRecord(record));
        relocation.heldBytes += bytes;
        target.messages++;
        target.bytes += bytes;
        return true;
      }
    }
    let queue = target.owners.get(record.owner);
    if (queue === undefined) {
      queue = { records: new MailboxQueue(), bytes: 0, claimed: false, claimSerial: 0n };
      target.owners.set(record.owner, queue);
    }
    const retained = retainRecord(record);
    queue.records.push(retained);
    queue.bytes += bytes;
    target.messages++;
    target.bytes += bytes;
    if (!queue.claimed && !target.indexed.has(record.owner)) {
      target.indexed.add(record.owner);
      target.ready.push(record.owner);
    }
    return true;
  }

  tryClaim(
    domain: ServiceMailboxDomain,
    messageBudget: number,
    byteBudget: number
  ): ServiceMailboxClaim | undefined {
    requirePositive(messageBudget, 'messageBudget');
    requirePositive(byteBudget, 'byteBudget');
    const source = this.domain(domain);
    for (;;) {
      const owner = source.ready.shift();
      if (owner === undefined) return undefined;
      source.indexed.delete(owner);
      const queue = source.owners.get(owner);
      if (queue === undefined || queue.claimed || queue.records.length === 0) continue;

      queue.claimed = true;
      const serial = this.nextClaimSerial++;
      queue.claimSerial = serial;
      const records: ServiceMailboxRecord[] = [];
      let bytes = 0;
      while (queue.records.length > 0 && records.length < messageBudget) {
        const next = queue.records.peek()!;
        const nextBytes = retainedBytes(next);
        if (records.length > 0 && nextBytes > byteBudget - bytes) break;
        queue.records.shift();
        queue.bytes -= nextBytes;
        source.messages--;
        source.bytes -= nextBytes;
        bytes += nextBytes;
        records.push(next);
      }
      return { owner, domain, serial, records };
    }
  }

  release(
    claim: ServiceMailboxClaim,
    remaining: readonly ServiceMailboxRecord[] = []
  ): boolean {
    const target = this.domain(claim.domain);
    const queue = target.owners.get(claim.owner);
    if (queue === undefined || !queue.claimed || queue.claimSerial !== claim.serial) return false;
    if (remaining.length > 0) {
      const bytes = remaining.reduce((sum, record) => sum + retainedBytes(record), 0);
      queue.records.prepend(remaining);
      queue.bytes += bytes;
      target.messages += remaining.length;
      target.bytes += bytes;
    }
    queue.claimed = false;
    queue.claimSerial = 0n;
    if (queue.records.length === 0) {
      target.owners.delete(claim.owner);
    } else if (!target.indexed.has(claim.owner)) {
      target.indexed.add(claim.owner);
      target.ready.push(claim.owner);
    }
    return true;
  }

  trySealApplicationOwner(owner: string): ServiceMailboxRelocationSeal | undefined {
    if (owner.length === 0 || this.closed || this.relocatedOwners.has(owner)) return undefined;
    if (this.relocationSeals.has(owner)) return undefined;
    const queue = this.application.owners.get(owner);
    if (queue?.claimed === true) return undefined;
    const captured = queue?.records.drain() ?? [];
    if (queue !== undefined) {
      queue.bytes = 0;
      this.application.owners.delete(owner);
      this.application.indexed.delete(owner);
    }
    const serial = this.nextRelocationSerial++;
    this.relocationSeals.set(owner, { serial, captured, held: [], heldBytes: 0 });
    return { owner, serial, captured };
  }

  abortRelocation(seal: ServiceMailboxRelocationSeal): boolean {
    const current = this.relocationSeals.get(seal.owner);
    if (current === undefined || current.serial !== seal.serial) return false;
    this.relocationSeals.delete(seal.owner);
    const records = [...current.captured, ...current.held];
    if (records.length === 0) return true;
    const bytes = records.reduce((sum, record) => sum + retainedBytes(record), 0);
    this.application.owners.set(seal.owner, {
      records: new MailboxQueue(records),
      bytes,
      claimed: false,
      claimSerial: 0n
    });
    if (!this.application.indexed.has(seal.owner)) {
      this.application.indexed.add(seal.owner);
      this.application.ready.push(seal.owner);
    }
    return true;
  }

  commitRelocation(seal: ServiceMailboxRelocationSeal): readonly ServiceMailboxRecord[] | undefined {
    const current = this.relocationSeals.get(seal.owner);
    if (current === undefined || current.serial !== seal.serial) return undefined;
    this.relocationSeals.delete(seal.owner);
    this.relocatedOwners.add(seal.owner);
    const released = [...current.captured, ...current.held];
    this.application.messages -= released.length;
    this.application.bytes -= released.reduce((sum, record) => sum + retainedBytes(record), 0);
    return current.held;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    clearDomain(this.application);
    clearDomain(this.infrastructure);
    this.relocationSeals.clear();
    this.relocatedOwners.clear();
  }

  pendingMessages(domain: ServiceMailboxDomain): number {
    return this.domain(domain).messages;
  }

  pendingBytes(domain: ServiceMailboxDomain): number {
    return this.domain(domain).bytes;
  }

  private domain(domain: ServiceMailboxDomain): DomainState {
    return domain === 'application' ? this.application : this.infrastructure;
  }
}

function createDomain(messageBudget: number, byteBudget: number): DomainState {
  requirePositive(messageBudget, 'messageBudget');
  requirePositive(byteBudget, 'byteBudget');
  return {
    owners: new Map(),
    ready: new MailboxQueue(),
    indexed: new Set(),
    messages: 0,
    bytes: 0,
    messageBudget,
    byteBudget
  };
}

function clearDomain(domain: DomainState): void {
  domain.owners.clear();
  domain.ready.clear();
  domain.indexed.clear();
  domain.messages = 0;
  domain.bytes = 0;
}

/**
 * Internal FIFO used by the mailbox hot path. Two stacks keep dequeue and
 * prepend operations amortized O(1), avoiding array reindexing from shift and
 * unshift while retaining the queue's existing ordering semantics.
 */
class MailboxQueue<T> {
  private readonly front: T[] = [];
  private readonly back: T[] = [];

  constructor(initial?: readonly T[]) {
    if (initial !== undefined) {
      for (const value of initial) this.back.push(value);
    }
  }

  get length(): number {
    return this.front.length + this.back.length;
  }

  push(value: T): void {
    this.back.push(value);
  }

  peek(): T | undefined {
    this.moveBackToFront();
    return this.front.at(-1);
  }

  shift(): T | undefined {
    this.moveBackToFront();
    return this.front.pop();
  }

  prepend(values: readonly T[]): void {
    for (let index = values.length - 1; index >= 0; index -= 1) {
      this.front.push(values[index]!);
    }
  }

  drain(): T[] {
    this.moveBackToFront();
    const drained: T[] = [];
    while (this.front.length > 0) {
      drained.push(this.front.pop()!);
    }
    return drained;
  }

  clear(): void {
    this.front.length = 0;
    this.back.length = 0;
  }

  private moveBackToFront(): void {
    if (this.front.length > 0 || this.back.length === 0) return;
    for (let index = this.back.length - 1; index >= 0; index -= 1) {
      this.front.push(this.back[index]!);
    }
    this.back.length = 0;
  }
}

function retainRecord(record: ServiceMailboxRecord): ServiceMailboxRecord {
  // Raw binding ingress already transfers owned Buffer values to the
  // framework. Moving the record between mailbox queues must not create a
  // second representation of the same payload.
  return record;
}

function retainedBytes(record: ServiceMailboxRecord): number {
  return record.parts.reduce((sum, part) => sum + part.byteLength, 0);
}

function requirePositive(value: number, field: string): void {
  if (!Number.isSafeInteger(value) || value < 1) {
    throw new RangeError(`${field} must be a positive safe integer.`);
  }
}
