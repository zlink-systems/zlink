import { createHash } from 'node:crypto';
import { ZLINK_PROVIDER_MAX_PAGE_SIZE } from '../../contracts/Locations/Stores';
import type { ZLinkStoreCondition, ZLinkStoreScanCursor } from '../../contracts';
import type { ZLinkLocationStore, ZLinkStoreKey } from '../../contracts';
import { decodeAuthorityKey } from './authority-key-codec';
import { PROVIDER_STORAGE_NAMESPACE_PREFIX, storeKey } from './in-memory-provider-location-store';
import type {
  ZLinkAggregateFence,
  ZLinkAggregateParticipant,
  ZLinkAuthorityKey
} from './internal-location-contracts';

const MAX_PAGE_ENTRIES = 1_024;
const MAX_PAGE_BYTES = 1_024 * 1_024;
const MAX_MEMBERSHIP_MUTATION_BYTES = 1_024 * 1_024;
const MAX_COUNT = 0x7fff_ffff;
const IO_CONCURRENCY = 64;

interface InventoryEntry {
  readonly index: number;
  readonly authorityKey: string;
  readonly expectedStoreVersion: string;
  readonly ownerTransition: 'preserve' | 'newOwner';
  readonly authorityPayloadSha256: string;
  readonly membershipMutationSha256: string;
}

export interface StoredAggregateInventoryEntry extends InventoryEntry {
  readonly authorityPayload: Uint8Array;
  readonly membershipMutation: Uint8Array;
}

interface InventoryPosition {
  readonly level: number;
  readonly index: number;
  readonly startIndex: number;
  readonly entryCount: number;
}

interface InventoryReference extends InventoryPosition {
  readonly sha256: string;
}

interface InventoryPage extends InventoryPosition {
  readonly entries: readonly InventoryEntry[];
  readonly children: readonly InventoryReference[];
}

interface InventoryRootBody {
  readonly totalCount: number;
  readonly topLevel: number;
  readonly topPages: readonly InventoryReference[];
}

interface InventoryRoot extends InventoryRootBody {
  readonly digestSha256: string;
}

type InventoryRequest = ZLinkAggregateFence & {
  readonly participants: readonly ZLinkAggregateParticipant[];
};

export class ZLinkAggregateInventoryError extends Error {}

/** Owns §8.3 inventory records above the opaque Location Store provider. */
export class ZLinkAggregateInventoryStore {
  constructor(private readonly provider: ZLinkLocationStore) {}

  async store(request: InventoryRequest, signal?: AbortSignal): Promise<string> {
    const inventory = this.plan(request);
    await inventory.write(signal);
    return inventory.digest;
  }

  plan(request: InventoryRequest) {
    validateAggregateFence(request);
    const participants = request.participants.map((participant, index) => {
      const authorityPayload = Buffer.from(participant.authorityPayload);
      const membershipMutation = Buffer.from(participant.membershipMutation);
      if (!validMembershipMutationLength(membershipMutation)) {
        throw new RangeError(
          'Aggregate inventory participant membership mutation length is invalid.'
        );
      }
      return {
        authorityPayload,
        membershipMutation,
        entry: {
          index,
          authorityKey: participant.authorityKey.value,
          expectedStoreVersion: participant.expectedStoreVersion.value,
          ownerTransition: participant.ownerTransition,
          authorityPayloadSha256: sha256(authorityPayload),
          membershipMutationSha256: sha256(membershipMutation)
        }
      };
    });
    const entries = participants.map(({ entry }) => entry);
    const entryError = invalidEntry(entries);
    if (entryError !== undefined) throw new TypeError(`Aggregate inventory ${entryError}.`);
    const tree = buildTree(entries);
    return {
      digest: tree.root.digestSha256,
      entries,
      write: async (writeSignal?: AbortSignal) => {
        await parallelForEach(
          participants,
          async ({ entry, authorityPayload, membershipMutation }) => {
            await this.putImmutable(
              request,
              participantKey(request, entry.index, 'authorityPayload'),
              authorityPayload,
              writeSignal
            );
            await this.putImmutable(
              request,
              participantKey(request, entry.index, 'membershipMutation'),
              membershipMutation,
              writeSignal
            );
          }
        );
        await parallelForEach(tree.pages, async ({ position, bytes }) => {
          await this.putImmutable(request, pageKey(request, position), bytes, writeSignal);
        });
        await this.putImmutable(
          request,
          rootKey(request),
          encodeAggregateJson(tree.root),
          writeSignal
        );
      }
    };
  }

