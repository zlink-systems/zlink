import { PerfClock } from '../shared/clock';
import { PerfEchoReply, PerfEchoRequest, PerfValidationException } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ScenarioMetrics } from './scenario-metrics';

// The harness correlation of a send/send operation (§13): the first public send and the return send are two one-way
// calls, tied together only by the correlationId in the DTO. The first result of a correlation stands; a reply that
// arrives after that is only counted (duplicate, late or unknown). The table clears with the window at reset.
//
// Order of one operation: register (right before the first public send, fixing the expiry deadline), then
// firstSendEnded with that send's terminal, then completeAsync for the final result. The return handler calls reply.
enum State { Pending, Succeeded, Failed, Expired }

export class CorrelationEntry {
  state = State.Pending;
  closedTicks = 0n;
  request: PerfEchoRequest | undefined;
  readonly result: Promise<Error | undefined>;
  private resolveResult!: (error: Error | undefined) => void;

  constructor(request: PerfEchoRequest, readonly startedTicks: bigint, readonly expiresAtTicks: bigint) {
    this.request = request;
    this.result = new Promise((resolve) => { this.resolveResult = resolve; });
  }

  close(state: State, error: Error | undefined, closedTicks = PerfClock.now()): boolean {
    if (this.state !== State.Pending) return false;
    this.state = state;
    this.closedTicks = closedTicks;
    this.request = undefined;
    this.resolveResult(error);
    return true;
  }
}

export class SendSendCorrelation {
  private entries = new Map<string, CorrelationEntry>();

  constructor(private readonly measurement: Measurement, private readonly metrics: ScenarioMetrics) {
    metrics.counters('messages.admitted', 'messages.expired', 'messages.duplicateReply', 'messages.lateReply', 'messages.unknownCorrelation');
    metrics.onReset(() => this.entries.clear());
  }

  register(request: PerfEchoRequest, startedTicks: bigint): CorrelationEntry {
    const expires = PerfClock.now() + BigInt(this.measurement.config.workload.correlationExpiryMs) * 1_000_000n;
    const entry = new CorrelationEntry(request, startedTicks, expires);
    if (this.entries.has(request.correlationId)) throw new PerfValidationException('IdentityMismatch', 'A correlationId was issued twice.');
    this.entries.set(request.correlationId, entry);
    return entry;
  }

  find(correlationId: string): CorrelationEntry | undefined {
    return this.entries.get(correlationId);
  }

  // The terminal of the first public send: a normal admission is counted; a failure is the final result unless
  // the echo was already fixed first.
  firstSendEnded(entry: CorrelationEntry, error: unknown): void {
    const now = PerfClock.now();
    if (this.expireIfDue(entry, now)) return;
    if (error === undefined || error === null) {
      if (this.measurement.phase !== 'setup') this.metrics.count('messages.admitted');
    } else entry.close(State.Failed, error instanceof Error ? error : new Error(String(error)), now);
  }

  // The return handler's one call: the reply's identity and payload decide the first result.
  reply(reply: PerfEchoReply): void {
    const entry = this.entries.get(reply.correlationId);
    if (!entry) { this.metrics.count('messages.unknownCorrelation'); return; }
    const request = entry.request;
    let invalid: Error | undefined;
    if (request !== undefined) {
      try {
        PayloadPattern.validateIdentity(request, reply);
        this.measurement.pattern.validate(reply.payload);
      } catch (error) {
        if (!(error instanceof PerfValidationException)) throw error;
        invalid = error;
      }
    }
    const now = PerfClock.now();
    this.expireIfDue(entry, now);
    if (!entry.close(invalid === undefined ? State.Succeeded : State.Failed, invalid, now))
      this.metrics.count(entry.state === State.Succeeded ? 'messages.duplicateReply' : 'messages.lateReply');
  }

  // The final result once the first send has ended: the first result of the correlation, or its expiry. The time is
  // when that result was fixed, so an echo seen before the first send's terminal keeps its own time.
  async completeAsync(entry: CorrelationEntry): Promise<{ error: Error | undefined; completedTicks: bigint }> {
    const now = PerfClock.now();
    this.expireIfDue(entry, now);
    const remainingMs = Math.max(0, Number(entry.expiresAtTicks - now) / 1e6);
    let timer: NodeJS.Timeout | undefined;
    const expired = new Promise<'expired'>((resolve) => { timer = setTimeout(() => {
      this.expireIfDue(entry, PerfClock.now());
      resolve('expired');
    }, Math.ceil(remainingMs)); });
    const winner = await Promise.race([entry.result.then(() => 'closed' as const), expired]);
    clearTimeout(timer);
    if (winner === 'expired') this.expireIfDue(entry, PerfClock.now());
    return { error: await entry.result, completedTicks: entry.closedTicks };
  }

  private expireIfDue(entry: CorrelationEntry, now: bigint): boolean {
    if (entry.state !== State.Pending || now < entry.expiresAtTicks) return false;
    if (entry.close(State.Expired, new PerfValidationException('CorrelationExpired', 'No return send arrived before the correlation deadline.'), now))
      this.metrics.count('messages.expired');
    return true;
  }
}
