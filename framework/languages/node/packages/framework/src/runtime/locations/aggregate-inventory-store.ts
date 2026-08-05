import { createHash, timingSafeEqual } from 'node:crypto';
import type {
  ZLinkLocationStore,
  ZLinkStoreKey,
  ZLinkStoreReadResult
} from '../../contracts';
import type {
  ZLinkAggregateFence,
  ZLinkAggregatePrepareRequest
} from '../../contracts/Locations';
import { storeKey } from './in-memory-provider-location-store';

const PREFIX = 'zlink:v11:aggregate-inventory:';
const MAX_PAGE_ENTRIES = 1_024;
const MAX_PAGE_BYTES = 1_024 * 1_024;
const MAX_TREE_LEVELS = 32;
const WRITE_CONCURRENCY = 64;

interface InventoryEntry {
  readonly index: number;
  readonly authorityKey: string;
  readonly expectedStoreVersion: string;
  readonly ownerTransition: 'preserve' | 'newOwner';
  readonly authorityPayloadSha256: string;
  readonly membershipMutationSha256: string;
}

interface InventoryReference {
  readonly level: number;
  readonly index: number;
  readonly startIndex: number;
  readonly entryCount: number;
  readonly sha256: string;
}

interface InventoryPage {
  readonly kind: 'aggregate-inventory-page-v1';
  readonly level: number;
  readonly index: number;
  readonly startIndex: number;
  readonly entryCount: number;
  readonly entries: readonly InventoryEntry[];
  readonly children: readonly InventoryReference[];
}

interface InventoryRoot {
  readonly kind: 'aggregate-inventory-root-v1';
  readonly totalCount: number;
  readonly digest: string;
  readonly declaredDigest: string;
  readonly topLevel: number;
  readonly topPages: readonly InventoryReference[];
  readonly pageCountsByLevel: readonly number[];
}

interface InventoryTree {
  readonly root: InventoryRoot;
  readonly pages: readonly InventoryPage[];
}

/**
 * Stores the Framework-owned participant inventory through only the opaque
 * Location Store SPI. Providers do not interpret aggregate or participant data.
 */
export class ZLinkAggregateInventoryStore {
  constructor(private readonly provider: ZLinkLocationStore) {}

  async store(
    request: ZLinkAggregatePrepareRequest,
    signal?: AbortSignal
  ): Promise<void> {
    const fence = aggregateFence(request);
    const tree = buildTree(request);
    await parallelForEach(tree.pages, WRITE_CONCURRENCY, async page => {
      const bytes = encode(page);
      if (page.entries.length > MAX_PAGE_ENTRIES
        || page.children.length > MAX_PAGE_ENTRIES
        || bytes.byteLength > MAX_PAGE_BYTES) {
        throw new Error('Aggregate inventory page exceeds its encoded bounds.');
      }
      await putImmutable(
        this.provider,
        pageKey(fence, page.level, page.index),
        bytes,
        signal
      );
    });
    await putImmutable(this.provider, rootKey(fence), encode(tree.root), signal);
    await this.read(fence, request.inventoryDigest, signal);
  }