  async hasRoot(fence: ZLinkAggregateFence, signal?: AbortSignal): Promise<boolean> {
    return (await this.provider.read(rootKey(fence), signal)).kind === 'found';
  }

  async remove(
    fence: ZLinkAggregateFence,
    aggregateCondition: ZLinkStoreCondition,
    terminal: boolean,
    signal?: AbortSignal
  ): Promise<boolean> {
    const root = rootKey(fence);
    if (terminal && (await this.provider.read(root, signal)).kind === 'missing') return true;
    let cursor: ZLinkStoreScanCursor | undefined;
    for (;;) {
      const page = await this.provider.scan(
        { prefix: aggregatePrefix(fence), cursor, limit: ZLINK_PROVIDER_MAX_PAGE_SIZE },
        signal
      );
      if (page.kind === 'expired') {
        cursor = undefined;
        continue;
      }
      const mutations = page.value.items
        .filter(({ key }) => key.value !== root.value)
        .map(({ key }) => ({ kind: 'delete' as const, key }));
      if (
        mutations.length !== 0 &&
        (await this.provider.write({ conditions: [aggregateCondition], mutations }, signal))
          .kind === 'conflict'
      )
        return false;
      cursor = page.value.nextCursor;
      if (cursor === undefined) break;
    }
    return (
      (
        await this.provider.write(
          {
            conditions: [aggregateCondition],
            mutations: [{ kind: 'delete', key: root }]
          },
          signal
        )
      ).kind === 'applied'
    );
  }

