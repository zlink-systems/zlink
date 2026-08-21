import type { RoutingId } from '../../contracts';
import { ZLinkSpotKind, zlinkSpotKindFromWire, zlinkSpotKindToWire } from '../../contracts';
import { crc32c } from '../foundation/service-relocation-runtime';
import { decodeRoutingId, encodeRoutingIdStorageHex } from '../routing-id';

const AUTHORITY_MAGIC = Buffer.from([0x5a, 0x4c, 0x41, 0x55]);
const AUTHORITY_VERSION = 1;
const AUTHORITY_FLAGS = 0;
const ACTOR_RELOCATION_MAGIC = Buffer.from([0x5a, 0x4c, 0x41, 0x50]);
const MAXIMUM_BYTES = 1024 * 1024;
const FATAL_UTF8 = new TextDecoder('utf-8', { fatal: true });

export type ZLinkActorAuthorityState = 'creating' | 'ready';

export interface ZLinkActorAuthorityPayload {
  readonly state: ZLinkActorAuthorityState;
  readonly stableType: string;
  readonly actorId: string;
  readonly currentSpotId: string;
  readonly currentSpotGeneration: bigint;
  readonly currentSpotKind: ZLinkSpotKind.Entry | ZLinkSpotKind.User;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly meshName: string;
  readonly nodeRid: RoutingId;
  readonly nodeGeneration: bigint;
}

export interface ZLinkCanonicalAuthorityPayload {
  readonly body: Buffer;
}

interface ActorRelocationEnvelope {
  readonly prefix: Buffer;
  readonly phase: number;
  readonly applicationPayload: Buffer;
}

/** Encodes the durable authority-payload-v1 envelope around one schema body. */
export function encodeCanonicalAuthorityPayload(
  value: ZLinkCanonicalAuthorityPayload
): Buffer {
  const body = Buffer.from(value.body);
  const envelope = concat(
    AUTHORITY_MAGIC,
    Buffer.of(AUTHORITY_VERSION),
    u16be(AUTHORITY_FLAGS),
    u32be(body.byteLength),
    body
  );
  const encoded = concat(envelope, u32be(crc32c(envelope)));
  if (encoded.byteLength > MAXIMUM_BYTES) {
    throw new RangeError('Actor authority payload exceeds 1 MiB.');
  }
  return encoded;
}

/** Decodes and checksum-validates the durable authority-payload-v1 envelope. */
export function decodeCanonicalAuthorityPayload(
  payload: Uint8Array
): ZLinkCanonicalAuthorityPayload | undefined {
  try {
    const reader = new BigEndianReader(payload);
    if (reader.bytes.byteLength < 15 || reader.bytes.byteLength > MAXIMUM_BYTES) {
      return undefined;
    }
    reader.expect(AUTHORITY_MAGIC);
    if (reader.u8() !== AUTHORITY_VERSION || reader.u16() !== AUTHORITY_FLAGS) {
      return undefined;
    }
    const body = reader.take(reader.u32());
    const checksumOffset = reader.offset;
    const checksum = reader.u32();
    if (!reader.done || crc32c(reader.bytes.subarray(0, checksumOffset)) !== checksum) {
      return undefined;
    }
    return { body: Buffer.from(body) };
  } catch {
    return undefined;
  }
}

/** Byte-exact Node counterpart of .NET ZLinkActorAuthorityPayloadCodec.Encode. */
export function encodeActorAuthorityPayload(value: ZLinkActorAuthorityPayload): Buffer {
  const spotKind = zlinkSpotKindToWire(value.currentSpotKind);
  if (spotKind !== 1 && spotKind !== 2) {
    throw new RangeError('Actor currentSpotKind must be Entry or User.');
  }
  const actor = concat(
    text8(value.stableType, 'stableType'),
    text8(value.actorId, 'actorId'),
    Buffer.of(value.state === 'creating' ? 0 : 1),
    text8(value.currentSpotId, 'currentSpotId'),
    nonzeroU64be(value.currentSpotGeneration, 'currentSpotGeneration'),
    Buffer.of(spotKind)
  );
  const body = concat(
    Buffer.of(value.state === 'creating' ? 1 : 0),
    Buffer.of(1),
    u16be(actor.byteLength),
    actor,
    text8(value.ownerId, 'ownerId'),
    nonzeroU64be(value.ownerLeaseGeneration, 'ownerLeaseGeneration'),
    text8(value.meshName, 'meshName'),
    rid(value.nodeRid, 'nodeRid'),
    nonzeroU64be(value.nodeGeneration, 'nodeGeneration'),
    Buffer.of(0),
    u32be(0),
    Buffer.of(0),
    u32be(0)
  );
  return encodeCanonicalAuthorityPayload({ body });
}

