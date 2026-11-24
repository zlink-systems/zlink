import type {
  ZLinkLocationStore,
  ZLinkStoreCondition,
  ZLinkStoreKey,
  ZLinkStoreReadResult,
  ZLinkStoreValue,
  ZLinkStoreWriteRequest,
  ZLinkStoreWriteResult,
  ZLinkStoreScanRequest,
  ZLinkStoreScanResult,
  ZLinkStoreScanItem,
  ZLinkStoreVersion,
  ZLinkStoreScanCursor
} from '@zlink-systems/framework';
import type { ZLinkEtcdLocationOptions } from './options';
import {
  EtcdClient,
  EtcdCallError,
  Operation,
  KeyValue,
  RangeResponse,
  ReadTxnResponse,
  TxnResponse
} from './internal/client';
import {
  keys,
  decodeClock,
  decodeMetadata,
  encodeClock,
  encodeMetadata,
  encodeVersion,
  validateRecord,
  prefixEnd,
  Metadata,
  MAX_TIME_MS,
  StoreFormatError
} from './internal/encoding';
import { bounds, key, validateScan } from './internal/input';
import { monotonicMs, ScanCursor, Snapshot } from './internal/cursor';

const range = (key: Buffer, revision?: string) => ({
  request_range: { key, revision, serializable: false }
});
const missing = (key: Buffer) => ({ key, target: 'CREATE', result: 'EQUAL', create_revision: '0' });
const same = (key: Buffer, record: KeyValue | undefined) =>
  record === undefined
    ? missing(key)
    : { key, target: 'MOD', result: 'EQUAL', mod_revision: record.mod_revision };
const live = (metadata: Metadata | null, clock: bigint) =>
  metadata !== null && (!metadata.hasExpiry || metadata.expiresAtMs > clock);
const date = (value: bigint) => new Date(Number(value));

interface WriteSubject {
  readonly logical: Buffer;
  readonly condition?: ZLinkStoreCondition;
  counter: boolean;
}

function writeSubjects(request: ZLinkStoreWriteRequest): Map<string, WriteSubject> {
  const subjects = new Map<string, WriteSubject>();
  function addCounter(key: ZLinkStoreKey): void {
    const subject = subjects.get(key.value);
    if (subject === undefined)
      subjects.set(key.value, { logical: Buffer.from(key.value), counter: true });
    else subject.counter = true;
  }
  for (const condition of request.conditions) {
    if (condition.kind === 'sumAtMost') {
      for (const key of condition.keys) addCounter(key);
    } else {
      subjects.set(condition.key.value, {
        logical: Buffer.from(condition.key.value),
        condition,
        counter: false
      });
    }
  }
  for (const mutation of request.mutations) {
    if (mutation.kind === 'add') addCounter(mutation.key);
  }
  return subjects;
}

/** Opaque Location Store SPI backed by etcd linearizable transactions. */
export class ZLinkEtcdLocationStore implements ZLinkLocationStore {
  private readonly client: EtcdClient;
  private readonly keys;
  private readonly cursors: ScanCursor;
  private readonly pending = new Set<Promise<unknown>>();
  private disposal: Promise<void> | undefined;

  constructor(options: ZLinkEtcdLocationOptions) {
    this.client = new EtcdClient(options);
    this.keys = keys(options.namespace);
    this.cursors = new ScanCursor(options.namespace);
  }

  async read(subject: ZLinkStoreKey, signal?: AbortSignal): Promise<ZLinkStoreReadResult> {
    const logical = key(subject);
    return this.perform(signal, async (operation) => {
      const response = await this.readTxn(
        [this.keys.clock, this.keys.metadata(logical), this.keys.payload(logical)],
        operation
      );
      const clock = this.clock(response);
      const metadata = response.responses[1].response_range.kvs.at(0);
      const payload = response.responses[2].response_range.kvs.at(0);
      const decoded = validateRecord(
        metadata?.value ?? null,
        payload?.value ?? null,
        metadata?.mod_revision ?? '',
        payload?.mod_revision ?? ''
      );
      if (!live(decoded, clock)) return { kind: 'missing', storeNow: date(clock) };
      return {
        kind: 'found',
        value: this.value(metadata!, payload!, decoded!, clock, response.header.cluster_id)
      };
    });
  }