  async read(
    fence: ZLinkAggregateFence,
    expectedDeclaredDigest?: Uint8Array,
    signal?: AbortSignal
  ): Promise<readonly {
    readonly authorityKey: string;
    readonly expectedStoreVersion: string;
    readonly ownerTransition: 'preserve' | 'newOwner';
    readonly authorityPayloadSha256: string;
    readonly membershipMutationSha256: string;
  }[]> {
    const rootBytes = await requireFound(this.provider, rootKey(fence), signal);
    if (rootBytes.byteLength > MAX_PAGE_BYTES) {
      throw dataLost(fence, 'root exceeds 1 MiB');
    }
    const root = decodeRoot(rootBytes);
    validateRoot(root, fence);
    if (
      expectedDeclaredDigest !== undefined
      && !sameHash(root.declaredDigest, expectedDeclaredDigest)
    ) {
      throw dataLost(fence, 'declared digest does not match');
    }

    const entries: InventoryEntry[] = [];
    const observedPageCounts = Array.from(
      { length: root.topLevel + 1 },
      () => 0
    );
    const visited = new Set<string>();
    const readPage = async (reference: InventoryReference): Promise<void> => {
      validateReference(reference, root, fence);
      const identity = `${reference.level}:${reference.index}`;
      if (visited.has(identity)) throw dataLost(fence, 'page reference is duplicated');
      visited.add(identity);
      const bytes = await requireFound(
        this.provider,
        pageKey(fence, reference.level, reference.index),
        signal
      );
      if (
        bytes.byteLength > MAX_PAGE_BYTES
        || !sameHash(reference.sha256, createHash('sha256').update(bytes).digest())
      ) {
        throw dataLost(fence, 'page checksum is invalid');
      }
      const page = decodePage(bytes);
      if (
        page.level !== reference.level
        || page.index !== reference.index
        || page.startIndex !== reference.startIndex
        || page.entryCount !== reference.entryCount
      ) {
        throw dataLost(fence, 'page metadata changed');
      }
      if (
        page.entries.length > MAX_PAGE_ENTRIES
        || page.children.length > MAX_PAGE_ENTRIES
      ) {
        throw dataLost(fence, 'page entry bound is invalid');
      }
      observedPageCounts[page.level]!++;
      if (page.level === 0) {
        if (
          page.entries.length < 1
          || page.children.length !== 0
          || page.entries.length !== page.entryCount
        ) {
          throw dataLost(fence, 'leaf page bounds are invalid');
        }
        for (let offset = 0; offset < page.entries.length; offset++) {
          const entry = page.entries[offset]!;
          if (entry.index !== page.startIndex + offset) {
            throw dataLost(fence, 'leaf page entries are reordered');
          }
          entries.push(entry);
        }
        return;
      }
      if (page.entries.length !== 0 || page.children.length < 1) {
        throw dataLost(fence, 'index page bounds are invalid');
      }
      let childStart = page.startIndex;
      for (const child of page.children) {
        if (child.level !== page.level - 1 || child.startIndex !== childStart) {
          throw dataLost(fence, 'index page children are reordered');
        }
        await readPage(child);
        childStart += child.entryCount;
      }
      if (childStart !== page.startIndex + page.entryCount) {
        throw dataLost(fence, 'index page entry count changed');
      }
    };
    let expectedStart = 0;
    for (const reference of root.topPages) {
      if (
        reference.level !== root.topLevel
        || reference.startIndex !== expectedStart
      ) {
        throw dataLost(fence, 'top page references are reordered');
      }
      await readPage(reference);
      expectedStart += reference.entryCount;
    }
    if (
      expectedStart !== root.totalCount
      || entries.length !== root.totalCount
      || observedPageCounts.some(
        (count, level) => count !== root.pageCountsByLevel[level]
      )
    ) {
      throw dataLost(fence, 'page counts do not match the root');
    }
    for (let level = 0; level < observedPageCounts.length; level++) {
      for (let index = 0; index < observedPageCounts[level]!; index++) {
        if (!visited.has(`${level}:${index}`)) {
          throw dataLost(fence, 'page indexes are not contiguous');
        }
      }
    }

    const digest = createHash('sha256');
    for (let index = 0; index < entries.length; index++) {
      const entry = entries[index]!;
      validateEntry(entry, index, fence);
      digest.update(encode(entry));
    }
    if (!sameHash(root.digest, digest.digest())) {
      throw dataLost(fence, 'inventory digest does not match');
    }
    return entries.map(entry => ({ ...entry }));
  }
}

function buildTree(request: ZLinkAggregatePrepareRequest): InventoryTree {
  if (request.participants.length < 1) {
    throw new RangeError('Aggregate inventory requires at least one participant.');
  }
  if (request.inventoryDigest.byteLength !== 32) {
    throw new TypeError('Aggregate inventory digest must contain 32 bytes.');
  }
  const entries = request.participants.map((participant, index): InventoryEntry => ({
    index,
    authorityKey: participant.authorityKey.value,
    expectedStoreVersion: participant.expectedStoreVersion.value,
    ownerTransition: participant.ownerTransition,
    authorityPayloadSha256: sha256(participant.authorityPayload),
    membershipMutationSha256: sha256(participant.membershipMutation)
  }));
  const keys = entries.map(entry => entry.authorityKey);
  if (
    new Set(keys).size !== keys.length
    || keys.some((key, index) => index > 0 && keys[index - 1]!.localeCompare(key) >= 0)
  ) {
    throw new TypeError('Aggregate participants must be unique and canonically sorted.');
  }

  const digest = createHash('sha256');
  for (const entry of entries) digest.update(encode(entry));
  const pages: InventoryPage[] = [];
  let references = packLeafPages(entries, pages);
  let level = 0;
  while (
    references.length > MAX_PAGE_ENTRIES
    || encode(rootCandidate(entries.length, level, references, pages, '', '')).byteLength
      > MAX_PAGE_BYTES
  ) {
    level++;
    if (level >= MAX_TREE_LEVELS) {
      throw new RangeError('Aggregate inventory tree exceeds 32 levels.');
    }
    references = packIndexPages(level, references, pages);
  }
  const root = rootCandidate(
    entries.length,
    level,
    references,
    pages,
    digest.digest('hex'),
    Buffer.from(request.inventoryDigest).toString('hex')
  );
  if (encode(root).byteLength > MAX_PAGE_BYTES) {
    throw new RangeError('Aggregate inventory root exceeds 1 MiB.');
  }
  return { root, pages };
}