  async read(
    fence: ZLinkAggregateFence,
    expectedDigest?: Uint8Array,
    signal?: AbortSignal
  ): Promise<readonly StoredAggregateInventoryEntry[]> {
    validateAggregateFence(fence);
    const rootBytes = await this.requireFound(fence, rootKey(fence), signal);
    if (rootBytes.byteLength > MAX_PAGE_BYTES) throw dataLost(fence, 'root size is invalid');
    const root = decode<InventoryRoot>(rootBytes, fence);
    if (
      !hasFields(root, 4) ||
      !isCount(root.totalCount) ||
      !isCount(root.topLevel) ||
      root.topLevel >= 32 ||
      !Array.isArray(root.topPages) ||
      root.topPages.length !== (root.totalCount === 0 ? 0 : 1) ||
      !isSha256(root.digestSha256)
    ) {
      throw dataLost(fence, 'root fields are invalid');
    }
    if (
      expectedDigest !== undefined &&
      root.digestSha256 !== Buffer.from(expectedDigest).toString('hex')
    ) {
      throw dataLost(fence, 'root checksum differs from the sealed inventory');
    }

    const entries: InventoryEntry[] = [];
    const nextPageIndex = Array.from({ length: root.topLevel + 1 }, () => 0);
    const readPage = async (reference: InventoryReference, level: number): Promise<void> => {
      if (
        !hasFields(reference, 5) ||
        reference.level !== level ||
        reference.index !== nextPageIndex[level] ||
        reference.startIndex !== entries.length ||
        !isCount(reference.entryCount) ||
        reference.entryCount < 1 ||
        reference.entryCount > root.totalCount - entries.length ||
        !isSha256(reference.sha256)
      ) {
        throw dataLost(fence, 'page reference is invalid or out of order');
      }
      nextPageIndex[level] = reference.index + 1;
      const bytes = await this.requireFound(fence, pageKey(fence, reference), signal);
      if (bytes.byteLength > MAX_PAGE_BYTES || sha256(bytes) !== reference.sha256) {
        throw dataLost(fence, 'page checksum or size is invalid');
      }
      const page = decode<InventoryPage>(bytes, fence);
      if (
        !hasFields(page, 6) ||
        page.level !== reference.level ||
        page.index !== reference.index ||
        page.startIndex !== reference.startIndex ||
        page.entryCount !== reference.entryCount ||
        !Array.isArray(page.entries) ||
        !Array.isArray(page.children) ||
        page.entries.length > MAX_PAGE_ENTRIES ||
        page.children.length > MAX_PAGE_ENTRIES
      ) {
        throw dataLost(fence, 'page fields are invalid');
      }
      if (page.level === 0) {
        if (page.entries.length !== page.entryCount || page.children.length !== 0) {
          throw dataLost(fence, 'leaf page bounds are invalid');
        }
        entries.push(...page.entries);
      } else {
        if (page.entries.length !== 0 || page.children.length === 0) {
          throw dataLost(fence, 'index page bounds are invalid');
        }
        for (const child of page.children) await readPage(child, level - 1);
        if (entries.length !== page.startIndex + page.entryCount) {
          throw dataLost(fence, 'index page entry count differs from its children');
        }
      }
    };
    for (const reference of root.topPages) await readPage(reference, root.topLevel);
    const entryError = invalidEntry(entries);
    if (entryError !== undefined) throw dataLost(fence, entryError);
    if (!encodeAggregateJson(buildTree(entries).root).equals(Buffer.from(rootBytes))) {
      throw dataLost(fence, 'root checksum or canonical page partition is invalid');
    }
    const participants: StoredAggregateInventoryEntry[] = [];
    await parallelForEach(entries, async (entry) => {
      const [authorityPayload, membershipMutation] = await Promise.all([
        this.readAuthorityPayload(fence, entry.index, entry.authorityPayloadSha256, signal),
        this.readParticipantBytes(
          fence,
          entry.index,
          'membershipMutation',
          entry.membershipMutationSha256,
          signal
        )
      ]);
      if (!validMembershipMutationLength(membershipMutation)) {
        throw dataLost(fence, 'participant membership mutation length is invalid');
      }
      participants[entry.index] = { ...entry, authorityPayload, membershipMutation };
    });
    return participants;
  }

  readAuthorityPayload(
    fence: ZLinkAggregateFence,
    index: number,
    hash: string,
    signal?: AbortSignal
  ) {
    return this.readParticipantBytes(fence, index, 'authorityPayload', hash, signal);
  }

  private async readParticipantBytes(
    fence: ZLinkAggregateFence,
    index: number,
    kind: 'authorityPayload' | 'membershipMutation',
    hash: string,
    signal?: AbortSignal
  ): Promise<Uint8Array> {
    const bytes = await this.requireFound(fence, participantKey(fence, index, kind), signal);
    if (sha256(bytes) !== hash) throw dataLost(fence, 'participant checksum is invalid');
    return bytes;
  }

  private async putImmutable(
    fence: ZLinkAggregateFence,
    key: ZLinkStoreKey,
    bytes: Uint8Array,
    signal?: AbortSignal
  ): Promise<void> {
    const result = await this.provider.write(
      { conditions: [{ kind: 'missing', key }], mutations: [{ kind: 'put', key, bytes }] },
      signal
    );
    if (result.kind === 'applied') return;
    const current = await this.requireFound(fence, key, signal);
    if (!Buffer.from(current).equals(bytes)) {
      throw dataLost(fence, `immutable value changed at '${key.value}'`);
    }
  }

  private async requireFound(
    fence: ZLinkAggregateFence,
    key: ZLinkStoreKey,
    signal?: AbortSignal
  ): Promise<Uint8Array> {
    const result = await this.provider.read(key, signal);
    if (result.kind !== 'found') throw dataLost(fence, `value is missing at '${key.value}'`);
    return result.value.bytes;
  }
}