  async write(
    request: ZLinkStoreWriteRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreWriteResult> {
    bounds.validateStoreWriteRequest(request, Number(MAX_TIME_MS));
    const subjects = writeSubjects(request);
    const observedKeys = [...subjects.values()].map((subject) =>
      this.keys.metadata(subject.logical)
    );
    return this.perform(signal, async (operation) => {
      for (;;) {
        const response = await this.readTxn([this.keys.clock, ...observedKeys], operation);
        const clock = this.clock(response);
        const storeNow = date(clock);
        const observations = new Map(
          [...subjects.entries()].map(([logical, subject], index) => {
            const record = response.responses[index + 1].response_range.kvs.at(0);
            const metadata = record === undefined ? null : decodeMetadata(record.value);
            return [
              logical,
              { subject, record, metadata, payload: undefined as Buffer | undefined }
            ];
          })
        );
        const payloadReads = [...observations.values()].filter(({ subject, metadata }) => {
          if (metadata === null) return false;
          const condition = subject.condition;
          return (
            (subject.counter &&
              live(metadata, clock) &&
              metadata.payloadLength > 0 &&
              metadata.payloadLength <= bounds.STORE_COUNTER_MAX_BYTES) ||
            (condition?.kind === 'value' &&
              metadata.payloadLength === condition.expected.byteLength)
          );
        });
        if (payloadReads.length > 0) {
          let read: ReadTxnResponse;
          try {
            read = await this.readTxn(
              payloadReads.map(({ subject }) => this.keys.payload(subject.logical)),
              operation,
              response.header.revision
            );
          } catch (error) {
            if (error instanceof EtcdCallError && error.kind === 'compaction') continue;
            throw error;
          }
          for (const [offset, observation] of payloadReads.entries()) {
            const payload = read.responses[offset].response_range.kvs.at(0);
            validateRecord(
              observation.record!.value,
              payload?.value ?? null,
              observation.record!.mod_revision,
              payload?.mod_revision ?? ''
            );
            observation.payload = payload!.value;
          }
        }

        const counter = (key: string) => {
          const { metadata, payload } = observations.get(key)!;
          if (!live(metadata, clock)) return 0n;
          return payload === undefined ? undefined : bounds.readStoreCounter(payload);
        };
        for (const condition of request.conditions) {
          if (condition.kind === 'sumAtMost') {
            const matched = bounds.sumStoreCounters(condition, counter);
            if (matched === undefined) return { kind: 'corrupt', storeNow, putVersions: [] };
            if (!matched) return { kind: 'conflict', storeNow };
            continue;
          }
          const { record, metadata, payload } = observations.get(condition.key.value)!;
          const present = live(metadata, clock);
          let matches: boolean;
          if (condition.kind === 'missing') matches = !present;
          else if (condition.kind === 'version') {
            matches =
              present &&
              condition.expected.value ===
                encodeVersion(response.header.cluster_id, record!.mod_revision);
          } else {
            matches = present && payload?.equals(Buffer.from(condition.expected)) === true;
          }
          if (!matches) return { kind: 'conflict', storeNow };
        }

        const expiries = [];
        const compare: object[] = [
          { key: this.keys.clock, target: 'CREATE', result: 'GREATER', create_revision: '0' }
        ];
        for (const { subject, record, metadata } of observations.values()) {
          compare.push(same(this.keys.metadata(subject.logical), record));
          if (metadata?.hasExpiry === true && live(metadata, clock))
            expiries.push(metadata.expiresAtMs);
        }
        if (expiries.length > 0)
          compare.push({
            key: this.keys.clock,
            target: 'VALUE',
            result: 'LESS',
            value: encodeClock(expiries.reduce((a, b) => (a < b ? a : b)))
          });
        const success: object[] = [];
        for (const mutation of request.mutations) {
          const logical = Buffer.from(mutation.key.value);
          if (mutation.kind === 'delete') {
            success.push(
              { request_delete_range: { key: this.keys.metadata(logical) } },
              { request_delete_range: { key: this.keys.payload(logical) } }
            );
          } else {
            const bytes =
              mutation.kind === 'add'
                ? bounds.addStoreCounter(counter(mutation.key.value), mutation.delta)
                : Buffer.from(mutation.bytes);
            if (bytes === undefined) return { kind: 'corrupt', storeNow, putVersions: [] };
            const retention = mutation.kind === 'put' ? mutation.retentionMs : undefined;
            const expiresAtMs = retention === undefined ? 0n : clock + BigInt(Math.ceil(retention));
            if (expiresAtMs > MAX_TIME_MS)
              throw new RangeError('Store expiry exceeds its encoding range');
            success.push(
              {
                request_put: {
                  key: this.keys.metadata(logical),
                  value: encodeMetadata({
                    hasExpiry: retention !== undefined,
                    expiresAtMs,
                    payloadLength: bytes.byteLength
                  })
                }
              },
              {
                request_put: { key: this.keys.payload(logical), value: bytes }
              }
            );
          }
        }
        const applied = await this.client.rpc<TxnResponse>(
          'KV',
          'txn',
          { compare, success },
          operation
        );
        if (!applied.succeeded) continue;
        return {
          kind: 'applied',
          storeNow,
          putVersions: request.mutations
            .filter((m) => m.kind !== 'delete')
            .map((m) => ({
              key: { value: m.key.value } as ZLinkStoreKey,
              version: {
                value: encodeVersion(applied.header.cluster_id, applied.header.revision)
              } as ZLinkStoreVersion
            }))
        };
      }
    });
  }

  async scan(request: ZLinkStoreScanRequest, signal?: AbortSignal): Promise<ZLinkStoreScanResult> {
    validateScan(request);
    return this.perform(signal, async (operation) => {
      let snapshot: Snapshot | undefined;
      if (request.cursor !== undefined) {
        snapshot = this.cursors.decode(request.cursor.value, request.prefix);
        if (snapshot === undefined) return { kind: 'expired' };
      }
      const started = monotonicMs();
      const observed = await this.readTxn([this.keys.clock], operation);
      const clock = this.clock(observed);
      snapshot ??= {
        cluster: observed.header.cluster_id,
        revision: observed.header.revision,
        clock,
        started,
        last: Buffer.alloc(0)
      };
      if (snapshot.cluster !== observed.header.cluster_id) return { kind: 'expired' };
      try {
        return await this.scanPage(request, snapshot, operation);
      } catch (error) {
        if (error instanceof EtcdCallError && error.kind === 'compaction')
          return { kind: 'expired' };
        throw error;
      }
    });
  }

  dispose(): Promise<void> {
    this.disposal ??= Promise.allSettled([...this.pending]).then(() => this.client.close());
    return this.disposal;
  }

  private async perform<T>(
    signal: AbortSignal | undefined,
    action: (operation: Operation) => Promise<T>
  ): Promise<T> {
    if (this.disposal !== undefined) throw new Error('Location Store is disposed');
    if (signal?.aborted === true) throw new Error('Store operation stopped');
    const pending = action(this.client.operation(signal, true));
    this.pending.add(pending);
    try {
      return await pending;
    } catch (error) {
      if (error instanceof EtcdCallError) throw new Error('Location Store operation failed');
      throw error;
    } finally {
      this.pending.delete(pending);
    }
  }

  private readTxn(
    subjects: Buffer[],
    operation: Operation,
    revision?: string
  ): Promise<ReadTxnResponse> {
    return this.client.rpc(
      'KV',
      'txn',
      { success: subjects.map((subject) => range(subject, revision)) },
      operation
    );
  }

  private clock(response: ReadTxnResponse): bigint {
    const clock = response.responses[0].response_range.kvs.at(0);
    if (clock === undefined) throw new StoreFormatError();
    return decodeClock(clock.value);
  }

  private value(
    metadata: KeyValue,
    payload: KeyValue,
    decoded: Metadata,
    clock: bigint,
    cluster: string
  ): ZLinkStoreValue {
    return {
      bytes: Uint8Array.from(payload.value),
      version: { value: encodeVersion(cluster, metadata.mod_revision) } as ZLinkStoreVersion,
      expiresAt: decoded.hasExpiry ? date(decoded.expiresAtMs) : undefined,
      storeNow: date(clock)
    };
  }

  private async scanPage(
    request: ZLinkStoreScanRequest,
    snapshot: Snapshot,
    operation: Operation
  ): Promise<ZLinkStoreScanResult> {
    const prefix = Buffer.concat([this.keys.metadataPrefix, Buffer.from(request.prefix)]);
    const end = prefixEnd(prefix);
    let start =
      snapshot.last.length === 0
        ? prefix
        : Buffer.concat([this.keys.metadata(snapshot.last), Buffer.from([0])]);
    const selected: { record: KeyValue; decoded: Metadata; logical: Buffer }[] = [];
    let size = 0;
    let more = true;
    let pageFull = false;
    while (more && !pageFull && selected.length < request.limit) {
      if (!this.cursors.valid(snapshot)) return { kind: 'expired' };
      const response = await this.client.rpc<RangeResponse>(
        'KV',
        'range',
        {
          key: start,
          range_end: end,
          limit: '1000',
          revision: snapshot.revision,
          sort_order: 'ASCEND',
          sort_target: 'KEY',
          serializable: false
        },
        operation
      );
      more = response.more;
      for (const record of response.kvs) {
        const decoded = decodeMetadata(record.value);
        const logical = record.key.subarray(this.keys.metadataPrefix.length);
        if (live(decoded, snapshot.clock)) {
          const itemSize =
            logical.length +
            Buffer.byteLength(encodeVersion(snapshot.cluster, record.mod_revision)) +
            decoded.payloadLength;
          if (
            selected.length === request.limit ||
            size + itemSize > bounds.ZLINK_PROVIDER_MAX_ENCODED_PAGE_BYTES
          ) {
            more = true;
            pageFull = true;
            break;
          }
          size += itemSize;
          selected.push({ record, decoded, logical });
        }
        snapshot.last = logical;
      }
      start = Buffer.concat([this.keys.metadata(snapshot.last), Buffer.from([0])]);
    }
    const items: ZLinkStoreScanItem[] = [];
    if (selected.length > 0) {
      const payloads = await this.readTxn(
        selected.map((item) => this.keys.payload(item.logical)),
        operation,
        snapshot.revision
      );
      for (const [index, item] of selected.entries()) {
        const payload = payloads.responses[index].response_range.kvs.at(0);
        validateRecord(
          item.record.value,
          payload?.value ?? null,
          item.record.mod_revision,
          payload?.mod_revision ?? ''
        );
        items.push({
          key: { value: item.logical.toString('utf8') } as ZLinkStoreKey,
          value: this.value(item.record, payload!, item.decoded, snapshot.clock, snapshot.cluster)
        });
      }
    }
    if (!this.cursors.valid(snapshot)) return { kind: 'expired' };
    return {
      kind: 'page',
      value: {
        items,
        storeNow: date(snapshot.clock),
        nextCursor: more
          ? ({ value: this.cursors.encode(snapshot, request.prefix) } as ZLinkStoreScanCursor)
          : undefined
      }
    };
  }
}