function packLeafPages(
  entries: readonly InventoryEntry[],
  pages: InventoryPage[]
): InventoryReference[] {
  const references: InventoryReference[] = [];
  for (let offset = 0; offset < entries.length;) {
    let count = Math.min(MAX_PAGE_ENTRIES, entries.length - offset);
    let page: InventoryPage;
    let bytes: Uint8Array;
    do {
      page = {
        kind: 'aggregate-inventory-page-v1',
        level: 0,
        index: references.length,
        startIndex: offset,
        entryCount: count,
        entries: entries.slice(offset, offset + count),
        children: []
      };
      bytes = encode(page);
      if (bytes.byteLength <= MAX_PAGE_BYTES) break;
      count = Math.floor(count / 2);
    } while (count > 0);
    if (count < 1 || bytes!.byteLength > MAX_PAGE_BYTES) {
      throw new RangeError('Aggregate inventory entry exceeds 1 MiB.');
    }
    pages.push(page!);
    references.push(referenceFor(page!, bytes!));
    offset += count;
  }
  return references;
}

function packIndexPages(
  level: number,
  children: readonly InventoryReference[],
  pages: InventoryPage[]
): InventoryReference[] {
  const references: InventoryReference[] = [];
  for (let offset = 0; offset < children.length;) {
    let count = Math.min(MAX_PAGE_ENTRIES, children.length - offset);
    let page: InventoryPage;
    let bytes: Uint8Array;
    do {
      const selected = children.slice(offset, offset + count);
      page = {
        kind: 'aggregate-inventory-page-v1',
        level,
        index: references.length,
        startIndex: selected[0]!.startIndex,
        entryCount: selected.reduce((sum, child) => sum + child.entryCount, 0),
        entries: [],
        children: selected
      };
      bytes = encode(page);
      if (bytes.byteLength <= MAX_PAGE_BYTES) break;
      count = Math.floor(count / 2);
    } while (count > 0);
    if (count < 1 || bytes!.byteLength > MAX_PAGE_BYTES) {
      throw new RangeError('Aggregate inventory page reference exceeds 1 MiB.');
    }
    pages.push(page!);
    references.push(referenceFor(page!, bytes!));
    offset += count;
  }
  return references;
}

function rootCandidate(
  totalCount: number,
  topLevel: number,
  topPages: readonly InventoryReference[],
  pages: readonly InventoryPage[],
  digest: string,
  declaredDigest: string
): InventoryRoot {
  const pageCountsByLevel = Array.from({ length: topLevel + 1 }, () => 0);
  for (const page of pages) {
    if (page.level <= topLevel) pageCountsByLevel[page.level]!++;
  }
  return {
    kind: 'aggregate-inventory-root-v1',
    totalCount,
    digest,
    declaredDigest,
    topLevel,
    topPages,
    pageCountsByLevel
  };
}

function validateRoot(root: InventoryRoot, fence: ZLinkAggregateFence): void {
  if (
    !Number.isSafeInteger(root.totalCount)
    || root.totalCount < 1
    || !isSha256(root.digest)
    || !isSha256(root.declaredDigest)
    || !Number.isSafeInteger(root.topLevel)
    || root.topLevel < 0
    || root.topLevel >= MAX_TREE_LEVELS
    || root.topPages.length < 1
    || root.topPages.length > MAX_PAGE_ENTRIES
    || root.pageCountsByLevel.length !== root.topLevel + 1
    || root.pageCountsByLevel.some(
      count => !Number.isSafeInteger(count) || count < 1 || count > root.totalCount
    )
  ) {
    throw dataLost(fence, 'root metadata is invalid');
  }
  const leafPages = root.pageCountsByLevel[0]!;
  if (
    leafPages > root.totalCount
    || root.totalCount > leafPages * MAX_PAGE_ENTRIES
    || root.topPages.length !== root.pageCountsByLevel[root.topLevel]
  ) {
    throw dataLost(fence, 'root page counts are invalid');
  }
  for (let level = 1; level < root.pageCountsByLevel.length; level++) {
    const lower = root.pageCountsByLevel[level - 1]!;
    const current = root.pageCountsByLevel[level]!;
    if (current < Math.ceil(lower / MAX_PAGE_ENTRIES) || current > lower) {
      throw dataLost(fence, 'index page counts are invalid');
    }
  }
}

function validateReference(
  reference: InventoryReference,
  root: InventoryRoot,
  fence: ZLinkAggregateFence
): void {
  if (
    !Number.isSafeInteger(reference.level)
    || reference.level < 0
    || reference.level > root.topLevel
    || !Number.isSafeInteger(reference.index)
    || reference.index < 0
    || !Number.isSafeInteger(reference.startIndex)
    || reference.startIndex < 0
    || !Number.isSafeInteger(reference.entryCount)
    || reference.entryCount < 1
    || !isSha256(reference.sha256)
  ) {
    throw dataLost(fence, 'page reference is invalid');
  }
}

