import * as fs from 'node:fs';
import { Identity, NullReason } from '../shared/contracts';
import { LATENCY_SUFFIXES } from '../shared/histogram';
import { MetricCatalog } from '../shared/metric-catalog';
import { PerfMetricsSnapshot } from '../shared/measurement';

// Perf spec §15.4: sequence originals of the PS cells. The Publisher and Subscriber role projects share this file, so the
// original format has one definition.
export interface SequenceRange {
  first: string;
  last: string;
}

export interface PublisherSequences extends Identity {
  attemptedRanges: SequenceRange[];
  windowSuccessRanges: SequenceRange[];
}

export interface SubscriberSequences extends Identity {
  subscriberId: number;
  windowRanges: SequenceRange[];
  duplicateEvents: string;
  nullReasons: Record<string, NullReason>;
  timingEvidence: unknown[] | null;
}

// A set of U64 sequences kept as a chunked bit set: one bit per sequence. Sequences stay below 2^53 in a run, so they are
// plain numbers here and become canonical decimal text only in the original.
const CHUNK_BITS = 1 << 18;
const CHUNK_WORDS = CHUNK_BITS / 32;

export class SequenceBitSet {
  private readonly chunks = new Map<number, Uint32Array>();
  private size = 0;

  get count(): number {
    return this.size;
  }
  get retainedBytes(): number {
    return this.chunks.size * CHUNK_WORDS * 4;
  }

  // False when the sequence was already in the set.
  trySet(sequence: number): boolean {
    const index = Math.floor(sequence / CHUNK_BITS);
    let chunk = this.chunks.get(index);
    if (!chunk) {
      chunk = new Uint32Array(CHUNK_WORDS);
      this.chunks.set(index, chunk);
    }
    const bit = sequence % CHUNK_BITS;
    const mask = 1 << (bit & 31);
    if ((chunk[bit >> 5] & mask) !== 0) return false;
    chunk[bit >> 5] |= mask;
    this.size++;
    return true;
  }

  contains(sequence: number): boolean {
    const chunk = this.chunks.get(Math.floor(sequence / CHUNK_BITS));
    if (!chunk) return false;
    const bit = sequence % CHUNK_BITS;
    return (chunk[bit >> 5] & (1 << (bit & 31))) !== 0;
  }

  // Maximal contiguous intervals, ascending, both ends inclusive (§15.4).
  ranges(): SequenceRange[] {
    const ranges: SequenceRange[] = [];
    let open: number | undefined;
    let next = 0; // the sequence expected next while an interval is open
    const close = (): void => {
      if (open !== undefined) ranges.push({ first: String(open), last: String(next - 1) });
      open = undefined;
    };
    const add = (first: number, length: number): void => {
      if (open === undefined) open = first;
      else if (first !== next) {
        close();
        open = first;
      }
      next = first + length;
    };
    for (const index of [...this.chunks.keys()].sort((a, b) => a - b)) {
      const chunk = this.chunks.get(index)!;
      const origin = index * CHUNK_BITS;
      for (let word = 0; word < CHUNK_WORDS; word++) {
        const bits = chunk[word];
        const position = origin + word * 32;
        if (bits === 0) {
          close();
          continue;
        }
        if (bits === 0xffffffff) {
          add(position, 32);
          continue;
        }
        let bit = 0;
        while (bit < 32) {
          if (((bits >>> bit) & 1) === 0) {
            bit++;
            continue;
          }
          let run = 0;
          while (bit + run < 32 && ((bits >>> (bit + run)) & 1) === 1) run++;
          add(position + bit, run);
          bit += run;
        }
      }
    }
    close();
    return ranges;
  }
}

// Metric keys a PS role owns or hands to the runner's sequence intersection (§14, §15.4).
const INTERSECTION_KEYS = [
  'fanout.subscriberCount',
  'fanout.deliveredInWindow',
  'fanout.outOfCohortEvents',
  'fanout.deliveryRatio',
  'fanout.deliveryOpsPerSec'
];
const ECHO_KEYS = ['messages.completed', 'throughput.kops'];

export const FanoutMetrics = {
  topic: 'perf.echo',

  value(snapshot: PerfMetricsSnapshot, key: string, value: unknown): void {
    snapshot.metrics[key] = value;
    delete snapshot.nullReasons[`/metrics/${key}`];
  },

  setNull(snapshot: PerfMetricsSnapshot, key: string, code: string, reason: string): void {
    MetricCatalog.setNull(snapshot.metrics, snapshot.nullReasons, 'metrics', key, code, reason);
  },

  // Every PS role: the echo outcomes and echo latency do not apply (§10.11); delivery is intersected by the runner.
  applyCommon(snapshot: PerfMetricsSnapshot, hasDeliveryOwner: boolean): void {
    for (const key of INTERSECTION_KEYS)
      FanoutMetrics.setNull(
        snapshot,
        key,
        'NOT_APPLICABLE',
        "Delivery counts come from the runner's intersection of the publisher and subscriber sequence originals (§15.4)."
      );
    for (const suffix of LATENCY_SUFFIXES)
      FanoutMetrics.setNull(
        snapshot,
        `latency.${suffix}`,
        'NOT_APPLICABLE',
        'A fanout cell has no echo round trip (§10.11).'
      );
    for (const key of ['latencyMs']) {
      MetricCatalog.setNull(
        snapshot.histograms,
        snapshot.nullReasons,
        'histograms',
        key,
        'NOT_APPLICABLE',
        'A fanout cell has no echo round trip (§10.11).'
      );
      delete snapshot.nullReasons[`/histograms/${key}/maxNs`];
    }
    for (const key of ECHO_KEYS)
      FanoutMetrics.setNull(
        snapshot,
        key,
        'NOT_APPLICABLE',
        'A fanout cell records publish admission, not echo completion (§10.11).'
      );
    const code = hasDeliveryOwner ? 'CLOCK_DOMAIN_UNVERIFIED' : 'NOT_APPLICABLE';
    for (const suffix of LATENCY_SUFFIXES)
      FanoutMetrics.setNull(
        snapshot,
        `fanout.deliveryLatency.${suffix}`,
        code,
        hasDeliveryOwner
          ? 'Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2).'
          : 'Delivery latency is observed by Subscriber processes.'
      );
    for (const key of ['fanoutDeliveryLatencyMs'])
      MetricCatalog.setNull(
        snapshot.histograms,
        snapshot.nullReasons,
        'histograms',
        key,
        code,
        hasDeliveryOwner
          ? 'No verified shared clock domain between Publisher and Subscriber processes (§15.2).'
          : 'Delivery latency is observed by Subscriber processes.'
      );
  },

  // The original of this cell is written once and never replaced.
  writeOnce(path: string, original: unknown): void {
    fs.writeFileSync(path, JSON.stringify(original) + '\n', { flag: 'wx' });
  }
};
