import { NullReason, nullReason } from '../shared/contracts';
import { Histogram, LATENCY_SUFFIXES } from '../shared/histogram';
import { Measurement, PerfMetricsSnapshot } from '../shared/measurement';

// The §14 family keys a scenario fills beside the shared echo counters (driver.*, spot.*, worker.*, messages.admitted...).
// Counters and histograms clear with the window at reset and are written into every snapshot; a key the scenario
// declares as unsupported keeps its null with the public-observation reason. Keys it does not name stay as
// Measurement wrote them (null, NOT_APPLICABLE).
export class ScenarioMetrics {
  private counts = new Map<string, number>();
  private histograms = new Map<string, { prefix: string; histogram: Histogram }>();
  private readonly resets: (() => void)[] = [];
  private readonly aliases: { from: string; to: string }[] = [];
  private readonly unsupported = new Map<string, { code: string; reason: string }>();
  private readonly unsupportedHistograms = new Map<string, { code: string; reason: string }>();
  private readonly provenanceValues = new Map<string, unknown>();

  constructor(private readonly measurement: Measurement) {
    measurement.onReset = () => this.reset();
    measurement.enrichSnapshot = (snapshot) => this.enrich(snapshot);
  }

  // Counter keys the scenario measures; they read "0" until observed instead of staying null.
  counters(...keys: string[]): this {
    for (const key of keys) if (!this.counts.has(key)) this.counts.set(key, 0);
    return this;
  }

  // A latency histogram (§15.3): the histogram key and the dotted metric prefix it exports.
  latency(histogramKey: string, prefix: string): this {
    this.histograms.set(histogramKey, { prefix, histogram: new Histogram() });
    return this;
  }

  markUnsupported(code: string, reason: string, ...keys: string[]): this {
    for (const key of keys) this.unsupported.set(key, { code, reason });
    return this;
  }

  markHistogramsUnsupported(code: string, reason: string, ...keys: string[]): this {
    for (const key of keys) this.unsupportedHistograms.set(key, { code, reason });
    return this;
  }

  // §14.1: no public observation of a Spot's mailbox or turn internals; a Spot workload keeps these null with that reason.
  spotInternalsUnsupported(): this {
    return this.markUnsupported('PUBLIC_OBSERVATION_UNSUPPORTED', 'Public status is a host aggregate; no per-Spot mailbox, turn or resume observation exists.',
      'spot.mailboxDepth.max', 'spot.mailboxDepth.mean', 'spot.suspendedTurns', 'spot.resumedTurns', 'spot.resumeLatency.p95Ms', 'spot.resumeLatency.p99Ms');
  }

  // A metric family that is the same interval as another (§15.3: spot.remoteCallLatency.* is latency.*).
  aliasLatency(fromPrefix: string, toPrefix: string): this {
    this.aliases.push({ from: fromPrefix, to: toPrefix });
    return this;
  }

  provenance(key: string, value: unknown): this {
    this.provenanceValues.set(key, value);
    return this;
  }

  // State a scenario keeps beside the counters (a correlation table) clears with the same reset.
  onReset(reset: () => void): void {
    this.resets.push(reset);
  }

  count(key: string, amount = 1): void {
    this.counts.set(key, (this.counts.get(key) ?? 0) + amount);
  }

  // Only a sample whose operation finished inside the measured window belongs to the window histogram.
  // windowTicks: when the operation this interval belongs to finished (a worker interval ends before its operation does).
  record(histogramKey: string, startedTicks: bigint, endedTicks: bigint, windowTicks: bigint = endedTicks): void {
    if (windowTicks >= this.measurement.endTicks) return;
    this.histograms.get(histogramKey)!.histogram.record(endedTicks - startedTicks);
  }

  private reset(): void {
    for (const key of this.counts.keys()) this.counts.set(key, 0);
    for (const [key, entry] of this.histograms) this.histograms.set(key, { prefix: entry.prefix, histogram: new Histogram() });
    for (const reset of this.resets) reset();
  }

  private enrich(snapshot: PerfMetricsSnapshot): void {
    const reasons: Record<string, NullReason> = snapshot.nullReasons;
    for (const [key, value] of this.counts) {
      snapshot.metrics[key] = String(value);
      delete reasons[`/metrics/${key}`];
    }
    for (const [key, { prefix, histogram }] of this.histograms) {
      for (const suffix of LATENCY_SUFFIXES) delete reasons[`/metrics/${prefix}.${suffix}`];
      delete reasons[`/histograms/${key}`];
      histogram.export(key, prefix, snapshot.metrics, snapshot.histograms, reasons);
    }
    for (const { from, to } of this.aliases) {
      for (const suffix of LATENCY_SUFFIXES) {
        snapshot.metrics[`${to}.${suffix}`] = snapshot.metrics[`${from}.${suffix}`];
        const reason = reasons[`/metrics/${from}.${suffix}`];
        if (reason) reasons[`/metrics/${to}.${suffix}`] = reason;
        else delete reasons[`/metrics/${to}.${suffix}`];
      }
    }
    for (const [key, { code, reason }] of this.unsupported) {
      snapshot.metrics[key] = null;
      reasons[`/metrics/${key}`] = nullReason(code, reason, 'spec/server/06-observability/01-runtime-monitoring');
    }
    for (const [key, { code, reason }] of this.unsupportedHistograms) {
      snapshot.histograms[key] = null;
      reasons[`/histograms/${key}`] = nullReason(code, reason);
    }
    for (const [key, value] of this.provenanceValues) snapshot.provenance[key] = value;
  }
}
