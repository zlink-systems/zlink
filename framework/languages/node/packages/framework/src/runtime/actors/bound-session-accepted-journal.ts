import type { ZLinkBlobReference, ZLinkRelocationStore } from '../../contracts';
import { crc32c } from '../foundation/service-relocation-runtime';
import { putNewRelocationBlob } from '../locations/relocation-blob';
import type { ZLinkActorHandoffPacket } from './actor-handoff';

const RETENTION_MS = 24 * 60 * 60 * 1_000;

export interface ZLinkBoundSessionAcceptedJournalRoot {
  readonly sealId: string;
  readonly actorId: string;
  readonly actorGeneration: bigint;
  readonly acceptedHighWater: bigint;
  readonly reference: ZLinkBlobReference;
  readonly checksumCrc32c: number;
}

/** Stores the sealed Session ingress snapshot before relocation publication. */
export class ZLinkBoundSessionAcceptedJournal {
  constructor(private readonly store: ZLinkRelocationStore) {}

  async prepare(
    actorId: string,
    actorGeneration: bigint,
    sealId: string,
    acceptedHighWater: bigint,
    entries: readonly ZLinkActorHandoffPacket[],
    signal?: AbortSignal
  ): Promise<ZLinkBoundSessionAcceptedJournalRoot> {
    requireIdentity(actorId, actorGeneration, sealId, acceptedHighWater);
    const payload = encodeRoot(actorId, actorGeneration, sealId, acceptedHighWater, entries);
    const stored = await putNewRelocationBlob(
      this.store,
      payload,
      RETENTION_MS,
      signal
    );
    const checksumCrc32c = crc32c(payload);
    const read = await this.store.read(stored.reference, signal);
    if (
      read.kind !== 'found'
      || !Buffer.from(read.bytes).equals(payload)
    ) {
      await this.store.delete(stored.reference).catch(() => undefined);
      throw new Error(`Actor '${actorId}' accepted-journal checksum did not match the stored root.`);
    }
    return {
      actorId,
      actorGeneration,
      sealId,
      acceptedHighWater,
      reference: stored.reference,
      checksumCrc32c
    };
  }

  async verify(
    root: ZLinkBoundSessionAcceptedJournalRoot,
    signal?: AbortSignal
  ): Promise<void> {
    requireIdentity(root.actorId, root.actorGeneration, root.sealId, root.acceptedHighWater);
    const read = await this.store.read(root.reference, signal);
    if (read.kind !== 'found' || crc32c(read.bytes) !== root.checksumCrc32c) {
      throw new Error(`Actor '${root.actorId}' accepted-journal root is missing or corrupt.`);
    }
    const decoded = JSON.parse(Buffer.from(read.bytes).toString('utf8')) as {
      readonly actorId?: unknown;
      readonly actorGeneration?: unknown;
      readonly sealId?: unknown;
      readonly acceptedHighWater?: unknown;
    };
    if (
      decoded.actorId !== root.actorId ||
      decoded.actorGeneration !== root.actorGeneration.toString() ||
      decoded.sealId !== root.sealId ||
      decoded.acceptedHighWater !== root.acceptedHighWater.toString()
    ) {
      throw new Error(`Actor '${root.actorId}' accepted-journal identity does not match its relocation fence.`);
    }
  }

  async delete(root: ZLinkBoundSessionAcceptedJournalRoot): Promise<void> {
    await this.store.delete(root.reference);
  }
}

function encodeRoot(
  actorId: string,
  actorGeneration: bigint,
  sealId: string,
  acceptedHighWater: bigint,
  entries: readonly ZLinkActorHandoffPacket[]
): Buffer {
  return Buffer.from(JSON.stringify({
    version: 1,
    actorId,
    actorGeneration: actorGeneration.toString(),
    sealId,
    acceptedHighWater: acceptedHighWater.toString(),
    entries
  }));
}

function requireIdentity(
  actorId: string,
  actorGeneration: bigint,
  sealId: string,
  acceptedHighWater: bigint
): void {
  if (actorId.length === 0 || actorGeneration <= 0n || sealId.length === 0 || acceptedHighWater < 0n) {
    throw new Error('Bound-session accepted-journal identity is invalid.');
  }
}