/** Byte-exact Node counterpart of .NET ZLinkActorAuthorityPayloadCodec.TryDecodeDirect. */
export function decodeActorAuthorityPayload(
  payload: Uint8Array
): ZLinkActorAuthorityPayload | undefined {
  try {
    const decoded = decodeCanonicalAuthorityPayload(payload);
    if (decoded === undefined) return undefined;
    const body = new BigEndianReader(decoded.body);
    const operation = body.u8();
    if (body.u8() !== 1) return undefined;
    const actor = body.reader(body.u16());
    const stableType = actor.text8();
    const actorId = actor.text8();
    const stateValue = actor.u8();
    const currentSpotId = actor.text8();
    const currentSpotGeneration = actor.nonzeroU64();
    const currentSpotKind = zlinkSpotKindFromWire(actor.u8());
    const state: ZLinkActorAuthorityState | undefined = stateValue === 0 && operation === 1
      ? 'creating'
      : stateValue === 1 && operation === 0
        ? 'ready'
        : undefined;
    if (
      !actor.done
      || state === undefined
      || currentSpotKind !== ZLinkSpotKind.Entry && currentSpotKind !== ZLinkSpotKind.User
    ) {
      return undefined;
    }
    const ownerId = body.text8();
    const ownerLeaseGeneration = body.nonzeroU64();
    const meshName = body.text8();
    const nodeRid = body.rid();
    const nodeGeneration = body.nonzeroU64();
    if (
      body.u8() !== 0
      || body.u32() !== 0
      || body.u8() !== 0
      || body.u32() !== 0
      || !body.done
    ) {
      return undefined;
    }
    return {
      state,
      stableType,
      actorId,
      currentSpotId,
      currentSpotGeneration,
      currentSpotKind,
      ownerId,
      ownerLeaseGeneration,
      meshName,
      nodeRid,
      nodeGeneration
    };
  } catch {
    return undefined;
  }
}

/** Returns the ZLAU payload only when a .NET-compatible ZLAP is Steady. */
export function actorRelocationAuthorityApplicationPayload(
  payload: Uint8Array
): Buffer | undefined {
  const current = decodeActorRelocationEnvelope(payload);
  return current === undefined
    ? Buffer.from(payload)
    : current.phase === 4
      ? Buffer.from(current.applicationPayload)
      : undefined;
}

/** Returns the ZLAU payload from every valid .NET-compatible ZLAP phase. */
export function relocatingActorAuthorityApplicationPayload(payload: Uint8Array): Buffer {
  return Buffer.from(decodeActorRelocationEnvelope(payload)?.applicationPayload ?? payload);
}

/** Replaces only the application payload of a valid ZLAP phase envelope. */
export function replaceActorRelocationAuthorityApplicationPayload(
  payload: Uint8Array,
  applicationPayload: Uint8Array
): Buffer {
  const current = decodeActorRelocationEnvelope(payload);
  if (current === undefined) return Buffer.from(applicationPayload);
  const withoutChecksum = concat(
    current.prefix,
    i32le(applicationPayload.byteLength),
    applicationPayload
  );
  const encoded = concat(withoutChecksum, u32le(crc32c(withoutChecksum)));
  if (encoded.byteLength > MAXIMUM_BYTES) {
    throw new RangeError('Actor relocation authority payload exceeds 1 MiB.');
  }
  return encoded;
}

function decodeActorRelocationEnvelope(
  payload: Uint8Array
): ActorRelocationEnvelope | undefined {
  try {
    const bytes = Buffer.from(payload);
    if (bytes.byteLength < 32 || bytes.byteLength > MAXIMUM_BYTES) return undefined;
    const checksumOffset = bytes.byteLength - 4;
    if (bytes.readUInt32LE(checksumOffset) !== crc32c(bytes.subarray(0, checksumOffset))) {
      return undefined;
    }
    const reader = new LittleEndianReader(bytes.subarray(0, checksumOffset));
    reader.expect(ACTOR_RELOCATION_MAGIC);
    const version = reader.u16();
    if (version !== 5 && version !== 6) return undefined;
    if (reader.take(16).every(byte => byte === 0)) return undefined;
    const phase = reader.u8();
    if (phase < 1 || phase > 4) return undefined;
    const isBound = reader.bool();
    if (isBound) {
      reader.bytes8();
      reader.bytes8();
      reader.text16();
      const bindingGeneration = reader.u64();
      const objectGeneration = reader.u64();
      const authorityOwnerGeneration = reader.u64();
      reader.text16();
      const targetNodeGeneration = reader.u64();
      const ownerLeaseGeneration = reader.u64();
      const sessionOwnerNodeGeneration = reader.u64();
      reader.u64();
      if (version === 6) {
        reader.text16();
        reader.u64();
      }
      if (
        bindingGeneration === 0n
        || objectGeneration === 0n
        || authorityOwnerGeneration === 0n
        || targetNodeGeneration === 0n
        || ownerLeaseGeneration === 0n
        || sessionOwnerNodeGeneration === 0n
      ) {
        return undefined;
      }
    }
    const prefix = Buffer.from(reader.bytes.subarray(0, reader.offset));
    const applicationLength = reader.i32();
    if (applicationLength < 1 || applicationLength > MAXIMUM_BYTES) return undefined;
    const applicationPayload = Buffer.from(reader.take(applicationLength));
    if (!reader.done) return undefined;
    return { prefix, phase, applicationPayload };
  } catch {
    return undefined;
  }
}