function validateEntry(
  entry: InventoryEntry,
  index: number,
  fence: ZLinkAggregateFence
): void {
  if (
    entry.index !== index
    || entry.authorityKey.length < 1
    || entry.expectedStoreVersion.length < 1
    || !['preserve', 'newOwner'].includes(entry.ownerTransition)
    || !isSha256(entry.authorityPayloadSha256)
    || !isSha256(entry.membershipMutationSha256)
  ) {
    throw dataLost(fence, 'inventory entry is invalid or reordered');
  }
}

async function putImmutable(
  provider: ZLinkLocationStore,
  key: ZLinkStoreKey,
  bytes: Uint8Array,
  signal?: AbortSignal
): Promise<void> {
  const result = await provider.write({
    conditions: [{ kind: 'missing', key }],
    mutations: [{ kind: 'put', key, bytes }]
  }, signal);
  if (result.kind === 'applied') return;
  const current = await provider.read(key, signal);
  if (
    current.kind !== 'found'
    || !Buffer.from(current.value.bytes).equals(Buffer.from(bytes))
  ) {
    throw new Error(`Immutable aggregate inventory value changed: ${key.value}.`);
  }
}

async function requireFound(
  provider: ZLinkLocationStore,
  key: ZLinkStoreKey,
  signal?: AbortSignal
): Promise<Uint8Array> {
  const result: ZLinkStoreReadResult = await provider.read(key, signal);
  if (result.kind !== 'found') {
    throw new Error(`Aggregate inventory value is missing: ${key.value}.`);
  }
  return result.value.bytes;
}

function decodeRoot(bytes: Uint8Array): InventoryRoot {
  const value: unknown = JSON.parse(Buffer.from(bytes).toString('utf8'));
  if (
    value === null
    || typeof value !== 'object'
    || !('kind' in value)
    || value.kind !== 'aggregate-inventory-root-v1'
  ) {
    throw new Error('Invalid inventory root.');
  }
  return value as InventoryRoot;
}

function decodePage(bytes: Uint8Array): InventoryPage {
  const value: unknown = JSON.parse(Buffer.from(bytes).toString('utf8'));
  if (
    value === null
    || typeof value !== 'object'
    || !('kind' in value)
    || value.kind !== 'aggregate-inventory-page-v1'
    || !('entries' in value)
    || !Array.isArray(value.entries)
    || !('children' in value)
    || !Array.isArray(value.children)
  ) {
    throw new Error('Invalid inventory page.');
  }
  return value as InventoryPage;
}

function encode(value: unknown): Uint8Array {
  return Buffer.from(JSON.stringify(value), 'utf8');
}

function referenceFor(
  page: InventoryPage,
  encoded: Uint8Array
): InventoryReference {
  return {
    level: page.level,
    index: page.index,
    startIndex: page.startIndex,
    entryCount: page.entryCount,
    sha256: sha256(encoded)
  };
}

function aggregateFence(request: ZLinkAggregatePrepareRequest): ZLinkAggregateFence {
  return {
    aggregateId: request.aggregateId,
    aggregateGeneration: request.aggregateGeneration
  };
}

function rootKey(fence: ZLinkAggregateFence): ZLinkStoreKey {
  return storeKey(`${aggregatePrefix(fence)}root`);
}

function pageKey(
  fence: ZLinkAggregateFence,
  level: number,
  index: number
): ZLinkStoreKey {
  return storeKey(`${aggregatePrefix(fence)}page:${level}:${index}`);
}

function aggregatePrefix(fence: ZLinkAggregateFence): string {
  return `${PREFIX}${encodeURIComponent(fence.aggregateId.value)}:${fence.aggregateGeneration}:`;
}

function sha256(bytes: Uint8Array): string {
  return createHash('sha256').update(bytes).digest('hex');
}

function isSha256(value: unknown): value is string {
  return typeof value === 'string' && /^[0-9a-f]{64}$/.test(value);
}

function sameHash(expected: string, actual: Uint8Array): boolean {
  return isSha256(expected)
    && actual.byteLength === 32
    && timingSafeEqual(Buffer.from(expected, 'hex'), Buffer.from(actual));
}

function dataLost(fence: ZLinkAggregateFence, reason: string): Error {
  return new Error(
    `Aggregate '${fence.aggregateId.value}:${fence.aggregateGeneration}' inventory ${reason}.`
  );
}

async function parallelForEach<T>(
  values: readonly T[],
  concurrency: number,
  operation: (value: T) => Promise<void>
): Promise<void> {
  let next = 0;
  await Promise.all(Array.from(
    { length: Math.min(concurrency, values.length) },
    async () => {
      while (next < values.length) {
        const index = next++;
        await operation(values[index]!);
      }
    }
  ));
}
