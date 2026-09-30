import { randomBytes } from 'node:crypto';

export interface ClockMetadata {
  source: string;
  nativeFrequencyHz: string;
  ticksUnit: 'ns';
  clockDomainId: string;
  scope: 'process' | 'host' | 'aligned-hosts';
  alignmentMethod: string | null;
  maxErrorNs: string | null;
  validFromTicks: string | null;
  validThroughTicks: string | null;
  evidence: string[];
}

// §15.2: elapsed time uses one monotonic clock, in nanoseconds. Unix time is only a label.
export const PerfClock = {
  domain: `process-${process.pid}-${randomBytes(16).toString('hex')}`,
  now: (): bigint => process.hrtime.bigint(),
  unixMs: (): string => String(Date.now()),
  metadata: (): ClockMetadata => ({
    source: 'process.hrtime.bigint',
    // hrtime.bigint is a nanosecond counter; Node documents no native tick frequency.
    nativeFrequencyHz: '1000000000',
    ticksUnit: 'ns',
    clockDomainId: PerfClock.domain,
    scope: 'process',
    alignmentMethod: null,
    maxErrorNs: null,
    validFromTicks: null,
    validThroughTicks: null,
    evidence: ['process.hrtime.bigint is monotonic (Node documentation).', 'RTT uses only this process clock; remote receivedTicks is diagnostic.']
  })
};