// §8.3 applies the maintenanceAggregateBytes bound to the stored body.
function validMembershipMutationLength(bytes: Uint8Array): boolean {
  return bytes.byteLength >= 1 && bytes.byteLength <= MAX_MEMBERSHIP_MUTATION_BYTES;
}

function buildTree(entries: readonly InventoryEntry[]) {
  const pages: { position: InventoryPosition; bytes: Buffer }[] = [];
  let topLevel = 0;
  let topPages = pack(
    0,
    entries,
    (entry) => entry.index,
    (last) => last.index + 1
  );
  while (topPages.length > 1) {
    topLevel++;
    topPages = pack(
      topLevel,
      topPages,
      (child) => child.startIndex,
      (last) => last.startIndex + last.entryCount
    );
  }
  const body: InventoryRootBody = { totalCount: entries.length, topLevel, topPages };
  return { root: { ...body, digestSha256: sha256(encodeAggregateJson(body)) }, pages };

  function pack<T extends InventoryEntry | InventoryReference>(
    level: number,
    values: readonly T[],
    start: (value: T) => number,
    end: (value: T) => number
  ): InventoryReference[] {
    const references: InventoryReference[] = [];
    const serialized = values.map(encodeAggregateJson);
    for (let offset = 0; offset < values.length;) {
      const index = references.length;
      const startIndex = start(values[offset]!);
      const prefix = level === 0 ? '{"children":[],"entries":[' : '{"children":[';
      let length = Buffer.byteLength(prefix);
      let count = 0;
      let suffix = '';
      let entryCount = 0;
      while (offset + count < values.length && count < MAX_PAGE_ENTRIES) {
        const nextEntryCount = end(values[offset + count]!) - startIndex;
        const nextSuffix =
          (level === 0 ? ']' : '],"entries":[]') +
          `,"entryCount":${nextEntryCount},"index":${index},"level":${level},"startIndex":${startIndex}}`;
        const nextLength = length + Number(count > 0) + serialized[offset + count]!.byteLength;
        if (nextLength + Buffer.byteLength(nextSuffix) > MAX_PAGE_BYTES) break;
        length = nextLength;
        entryCount = nextEntryCount;
        suffix = nextSuffix;
        count++;
      }
      if (count === 0) throw new RangeError('Aggregate inventory page item exceeds 1 MiB.');
      const bytes = Buffer.concat([
        Buffer.from(prefix),
        ...serialized
          .slice(offset, offset + count)
          .flatMap((value, index) => (index === 0 ? [value] : [JSON_COMMA, value])),
        Buffer.from(suffix)
      ]);
      const position = { level, index, startIndex, entryCount };
      pages.push({ position, bytes });
      references.push({ ...position, sha256: sha256(bytes) });
      offset += count;
    }
    return references;
  }
}

const JSON_COMMA = Buffer.from(',');