function text8(value: string, name: string): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength < 1 || bytes.byteLength > 0xff || bytes.includes(0)) {
    throw new RangeError(`${name} must contain 1..255 UTF-8 bytes without NUL.`);
  }
  return concat(Buffer.of(bytes.byteLength), bytes);
}

function rid(value: RoutingId, name: string): Buffer {
  const bytes = Buffer.from(encodeRoutingIdStorageHex(value), 'hex');
  if (bytes.byteLength < 1 || bytes.byteLength > 0xff) {
    throw new RangeError(`${name} must contain 1..255 bytes.`);
  }
  return concat(Buffer.of(bytes.byteLength), bytes);
}

function nonzeroU64be(value: bigint, name: string): Buffer {
  if (value === 0n || value < 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new RangeError(`${name} must be a non-zero u64.`);
  }
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function u16be(value: number): Buffer {
  const result = Buffer.alloc(2);
  result.writeUInt16BE(value);
  return result;
}

function u32be(value: number): Buffer {
  const result = Buffer.alloc(4);
  result.writeUInt32BE(value);
  return result;
}

function u32le(value: number): Buffer {
  const result = Buffer.alloc(4);
  result.writeUInt32LE(value);
  return result;
}

function i32le(value: number): Buffer {
  const result = Buffer.alloc(4);
  result.writeInt32LE(value);
  return result;
}

function concat(...parts: readonly Uint8Array[]): Buffer {
  return Buffer.concat(parts.map(part => Buffer.from(part)));
}

class BigEndianReader {
  readonly bytes: Buffer;
  offset = 0;

  constructor(payload: Uint8Array) {
    this.bytes = Buffer.from(payload);
  }

  get done(): boolean {
    return this.offset === this.bytes.byteLength;
  }

  expect(expected: Uint8Array): void {
    if (!this.take(expected.byteLength).equals(Buffer.from(expected))) {
      throw new Error('Authority payload magic mismatch.');
    }
  }

  u8(): number {
    return this.take(1)[0]!;
  }

  u16(): number {
    return this.take(2).readUInt16BE(0);
  }

  u32(): number {
    return this.take(4).readUInt32BE(0);
  }

  nonzeroU64(): bigint {
    const value = this.take(8).readBigUInt64BE(0);
    if (value === 0n) throw new Error('Authority non-zero u64 is zero.');
    return value;
  }

  text8(): string {
    const length = this.u8();
    if (length === 0) throw new Error('Authority text is empty.');
    const value = this.take(length);
    if (value.includes(0)) throw new Error('Authority text contains NUL.');
    return FATAL_UTF8.decode(value);
  }

  rid(): RoutingId {
    const length = this.u8();
    if (length === 0) throw new Error('Authority RID is empty.');
    const value = this.take(length);
    let text: string;
    try {
      text = FATAL_UTF8.decode(value);
    } catch {
      text = value.toString('hex');
    }
    return decodeRoutingId(text, value.toString('hex'));
  }

  reader(length: number): BigEndianReader {
    return new BigEndianReader(this.take(length));
  }

  take(length: number): Buffer {
    if (length < 0 || this.offset + length > this.bytes.byteLength) {
      throw new Error('Authority payload is truncated.');
    }
    const result = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return result;
  }
}

class LittleEndianReader {
  readonly bytes: Buffer;
  offset = 0;

  constructor(payload: Uint8Array) {
    this.bytes = Buffer.from(payload);
  }

  get done(): boolean {
    return this.offset === this.bytes.byteLength;
  }

  expect(expected: Uint8Array): void {
    if (!this.take(expected.byteLength).equals(Buffer.from(expected))) {
      throw new Error('Actor relocation payload magic mismatch.');
    }
  }

  u8(): number {
    return this.take(1)[0]!;
  }

  bool(): boolean {
    const value = this.u8();
    if (value !== 0 && value !== 1) throw new Error('Actor relocation bool is invalid.');
    return value === 1;
  }

  u16(): number {
    return this.take(2).readUInt16LE(0);
  }

  i32(): number {
    return this.take(4).readInt32LE(0);
  }

  u64(): bigint {
    return this.take(8).readBigUInt64LE(0);
  }

  bytes8(): Buffer {
    const length = this.u8();
    if (length === 0) throw new Error('Actor relocation byte field is empty.');
    return this.take(length);
  }

  text16(): string {
    const length = this.u16();
    if (length === 0) throw new Error('Actor relocation text is empty.');
    const value = this.take(length);
    if (value.includes(0)) throw new Error('Actor relocation text contains NUL.');
    return FATAL_UTF8.decode(value);
  }

  take(length: number): Buffer {
    if (length < 0 || this.offset + length > this.bytes.byteLength) {
      throw new Error('Actor relocation payload is truncated.');
    }
    const result = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return result;
  }
}
