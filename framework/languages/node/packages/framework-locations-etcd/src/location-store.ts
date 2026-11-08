import type {
  ZLinkLocationStore,
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
import { bounds, key, validateWrite, validateScan } from './internal/input';
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
    validateWrite(request);
    return this.perform(signal, async (operation) => {
      for (;;) {
        const response = await this.readTxn(
          [this.keys.clock, ...request.conditions.map((c) => this.keys.metadata(key(c.key)))],
          operation
        );
        const clock = this.clock(response);
        const records = response.responses.slice(1).map((r) => r.response_range.kvs.at(0));
        const metadata = records.map((r) => (r === undefined ? null : decodeMetadata(r.value)));
        const payloadIndexes = request.conditions.flatMap((condition, index) =>
          condition.kind === 'value' &&
          metadata[index]?.payloadLength === condition.expected.byteLength
            ? [index]
            : []
        );
        const payloads = new Map<number, KeyValue>();
        if (payloadIndexes.length > 0) {
          let read: ReadTxnResponse;
          try {
            read = await this.readTxn(
              payloadIndexes.map((i) => this.keys.payload(key(request.conditions[i].key))),
              operation,
              response.header.revision
            );
          } catch (error) {
            if (error instanceof EtcdCallError && error.kind === 'compaction') continue;
            throw error;
          }
          for (const [offset, index] of payloadIndexes.entries()) {
            const payload = read.responses[offset].response_range.kvs.at(0);
            validateRecord(
              records[index]!.value,
              payload?.value ?? null,
              records[index]!.mod_revision,
              payload?.mod_revision ?? ''
            );
            payloads.set(index, payload!);
          }
        }
        const matched = request.conditions.every((condition, index) => {
          const record = metadata[index];
          if (condition.kind === 'missing') return !live(record, clock);
          if (!live(record, clock)) return false;
          if (condition.kind === 'version')
            return (
              condition.expected.value ===
              encodeVersion(response.header.cluster_id, records[index]!.mod_revision)
            );
          return payloads.get(index)?.value.equals(Buffer.from(condition.expected)) === true;
        });
        if (!matched) return { kind: 'conflict', storeNow: date(clock) };
        const expiries = request.conditions.flatMap((condition, index) =>
          condition.kind !== 'missing' && metadata[index]?.hasExpiry === true
            ? [metadata[index]!.expiresAtMs]
            : []
        );
        const compare: object[] = [
          { key: this.keys.clock, target: 'CREATE', result: 'GREATER', create_revision: '0' },
          ...request.conditions.map((condition, index) =>
            same(this.keys.metadata(key(condition.key)), records[index])
          )
        ];
        if (expiries.length > 0)
          compare.push({
            key: this.keys.clock,
            target: 'VALUE',
            result: 'LESS',
            value: encodeClock(expiries.reduce((a, b) => (a < b ? a : b)))
          });
        const success: object[] = [];
        for (const mutation of request.mutations) {
          const logical = key(mutation.key);
          if (mutation.kind === 'delete') {
            success.push(
              { request_delete_range: { key: this.keys.metadata(logical) } },
              { request_delete_range: { key: this.keys.payload(logical) } }
            );
          } else {
            const expiresAtMs =
              mutation.retentionMs === undefined
                ? 0n
                : clock + BigInt(Math.ceil(mutation.retentionMs));
            if (expiresAtMs > MAX_TIME_MS)
              throw new RangeError('Store expiry exceeds its encoding range');
            success.push(
              {
                request_put: {
                  key: this.keys.metadata(logical),
                  value: encodeMetadata({
                    hasExpiry: mutation.retentionMs !== undefined,
                    expiresAtMs,
                    payloadLength: mutation.bytes.byteLength
                  })
                }
              },
              {
                request_put: { key: this.keys.payload(logical), value: Buffer.from(mutation.bytes) }
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
          storeNow: date(clock),
          putVersions: request.mutations
            .filter((m) => m.kind === 'put')
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
    const pending = action(this.client.operation(signal));
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
