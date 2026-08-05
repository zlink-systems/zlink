import {
  decodeServiceWireRelocationEnvelope,
  encodeServiceWireRelocationEnvelope,
  encodeServiceWireRelocationProgress,
  type ServiceWireRelocationEnvelope
} from './service-relocation-wire-codec';
import type {
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot
} from '../../contracts/Locations';
import type { ZLinkAuthorityStore } from '../locations/internal-store-contracts';
import {
  replaceServiceAuthorityRelocationState
} from './service-authority-payload-codec';
import {
  crc32c,
  type ServiceRelocationStorePort
} from './service-relocation-runtime';

export interface ServiceCanonicalRelocationAuthorityState {
  readonly relocationHigh: bigint;
  readonly relocationLow: bigint;
  readonly targetAttemptGeneration: bigint;
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: bigint;
  readonly sourceOwnerId: string;
  readonly sourceOwnerLeaseGeneration: bigint;
  readonly targetNodeRid: string;
  readonly targetNodeGeneration: bigint;
  readonly targetOwnerId: string;
  readonly targetOwnerLeaseGeneration: bigint;
  readonly reservationGeneration: bigint;
  readonly coordinatorOwnerId: string;
  readonly coordinatorLeaseGeneration: bigint;
  readonly coordinatorNodeRid: string;
  readonly coordinatorNodeGeneration: bigint;
  readonly phase: 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9;
  readonly relocationReference: string;
  readonly relocationChecksumCrc32c: number;
  readonly applicationVersion: bigint;
  readonly sourceCleanupState: 0 | 1 | 2;
}

export interface ServiceCanonicalRelocationPublication {
  readonly authority: ZLinkAuthoritySnapshot;
  readonly root: ServiceWireRelocationEnvelope;
  readonly reference: string;
  readonly checksumCrc32c: number;
}

/** Publishes canonical roots and their exact authority projection with one CAS. */
export class ServiceCanonicalRelocationPublicationRuntime {
  constructor(
    private readonly authority: Pick<ZLinkAuthorityStore, 'readAuthority' | 'compareExchangeAuthority'>,
    private readonly relocation: ServiceRelocationStorePort
  ) {}

  async publish(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    root: ServiceWireRelocationEnvelope,
    state: Omit<ServiceCanonicalRelocationAuthorityState,
      'relocationReference' | 'relocationChecksumCrc32c'>,
    signal?: AbortSignal
  ): Promise<ServiceCanonicalRelocationPublication> {
    const encoded = encodeServiceWireRelocationEnvelope(root);
    const checksumCrc32c = crc32c(encoded);
    const stored = await this.relocation.put(encoded, 24 * 60 * 60 * 1_000, signal);
    if (stored.checksumCrc32c !== checksumCrc32c || stored.expiresAtMs <= stored.storeNowMs) {
      await this.relocation.delete(stored.reference, signal);
      throw new Error('Relocation Store returned a different canonical root receipt.');
    }
    const nextState = {
      ...state,
      relocationReference: stored.reference,
      relocationChecksumCrc32c: checksumCrc32c
    };
    const payload = replaceServiceAuthorityRelocationState(
      expected.payload,
      encodeServiceCanonicalRelocationAuthorityState(nextState, root)
    );
    let current: ZLinkAuthoritySnapshot;
    try {
      const result = await this.authority.compareExchangeAuthority(key, expected.storeVersion, {
        kind: 'put', generationTransition: 'preserve', payload
      }, signal);
      if (result.kind !== 'stored') throw new Error('Canonical relocation authority CAS conflicted.');
      const { kind: _kind, ...snapshot } = result;
      current = { kind: 'snapshot', ...snapshot };
    } catch (error) {
      const reconciled = await this.authority.readAuthority(key, signal);
      if (reconciled.kind !== 'snapshot' || !Buffer.from(reconciled.payload).equals(payload)) {
        await this.relocation.delete(stored.reference, signal);
        throw error;
      }
      current = reconciled;
    }
    return { authority: current, root, reference: stored.reference, checksumCrc32c };
  }

  async replace(
    key: ZLinkAuthorityKey,
    expected: ServiceCanonicalRelocationPublication,
    successor: ServiceWireRelocationEnvelope,
    state: Omit<ServiceCanonicalRelocationAuthorityState,
      'relocationReference' | 'relocationChecksumCrc32c'>,
    signal?: AbortSignal
  ): Promise<ServiceCanonicalRelocationPublication> {
    const published = await this.publish(key, expected.authority, successor, state, signal);
    await this.relocation.delete(expected.reference, signal);
    return published;
  }

  async release(
    key: ZLinkAuthorityKey,
    expected: ServiceCanonicalRelocationPublication,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    const verified = decodeServiceWireRelocationEnvelope(
      encodeServiceWireRelocationEnvelope(expected.root)
    );
    if (verified.terminalCompletions.some(value => value.deliveryState === 0)) {
      throw new Error('Canonical relocation root still has pending reply relays.');
    }
    const payload = replaceServiceAuthorityRelocationState(
      expected.authority.payload,
      Buffer.concat([Buffer.of(0), Buffer.alloc(4)])
    );
    const result = await this.authority.compareExchangeAuthority(
      key,
      expected.authority.storeVersion,
      { kind: 'put', generationTransition: 'preserve', payload },
      signal
    );
    if (result.kind !== 'stored') throw new Error('Canonical relocation release CAS conflicted.');
    await this.relocation.delete(expected.reference, signal);
    const { kind: _kind, ...snapshot } = result;
    return { kind: 'snapshot', ...snapshot };
  }
}

