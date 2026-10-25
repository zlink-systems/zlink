import type { TableSnapshot } from '../domain/table-state';

const tableSnapshotBigIntFields = new Set(['chips', 'handNumber', 'pot']);

export function serializeTableSnapshot(snapshot: TableSnapshot): Uint8Array {
  return Buffer.from(
    JSON.stringify(snapshot, (_key, value: unknown) =>
      typeof value === 'bigint' ? value.toString(10) : value
    )
  );
}

export function deserializeTableSnapshot(payload: Uint8Array): TableSnapshot {
  return JSON.parse(Buffer.from(payload).toString('utf8'), (key, value: unknown) =>
    tableSnapshotBigIntFields.has(key) && typeof value === 'string' ? BigInt(value) : value
  ) as TableSnapshot;
}
