import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts/Errors/ZLinkFrameworkException';
import {
  ZLINK_PROVIDER_MAX_PAGE_SIZE,
  ZLINK_PROVIDER_MAX_VALUE_BYTES,
  type ZLinkLocationStore,
  type ZLinkStoreCondition,
  type ZLinkStoreMutation,
  type ZLinkStoreScanCursor,
  type ZLinkStoreScanItem
} from '../../contracts/Locations/Stores';
import { macrotaskBoundary } from '../abort';
import type { ZLinkLocationOwnerToken } from './internal-location-contracts';
import { storeKey } from './in-memory-provider-location-store';
import { isIndeterminateLocationStoreFailure } from './location-store-failure';
import {
  RELOCATION_REPLY_PREFIX,
  decodeRelocationReplyRecord,
  encodeRelocationReplyRecord,
  relocationReplyDataLost,
  relocationReplyKey,
  relocationReplyPrefix,
  type RelocationReplyDelivery,
  type RelocationReplyIdentity,
  type RelocationReplyRecord
} from './relocation-reply-record';

const LIVE_RETENTION_MS = 24 * 60 * 60 * 1000;
const TERMINAL_RETENTION_MS = 5 * 60 * 1000;

export class RelocationReplyStore {
  constructor(
    private readonly provider: ZLinkLocationStore,
    private readonly writerCondition: (
      writer: ZLinkLocationOwnerToken
    ) => Extract<ZLinkStoreCondition, { kind: 'value' }>
  ) {}

  static accepted(
    identity: RelocationReplyIdentity,
    writer: ZLinkLocationOwnerToken
  ): Extract<ZLinkStoreMutation, { kind: 'put' }> {
    const row: RelocationReplyRecord = {
      ...identity,
      formatVersion: 1,
      writer: { ownerId: writer.ownerId, leaseGeneration: writer.leaseGeneration.toString() },
      state: 'accepted'
    };
    return {
      kind: 'put',
      key: storeKey(relocationReplyKey(row)),
      bytes: encodeRelocationReplyRecord(row),
      retentionMs: LIVE_RETENTION_MS
    };
  }

  async complete(
    identity: RelocationReplyIdentity,
    writer: ZLinkLocationOwnerToken,
    reply: string,
    failureReply: () => string,
    signal?: AbortSignal
  ): Promise<Extract<RelocationReplyRecord, { reply: string }> | undefined> {
    const row = await this.update(
      identity,
      writer,
      (current) => {
        if (current.state !== 'accepted') return;
        // Reserve the longest state spelling so every terminal CAS fits too.
        const largest = { ...current, state: 'sourceLeaseExpired', reply } as const;
        return {
          ...current,
          state: 'pending',
          reply:
            encodeRelocationReplyRecord(largest).length > ZLINK_PROVIDER_MAX_VALUE_BYTES
              ? failureReply()
              : reply
        };
      },
      signal
    );
    return row?.state === 'pending' ? row : undefined;
  }

  async acknowledge(
    identity: RelocationReplyIdentity,
    writer: ZLinkLocationOwnerToken,
    delivery: RelocationReplyDelivery,
    signal?: AbortSignal
  ): Promise<void> {
    await this.update(
      identity,
      writer,
      (row) => (row.state === 'pending' ? { ...row, state: delivery } : undefined),
      signal
    );
  }