/** Encodes the `hasRelocation=true` case of authority-relocation-state. */
export function encodeServiceCanonicalRelocationAuthorityState(
  value: ServiceCanonicalRelocationAuthorityState,
  root: ServiceWireRelocationEnvelope
): Buffer {
  if (value.relocationHigh !== root.relocationHigh || value.relocationLow !== root.relocationLow) {
    throw new TypeError('Authority relocation identity differs from its immutable root.');
  }
  if (value.applicationVersion !== root.applicationVersion) {
    throw new TypeError('Authority application version differs from its immutable root.');
  }
  for (const field of [
    value.sourceNodeGeneration,
    value.sourceOwnerLeaseGeneration,
    value.targetNodeGeneration,
    value.targetOwnerLeaseGeneration,
    value.coordinatorLeaseGeneration,
    value.coordinatorNodeGeneration
  ]) {
    if (field <= 0n) throw new TypeError('Canonical relocation authority fence is unavailable.');
  }
  if (value.targetAttemptGeneration < 0n || value.reservationGeneration < 0n) {
    throw new TypeError('Canonical relocation authority generation is invalid.');
  }
  const terminalCompletionCount = root.terminalCompletions.length;
  const pendingRelayCount = root.terminalCompletions.reduce(
    (count, completion) => count + (completion.deliveryState === 0 ? 1 : 0),
    0
  );
  const body = concat(
    u64(value.relocationHigh),
    u64(value.relocationLow),
    u64(value.targetAttemptGeneration),
    text8(value.sourceNodeRid),
    u64(value.sourceNodeGeneration),
    text8(value.sourceOwnerId),
    u64(value.sourceOwnerLeaseGeneration),
    optionalText8(value.targetNodeRid),
    u64(value.targetNodeGeneration),
    optionalText8(value.targetOwnerId),
    u64(value.targetOwnerLeaseGeneration),
    u64(value.reservationGeneration),
    text8(value.coordinatorOwnerId),
    u64(value.coordinatorLeaseGeneration),
    text8(value.coordinatorNodeRid),
    u64(value.coordinatorNodeGeneration),
    Buffer.of(value.phase),
    relocationRoot(value.relocationReference, value.relocationChecksumCrc32c),
    i64(value.applicationVersion),
    encodeServiceWireRelocationProgress(root.participantProgress),
    u32(terminalCompletionCount),
    u32(pendingRelayCount),
    Buffer.of(value.sourceCleanupState)
  );
  return concat(Buffer.of(1), u32(body.byteLength), body);
}

function relocationRoot(reference: string, checksum: number): Buffer {
  const body = concat(text16(reference), u32(checksum));
  return concat(Buffer.of(1), u16(body.byteLength), body);
}

function optionalText8(value: string): Buffer {
  return value.length === 0 ? Buffer.of(0) : text8(value);
}

function text8(value: string): Buffer {
  const encoded = Buffer.from(value, 'utf8');
  if (encoded.byteLength < 1 || encoded.byteLength > 255 || encoded.includes(0)) {
    throw new TypeError('Canonical relocation authority text8 is invalid.');
  }
  return concat(Buffer.of(encoded.byteLength), encoded);
}

function text16(value: string): Buffer {
  const encoded = Buffer.from(value, 'utf8');
  if (encoded.byteLength < 1 || encoded.byteLength > 4_096 || encoded.includes(0)) {
    throw new TypeError('Canonical relocation reference is invalid.');
  }
  return concat(u16(encoded.byteLength), encoded);
}

function u16(value: number): Buffer {
  const result = Buffer.allocUnsafe(2);
  result.writeUInt16BE(value);
  return result;
}

function u32(value: number): Buffer {
  if (!Number.isSafeInteger(value) || value < 0 || value > 0xffff_ffff) {
    throw new RangeError('Canonical relocation u32 is invalid.');
  }
  const result = Buffer.allocUnsafe(4);
  result.writeUInt32BE(value);
  return result;
}

function u64(value: bigint): Buffer {
  if (value < 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new RangeError('Canonical relocation u64 is invalid.');
  }
  const result = Buffer.allocUnsafe(8);
  result.writeBigUInt64BE(value);
  return result;
}

function i64(value: bigint): Buffer {
  if (value < 0n || value > 0x7fff_ffff_ffff_ffffn) {
    throw new RangeError('Canonical relocation application version is invalid.');
  }
  const result = Buffer.allocUnsafe(8);
  result.writeBigInt64BE(value);
  return result;
}

function concat(...values: readonly Uint8Array[]): Buffer {
  return Buffer.concat(values.map(value => Buffer.from(value)));
}
