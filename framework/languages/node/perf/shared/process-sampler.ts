import { monitorEventLoopDelay, IntervalHistogram } from 'node:perf_hooks';
import { NullReason, nullReason } from './contracts';
import { PerfClock } from './clock';

// Process resources of one owner window (§14, §17.4): CPU time, peak RSS of 100 ms samples, heap and event-loop delay.
export class ProcessSampler {
  private readonly intervals: bigint[] = [];
  private started = 0n;
  private lastSample = 0n;
  private rssMax = 0;
  private heapUsedMax = 0;
  private cpuStart = process.cpuUsage();
  private cpuEnd = this.cpuStart;
  private delay: IntervalHistogram | undefined;
  private delaySummary: Record<string, number> | undefined;

  start(): void {
    this.started = this.lastSample = PerfClock.now();
    this.cpuStart = process.cpuUsage();
    const memory = process.memoryUsage();
    this.rssMax = memory.rss;
    this.heapUsedMax = memory.heapUsed;
    this.intervals.length = 0;
    this.delay?.disable();
    this.delay = monitorEventLoopDelay({ resolution: 10 });
    this.delay.enable();
  }

  sample(): void {
    const now = PerfClock.now();
    this.intervals.push(now - this.lastSample);
    this.lastSample = now;
    const memory = process.memoryUsage();
    this.rssMax = Math.max(this.rssMax, memory.rss);
    this.heapUsedMax = Math.max(this.heapUsedMax, memory.heapUsed);
  }

  end(): void {
    this.sample();
    this.cpuEnd = process.cpuUsage();
    if (this.delay) {
      this.delay.disable();
      const ms = (value: number): number => value / 1e6;
      this.delaySummary = {
        meanMs: ms(this.delay.mean),
        p50Ms: ms(this.delay.percentile(50)),
        p99Ms: ms(this.delay.percentile(99)),
        maxMs: ms(this.delay.max)
      };
    }
  }

  export(
    metrics: Record<string, unknown>,
    runtime: Record<string, unknown>,
    reasons: Record<string, NullReason>
  ): void {
    const seconds = Number(this.lastSample - this.started) / 1e9;
    const cpuSeconds =
      (this.cpuEnd.user - this.cpuStart.user + this.cpuEnd.system - this.cpuStart.system) / 1e6;
    metrics['process.cpuPercent'] = seconds > 0 ? (cpuSeconds / seconds) * 100 : 0;
    metrics['process.rssMb'] = this.rssMax / 1048576;
    // Node exposes no cumulative allocation counter and no GC generations; the collector counts are not renamed to them (§14).
    for (const key of ['process.allocatedMb', 'gc.gen0', 'gc.gen1', 'gc.gen2']) {
      metrics[key] = null;
      reasons[`/metrics/${key}`] = nullReason(
        'RUNTIME_METRIC_UNSUPPORTED',
        'Node.js exposes no cumulative allocation counter or GC generation counts.'
      );
    }
    runtime.rssSamplingInterval = {
      name: 'process.memoryUsage() RSS sample interval',
      unit: 'ns',
      type: 'sampling',
      value: {
        requestedIntervalNs: '100000000',
        actualIntervalsNs: this.intervals.map(String),
        startedTicks: this.started.toString(),
        endedTicks: this.lastSample.toString()
      }
    };
    runtime.rssMaxBytes = {
      name: 'process.memoryUsage().rss maximum',
      unit: 'bytes',
      type: 'number',
      value: this.rssMax
    };
    runtime.cpuObservationSeconds = {
      name: 'process.cpuUsage() observation span',
      unit: 's',
      type: 'number',
      value: seconds
    };
    runtime.heapUsedMb = {
      name: 'process.memoryUsage().heapUsed maximum of the RSS samples',
      unit: 'MiB',
      type: 'number',
      value: this.heapUsedMax / 1048576
    };
    runtime.eventLoopDelay = {
      name: 'perf_hooks.monitorEventLoopDelay (10 ms resolution)',
      unit: 'ms',
      type: 'summary',
      value: this.delaySummary ?? null
    };
  }

  dispose(): void {
    this.delay?.disable();
  }
}
