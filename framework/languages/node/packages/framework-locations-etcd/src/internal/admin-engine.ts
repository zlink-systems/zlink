import { EtcdClient, Operation, RangeResponse, TxnResponse } from './client';
import { decodeClock, decodeMetadata, encodeClock, keys, Metadata, prefixEnd, StoreFormatError } from './encoding';

const CLOCK_CHECK_INTERVAL_MS = 1000;

function stopped(signal?: AbortSignal): boolean { return signal?.aborted ?? false; }

export class MissingClockError extends Error {
  constructor() { super('Store clock key is missing'); }
}

export interface ClockSample { value: bigint; revision: string }
export interface ClockStore {
  operation?(signal?: AbortSignal): Operation;
  readClock(operation?: Operation): Promise<ClockSample>;
  compareClock(revision: string, value: bigint, operation?: Operation): Promise<boolean>;
}

export interface EngineTime {
  monotonicMs(): bigint;
  wallMs(): bigint;
  sleep(ms: number, signal?: AbortSignal): Promise<void>;
}

export const systemTime: EngineTime = {
  monotonicMs: () => process.hrtime.bigint() / 1_000_000n,
  wallMs: () => BigInt(Date.now()),
  sleep: (ms, signal) => new Promise(resolve => {
    if (stopped(signal)) { resolve(); return; }
    const finish = () => { clearTimeout(timer); signal?.removeEventListener('abort', finish); resolve(); };
    const timer = setTimeout(finish, ms);
    signal?.addEventListener('abort', finish, { once: true });
  })
};

export class ClockEngine {
  private sample: { clock: ClockSample; at: bigint } | undefined;
  constructor(private readonly store: ClockStore, private readonly time: EngineTime = systemTime) {}

  async step(signal?: AbortSignal): Promise<void> {
    if (stopped(signal)) return;
    const operation = this.store.operation?.(signal);
    try {
      if (this.sample === undefined) {
        const clock = await this.store.readClock(operation);
        this.sample = { clock, at: this.time.monotonicMs() };
      }
      const previous = this.sample;
      if (stopped(signal)) return;
      await this.time.sleep(CLOCK_CHECK_INTERVAL_MS, signal);
      if (stopped(signal)) return;
      const next = await this.store.readClock(operation);
      const receivedAt = this.time.monotonicMs();
      if (next.revision !== previous.clock.revision) {
        this.sample = { clock: next, at: receivedAt };
        return;
      }
      if (stopped(signal)) return;
      const elapsed = this.time.monotonicMs() - previous.at;
      let increment = elapsed < 1000n ? elapsed : 1000n;
      const wall = this.time.wallMs();
      if (wall > previous.clock.value + increment) {
        const correction = wall - previous.clock.value;
        increment = increment + 10n < correction ? increment + 10n : correction;
      }
      await this.store.compareClock(previous.clock.revision, previous.clock.value + increment, operation);
      this.sample = undefined;
    } catch (error) {
      this.sample = undefined;
      throw error;
    }
  }
}

export class EtcdAdminStore implements ClockStore {
  private readonly key;
  constructor(private readonly client: EtcdClient) { this.key = keys(client.options.namespace); }
  operation(signal?: AbortSignal): Operation { return this.client.operation(signal); }

  async readClock(operation = this.operation()): Promise<ClockSample> {
    const response = await this.client.rpc<RangeResponse>('KV', 'range', { key: this.key.clock }, operation);
    const value = response.kvs.at(0);
    if (value === undefined) throw new MissingClockError();
    return { value: decodeClock(value.value), revision: value.mod_revision };
  }

  async compareClock(revision: string, value: bigint, operation = this.operation()): Promise<boolean> {
    const response = await this.client.rpc<TxnResponse>('KV', 'txn', {
      compare: [{ key: this.key.clock, target: 'MOD', result: 'EQUAL', mod_revision: revision }],
      success: [{ request_put: { key: this.key.clock, value: encodeClock(value) } }]
    }, operation);
    return response.succeeded;
  }

  async init(wallMs = systemTime.wallMs()): Promise<boolean> {
    const response = await this.client.rpc<TxnResponse>('KV', 'txn', {
      compare: [{ key: this.key.namespacePrefix, range_end: prefixEnd(this.key.namespacePrefix), target: 'VERSION', result: 'EQUAL', version: '0' }],
      success: [{ request_put: { key: this.key.clock, value: encodeClock(wallMs) } }]
    }, this.operation());
    return response.succeeded;
  }

  async cleanup(signal?: AbortSignal): Promise<void> {
    if (stopped(signal)) return;
    const clock = await this.readClock(this.operation(signal));
    let start = this.key.metadataPrefix;
    const end = prefixEnd(start);
    while (!stopped(signal)) {
      const page = await this.client.rpc<RangeResponse>('KV', 'range', {
        key: start, range_end: end, limit: '1000', sort_order: 'ASCEND', sort_target: 'KEY'
      }, this.operation(signal));
      for (const record of page.kvs) {
        if (stopped(signal)) return;
        let metadata: Metadata;
        try { metadata = decodeMetadata(record.value); }
        catch (error) {
          if (!(error instanceof StoreFormatError)) throw error;
          process.stderr.write('corrupt metadata skipped\n');
          continue;
        }
        if (metadata.hasExpiry && metadata.expiresAtMs <= clock.value) {
          const frameworkKey = record.key.subarray(this.key.metadataPrefix.length);
          await this.client.rpc<TxnResponse>('KV', 'txn', {
            compare: [{ key: record.key, target: 'MOD', result: 'EQUAL', mod_revision: record.mod_revision }],
            success: [
              { request_delete_range: { key: record.key } },
              { request_delete_range: { key: this.key.payload(frameworkKey) } }
            ]
          }, this.operation(signal));
        }
      }
      if (!page.more || page.kvs.length === 0) return;
      start = Buffer.concat([page.kvs[page.kvs.length - 1].key, Buffer.from([0])]);
    }
  }
}
