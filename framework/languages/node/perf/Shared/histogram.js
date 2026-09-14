'use strict';
const fs = require('node:fs');
const path = require('node:path');
const fixturePath = path.resolve(__dirname, '../../../../perf-contract/histogram-bounds-ns.json');
class Histogram {
  constructor(bounds = JSON.parse(fs.readFileSync(fixturePath, 'utf8'))) {
    this.bounds = bounds.map(BigInt); this.counts = bounds.map(() => 0n);
    this.count = 0n; this.overflow = 0n; this.sum = 0n; this.max = 0n;
  }
  record(ns) {
    ns = BigInt(ns);
    if (ns < 0n) throw new RangeError('Negative elapsed time.');
    let left = 0, right = this.bounds.length;
    while (left < right) { const mid = (left + right) >>> 1; if (this.bounds[mid] < ns) left = mid + 1; else right = mid; }
    if (left === this.bounds.length) this.overflow++; else this.counts[left]++;
    this.count++; this.sum += ns; if (ns > this.max) this.max = ns;
  }
  percentile(p) {
    if (!this.count) return null;
    const rank = (BigInt(Math.round(p * 10)) * this.count + 999n) / 1000n;
    let total = 0n;
    for (let i = 0; i < this.counts.length; i++) { total += this.counts[i]; if (total >= rank) return Number(this.bounds[i]) / 1e6; }
    return null;
  }
  snapshot() {
    return { unit: 'ms', ticksUnit: 'ns', bucketSpec: 'ns-1us-1pct-60s-v1', boundsNs: this.bounds.map(String), counts: this.counts.map(String), overflow: String(this.overflow), count: String(this.count), sumNs: String(this.sum), maxNs: this.count ? String(this.max) : null, percentileMethod: 'nearest-rank-bucket-upper-bound' };
  }
  export(key, prefix, metrics, histograms, reasons) {
    histograms[key] = this.snapshot();
    for (const suffix of ['meanMs', 'p50Ms', 'p95Ms', 'p99Ms', 'p999Ms', 'maxMs']) {
      const value = !this.count ? null : suffix === 'meanMs' ? Number(this.sum) / Number(this.count) / 1e6 : suffix === 'maxMs' ? Number(this.max) / 1e6 : suffix === 'p999Ms' && this.count < 100000n ? null : this.percentile(suffix === 'p999Ms' ? 99.9 : Number(suffix.slice(1, 3)));
      metrics[`${prefix}.${suffix}`] = value;
      if (value === null) reasons[`/metrics/${prefix}.${suffix}`] = { code: suffix === 'p999Ms' && this.count < 100000n ? 'INSUFFICIENT_SAMPLES' : !this.count ? 'NO_SAMPLES' : 'HISTOGRAM_OVERFLOW', reason: !this.count ? 'No successful samples.' : suffix === 'p999Ms' && this.count < 100000n ? 'p999 requires 100000 successful samples.' : 'Nearest rank above final bound.', owner: 'perf/README.ko.md', ...(this.count && !(suffix === 'p999Ms' && this.count < 100000n) ? { lowerBoundMs: Number(this.bounds.at(-1)) / 1e6 } : {}) };
    }
    if (!this.count) reasons[`/histograms/${key}/maxNs`] = { code: 'NO_SAMPLES', reason: 'No successful samples.', owner: 'perf/README.ko.md' };
  }
}
module.exports = { Histogram, fixturePath };
