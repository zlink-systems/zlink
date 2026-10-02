import * as fs from 'node:fs';
import * as path from 'node:path';
import { NullReason, nullReason } from './contracts';

// The bucket bounds have one owner: framework/perf/schema/histogram-bounds.json (perf §6.5, §15.3).
// build/shared/histogram.js -> framework/perf/schema is five folders up from this file.
const BOUNDS_FILE = path.resolve(__dirname, '../../../../../perf/schema/histogram-bounds.json');
export const HISTOGRAM_BOUNDS: number[] = JSON.parse(fs.readFileSync(BOUNDS_FILE, 'utf8')) as number[];
const BOUNDS_NS: bigint[] = HISTOGRAM_BOUNDS.map((value) => BigInt(Math.round(value * 1_000_000)));

export interface HistogramSnapshot {
  unit: 'ms';
  ticksUnit: 'ns';
  bounds: number[];
  counts: string[];
  overflow: string;
  count: string;
  sumNs: string;
  maxNs: string | null;
  percentileMethod: 'nearest-rank-bucket-upper-bound-capped-by-max';
}

export const LATENCY_SUFFIXES = ['meanMs', 'p50Ms', 'p95Ms', 'p99Ms', 'maxMs'] as const;

export class Histogram {
  private readonly counts: number[] = HISTOGRAM_BOUNDS.map(() => 0);
  private count = 0;
  private overflow = 0;
  private sum = 0n;
  private max = 0n;

  record(elapsedNs: bigint): void {
    if (elapsedNs < 0n) throw new RangeError('elapsedNs must not be negative.');
    this.count++;
    let bucket = 0;
    while (bucket < BOUNDS_NS.length && elapsedNs > BOUNDS_NS[bucket]) bucket++;
    if (bucket === BOUNDS_NS.length) this.overflow++;
    else this.counts[bucket]++;
    this.sum += elapsedNs;
    if (elapsedNs > this.max) this.max = elapsedNs;
  }

  snapshot(): HistogramSnapshot {
    return {
      unit: 'ms', ticksUnit: 'ns', bounds: [...HISTOGRAM_BOUNDS], counts: this.counts.map(String), overflow: String(this.overflow),
      count: String(this.count), sumNs: this.sum.toString(), maxNs: this.count === 0 ? null : this.max.toString(),
      percentileMethod: 'nearest-rank-bucket-upper-bound-capped-by-max'
    };
  }

  // §15.3: mean and max come from the exact sum and max, percentiles from the nearest-rank bucket upper bound.
  export(histogramKey: string, metricPrefix: string, metrics: Record<string, unknown>, histograms: Record<string, unknown>,
    reasons: Record<string, NullReason>): void {
    histograms[histogramKey] = this.snapshot();
    for (const name of LATENCY_SUFFIXES) {
      const key = `${metricPrefix}.${name}`;
      let value: number | null = null;
      if (this.count !== 0) {
        if (name === 'meanMs') value = Number(this.sum) / this.count / 1_000_000;
        else if (name === 'maxMs') value = Number(this.max) / 1_000_000;
        else value = this.percentile(Number(name.slice(1, 3)));
      }
      metrics[key] = value;
      if (value === null) {
        reasons[`/metrics/${key}`] = this.count === 0
          ? nullReason('NO_SAMPLES', 'No successful samples in this cohort and window.')
          : nullReason('HISTOGRAM_OVERFLOW', 'Nearest rank lies above the final bucket.', 'perf/README.ko.md', HISTOGRAM_BOUNDS[HISTOGRAM_BOUNDS.length - 1]);
      }
    }
    if (this.count === 0) reasons[`/histograms/${histogramKey}/maxNs`] = nullReason('NO_SAMPLES', 'No successful samples in this cohort and window.');
  }

  private percentile(percentile: number): number | null {
    const rank = Math.floor((percentile * this.count + 99) / 100);
    let cumulative = 0;
    for (let i = 0; i < this.counts.length; i++) {
      cumulative += this.counts[i];
      if (cumulative >= rank) return Math.min(HISTOGRAM_BOUNDS[i], Number(this.max) / 1_000_000);
    }
    return null;
  }
}
