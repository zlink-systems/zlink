'use strict';
// Compact per-stream identity intervals. No payload or correlation string retention.
class Ranges {
  constructor() { this.streams = new Map(); this.duplicates = 0n; this.duplicatesByStream = new Map(); }
  add(stream, sequence) {
    const value = BigInt(sequence);
    let ranges = this.streams.get(stream);
    if (!ranges) { ranges = []; this.streams.set(stream, ranges); }
    let left = 0, right = ranges.length;
    while (left < right) { const mid = (left + right) >>> 1; if (ranges[mid][1] < value) left = mid + 1; else right = mid; }
    if (left < ranges.length && ranges[left][0] <= value) { this.duplicates++; this.duplicatesByStream.set(stream, (this.duplicatesByStream.get(stream) ?? 0n) + 1n); return false; }
    const previous = left > 0 && ranges[left - 1][1] + 1n === value;
    const next = left < ranges.length && ranges[left][0] === value + 1n;
    if (previous && next) { ranges[left - 1][1] = ranges[left][1]; ranges.splice(left, 1); }
    else if (previous) ranges[left - 1][1] = value;
    else if (next) ranges[left][0] = value;
    else ranges.splice(left, 0, [value, value]);
    return true;
  }
  forStream(stream) { return (this.streams.get(stream) ?? []).map(([first, last]) => ({ first: String(first), last: String(last) })); }
  export() { return [...this.streams.keys()].sort((a, b) => a - b).map(clientId => ({ clientId, ranges: this.forStream(clientId) })); }
  count() { let n = 0n; for (const ranges of this.streams.values()) for (const [first, last] of ranges) n += last - first + 1n; return n; }
}
module.exports = { Ranges };