  async retain(
    identity: RelocationReplyIdentity,
    writer: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<boolean> {
    const row = await this.update(
      identity,
      writer,
      (current, remainingMs) =>
        (current.state === 'accepted' || current.state === 'pending') &&
        remainingMs <= LIVE_RETENTION_MS / 2
          ? current
          : undefined,
      signal
    );
    return row?.state === 'accepted' || row?.state === 'pending';
  }

  async *scan(relocationId: string, signal?: AbortSignal): AsyncGenerator<RelocationReplyRecord> {
    for await (const item of this.snapshot(relocationReplyPrefix(relocationId), signal)) {
      yield decodeRelocationReplyRecord(item.value.bytes, item.key.value);
    }
  }

  acceptedRetentionAtHeartbeat(
    writer: ZLinkLocationOwnerToken,
    previousStoreNow: Date,
    storeNow: Date,
    signal?: AbortSignal
  ): (() => Promise<void>) | undefined {
    // Use the existing owner heartbeat. Two scans fit in the renewal window.
    const scanPeriodMs = LIVE_RETENTION_MS / 4;
    if (
      Math.floor(previousStoreNow.getTime() / scanPeriodMs) >=
      Math.floor(storeNow.getTime() / scanPeriodMs)
    )
      return;
    return () => this.retainAccepted(writer, signal);
  }

  private async retainAccepted(
    writer: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<void> {
    for await (const { key, value } of this.snapshot(RELOCATION_REPLY_PREFIX, signal)) {
      if ((value.expiresAt?.getTime() ?? 0) - value.storeNow.getTime() > LIVE_RETENTION_MS / 2)
        continue;
      const row = decodeRelocationReplyRecord(value.bytes, key.value);
      if (
        row.state === 'accepted' &&
        row.writer.ownerId === writer.ownerId &&
        row.writer.leaseGeneration === writer.leaseGeneration.toString()
      )
        await this.retain(row, writer, signal);
    }
  }

  private async *snapshot(
    prefix: string,
    signal?: AbortSignal
  ): AsyncGenerator<ZLinkStoreScanItem> {
    let cursor: ZLinkStoreScanCursor | undefined;
    do {
      const page = await this.provider.scan(
        { prefix, cursor, limit: ZLINK_PROVIDER_MAX_PAGE_SIZE },
        signal
      );
      if (page.kind === 'expired') throw new Error('Location Store relocation reply scan expired.');
      yield* page.value.items;
      cursor = page.value.nextCursor;
    } while (cursor !== undefined);
  }

  private async update(
    identity: RelocationReplyIdentity,
    writer: ZLinkLocationOwnerToken,
    next: (row: RelocationReplyRecord, remainingMs: number) => RelocationReplyRecord | undefined,
    signal?: AbortSignal
  ): Promise<RelocationReplyRecord | undefined> {
    const key = storeKey(relocationReplyKey(identity));
    const lease = this.writerCondition(writer);
    for (;;) {
      signal?.throwIfAborted();
      try {
        const read = await this.provider.read(key, signal);
        if (read.kind === 'missing') return;
        const row = decodeRelocationReplyRecord(read.value.bytes, key.value);
        if (row.objectKey !== identity.objectKey || row.replyRouteId !== identity.replyRouteId)
          return relocationReplyDataLost('Relocation reply identity changed.');
        if (
          row.writer.ownerId !== writer.ownerId ||
          row.writer.leaseGeneration !== writer.leaseGeneration.toString()
        )
          return;
        const leaseRead = await this.provider.read(lease.key, signal);
        if (
          leaseRead.kind === 'missing' ||
          !Buffer.from(leaseRead.value.bytes).equals(lease.expected)
        )
          return;
        const remainingMs = (read.value.expiresAt?.getTime() ?? 0) - read.value.storeNow.getTime();
        const replacement = next(row, remainingMs);
        if (replacement === undefined) return row;
        const retentionMs =
          replacement.state === 'accepted' || replacement.state === 'pending'
            ? LIVE_RETENTION_MS
            : TERMINAL_RETENTION_MS;
        const result = await this.provider.write(
          {
            conditions: [{ kind: 'version', key, expected: read.value.version }, lease],
            mutations: [
              { kind: 'put', key, bytes: encodeRelocationReplyRecord(replacement), retentionMs }
            ]
          },
          signal
        );
        if (result.kind === 'applied') return replacement;
        // Conflict and a lost write response both resolve by reading the same row.
      } catch (error) {
        if (
          error instanceof ZLinkFrameworkException &&
          error.kind !== ZLinkFrameworkErrorKind.Unavailable
        )
          throw error;
        if (!isIndeterminateLocationStoreFailure(error, signal)) throw error;
      }
      await macrotaskBoundary();
    }
  }
}