export function compareInventoryAuthorityKeys(left: string, right: string): number {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

function invalidEntry(entries: readonly InventoryEntry[]): string | undefined {
  for (let index = 0; index < entries.length; index++) {
    const entry = entries[index]!;
    if (
      !hasFields(entry, 6) ||
      entry.index !== index ||
      !isAuthorityKey(entry.authorityKey) ||
      !isText(entry.expectedStoreVersion, 4_096) ||
      !isOwnerTransition(entry.ownerTransition) ||
      !isSha256(entry.authorityPayloadSha256) ||
      !isSha256(entry.membershipMutationSha256)
    ) {
      return 'entry is invalid or out of order';
    }
    if (
      index > 0 &&
      compareInventoryAuthorityKeys(entries[index - 1]!.authorityKey, entry.authorityKey) >= 0
    ) {
      return 'authority keys are duplicated or out of order';
    }
  }
  return undefined;
}

function hasFields(value: unknown, size: number): boolean {
  return (
    value !== null &&
    typeof value === 'object' &&
    !Array.isArray(value) &&
    Object.keys(value).length === size
  );
}

function isAuthorityKey(value: unknown): boolean {
  if (typeof value !== 'string') return false;
  try {
    decodeAuthorityKey({ value } as ZLinkAuthorityKey);
    return true;
  } catch (error) {
    if (!(error instanceof TypeError)) throw error;
    return false;
  }
}

function isText(value: unknown, maxBytes: number): value is string {
  return (
    typeof value === 'string' &&
    value.length > 0 &&
    Buffer.byteLength(value) <= maxBytes &&
    !/[\uD800-\uDFFF]/u.test(value)
  );
}

function isOwnerTransition(value: unknown): boolean {
  return value === 'preserve' || value === 'newOwner';
}

function isCount(value: number): boolean {
  return Number.isInteger(value) && value >= 0 && value <= MAX_COUNT;
}

export function encodeAggregateJson(value: unknown): Buffer {
  return Buffer.from(
    JSON.stringify(value, (_, item: unknown) => {
      if (item === null || typeof item !== 'object' || Array.isArray(item)) return item;
      const record = item as Record<string, unknown>;
      return Object.fromEntries(
        Object.keys(record)
          .sort()
          .map((key) => [key, record[key]])
      );
    })
  );
}

function decode<T>(bytes: Uint8Array, fence: ZLinkAggregateFence): T {
  let value: unknown;
  try {
    value = JSON.parse(Buffer.from(bytes).toString('utf8'));
  } catch (error) {
    if (!(error instanceof SyntaxError)) throw error;
    throw dataLost(fence, 'JSON is invalid');
  }
  return value as T;
}

export function aggregateKey(fence: ZLinkAggregateFence): ZLinkStoreKey {
  validateAggregateFence(fence);
  return storeKey(
    `${PROVIDER_STORAGE_NAMESPACE_PREFIX}aggregate:${fence.aggregateId.value}:${fence.aggregateGeneration}`
  );
}

function rootKey(fence: ZLinkAggregateFence): ZLinkStoreKey {
  return storeKey(`${aggregatePrefix(fence)}root`);
}

function pageKey(fence: ZLinkAggregateFence, { level, index }: InventoryPosition): ZLinkStoreKey {
  return storeKey(`${aggregatePrefix(fence)}page:${level}:${index}`);
}

function participantKey(
  fence: ZLinkAggregateFence,
  index: number,
  kind: 'authorityPayload' | 'membershipMutation'
): ZLinkStoreKey {
  return storeKey(`${aggregatePrefix(fence)}participant:${index}:${kind}`);
}

export function validateAggregateFence(fence: ZLinkAggregateFence): void {
  if (
    !/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(
      fence.aggregateId.value
    ) ||
    fence.aggregateGeneration < 1n ||
    fence.aggregateGeneration > 0xffff_ffff_ffff_ffffn
  ) {
    throw new TypeError('Aggregate inventory identity is invalid.');
  }
}

function aggregatePrefix(fence: ZLinkAggregateFence): string {
  return `${PROVIDER_STORAGE_NAMESPACE_PREFIX}aggregate-inventory:${fence.aggregateId.value}:${fence.aggregateGeneration}:`;
}

function sha256(bytes: Uint8Array): string {
  return createHash('sha256').update(bytes).digest('hex');
}

function isSha256(value: unknown): value is string {
  return typeof value === 'string' && /^[0-9a-f]{64}$/.test(value);
}

function dataLost(fence: ZLinkAggregateFence, reason: string): ZLinkAggregateInventoryError {
  return new ZLinkAggregateInventoryError(
    `Aggregate '${fence.aggregateId.value}:${fence.aggregateGeneration}' inventory ${reason}.`
  );
}

async function parallelForEach<T>(
  values: readonly T[],
  operation: (value: T) => Promise<void>
): Promise<void> {
  let next = 0;
  const results = await Promise.allSettled(
    Array.from({ length: Math.min(IO_CONCURRENCY, values.length) }, async () => {
      while (next < values.length) await operation(values[next++]!);
    })
  );
  const failed = results.find((result) => result.status === 'rejected');
  if (failed !== undefined) throw failed.reason;
}
