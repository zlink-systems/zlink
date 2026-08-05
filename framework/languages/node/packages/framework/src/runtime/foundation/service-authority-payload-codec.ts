import { crc32c } from './service-relocation-runtime';
import type { ZLinkLocationOwnerToken } from '../../contracts/Locations';

const AUTHORITY_MAGIC = Buffer.from([0x5a, 0x4c, 0x41, 0x55]);
const AUTHORITY_FORMAT_VERSION = 1;
const AUTHORITY_FLAGS = 0;
const FATAL_UTF8 = new TextDecoder('utf-8', { fatal: true });

export interface ServiceReadySpotAuthority {
  readonly kind: 'user_spot' | 'instance_spot';
  readonly stableType: string;
  readonly spotId: string;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly ownerMeshName: string;
  readonly ownerNodeRid: string;
  readonly ownerNodeGeneration: bigint;
  readonly activationRecovery?: ServiceActivationRecoveryState;
}

export interface ServiceDecodedInstanceAuthority extends ServiceReadySpotAuthority {
  readonly kind: 'instance_spot';
  readonly state: 'coldActivating' | 'ready' | 'closing';
}


export interface ServiceActivationRecoveryState {
  readonly reference: string;
  readonly sha256: Uint8Array;
  readonly encodedSize: number;
  readonly inboxSequence: bigint;
  readonly replayCursor: bigint;
}

export interface ServiceInstanceAuthorityPayload {
  readonly state: 'coldActivating' | 'ready' | 'closing';
  readonly stableType: string;
  readonly spotId: string;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly ownerMeshName: string;
  readonly ownerNodeRid: string;
  readonly ownerNodeGeneration: bigint;
  readonly activationRecovery?: ServiceActivationRecoveryState;
}

export interface ServiceUserSpotAuthorityPayload {
  readonly state: 'creating' | 'ready' | 'closing';
  readonly stableType: string;
  readonly spotId: string;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly ownerMeshName: string;
  readonly ownerNodeRid: string;
  readonly ownerNodeGeneration: bigint;
}

/** Replaces only authority-relocation-state and preserves the canonical owner payload. */
export function replaceServiceAuthorityRelocationState(
  payload: Uint8Array,
  relocationState: Uint8Array
): Buffer {
  const bytes = Buffer.from(payload);
  const reader = new AuthorityReader(bytes);
  reader.expect(AUTHORITY_MAGIC);
  if (reader.u8() !== AUTHORITY_FORMAT_VERSION || reader.u16() !== AUTHORITY_FLAGS) {
    throw new TypeError('Authority payload header is invalid.');
  }
  const bodyLength = reader.u32();
  const body = reader.takeReader(bodyLength);
  const checksumOffset = reader.offset;
  const checksum = reader.u32();
  if (!reader.done || crc32c(bytes.subarray(0, checksumOffset)) !== checksum) {
    throw new TypeError('Authority payload checksum is invalid.');
  }
  body.u8();
  body.u8();
  body.skip(body.u16());
  body.text8();
  body.nonZeroU64();
  body.text8();
  body.rid();
  body.nonZeroU64();
  const relocationOffset = body.offset;
  body.bool8();
  body.skip(body.u32());
  const relocationEnd = body.offset;
  const currentBody = bytes.subarray(11, 11 + bodyLength);
  const nextBody = Buffer.concat([
    currentBody.subarray(0, relocationOffset),
    Buffer.from(relocationState),
    currentBody.subarray(relocationEnd)
  ]);
  const envelope = Buffer.concat([
    AUTHORITY_MAGIC,
    Buffer.of(AUTHORITY_FORMAT_VERSION),
    u16(AUTHORITY_FLAGS),
    u32(nextBody.byteLength),
    nextBody
  ]);
  return Buffer.concat([envelope, u32(crc32c(envelope))]);
}

export function encodeServiceUserSpotAuthorityPayload(
  value: ServiceUserSpotAuthorityPayload
): Buffer {
  const spot = conditional(2, concat(
    rid(value.spotId, 'spotId'),
    text8(value.stableType, 'stableType'),
    Buffer.of(1)
  ));
  const object = conditional(2, spot);
  const body = concat(
    Buffer.of(value.state === 'creating' ? 1 : value.state === 'closing' ? 3 : 0),
    object,
    text8(value.ownerId, 'ownerId'),
    u64(value.ownerLeaseGeneration, 'ownerLeaseGeneration'),
    text8(value.ownerMeshName, 'ownerMeshName'),
    rid(value.ownerNodeRid, 'ownerNodeRid'),
    u64(value.ownerNodeGeneration, 'ownerNodeGeneration'),
    conditional32(0, Buffer.alloc(0)),
    conditional32(0, Buffer.alloc(0))
  );
  const envelope = concat(
    AUTHORITY_MAGIC,
    Buffer.of(AUTHORITY_FORMAT_VERSION),
    u16(AUTHORITY_FLAGS),
    u32(body.byteLength),
    body
  );
  return concat(envelope, u32(crc32c(envelope)));
}

export function encodeServiceInstanceAuthorityPayload(
  value: ServiceInstanceAuthorityPayload
): Buffer {
  if (value.activationRecovery !== undefined && value.state !== 'ready') {
    throw new RangeError('activationRecovery is allowed only on a Ready Instance Spot authority.');
  }
  const instanceBody = concat(
    text8(value.stableType, 'stableType'),
    rid(value.spotId, 'spotId')
  );
  const instance = conditional(
    value.state === 'coldActivating' ? 1 : value.state === 'closing' ? 3 : 2,
    instanceBody
  );
  const spot = conditional(3, instance);
  const object = conditional(2, spot);
  const activationRecovery = value.activationRecovery === undefined
    ? conditional32(0, Buffer.alloc(0))
    : conditional32(1, concat(
        text16(value.activationRecovery.reference, 'activationRecovery.reference'),
        sha256(value.activationRecovery.sha256),
        boundedU32(value.activationRecovery.encodedSize, 'activationRecovery.encodedSize'),
        u64(value.activationRecovery.inboxSequence, 'activationRecovery.inboxSequence'),
        ordinalU64(value.activationRecovery.replayCursor, 'activationRecovery.replayCursor')
      ));
  if (
    value.activationRecovery !== undefined
    && value.activationRecovery.replayCursor > value.activationRecovery.inboxSequence
  ) {
    throw new RangeError('activationRecovery.replayCursor must not exceed inboxSequence.');
  }
  const body = concat(
    Buffer.of(value.state === 'coldActivating' ? 1 : value.state === 'closing' ? 3 : 0),
    object,
    text8(value.ownerId, 'ownerId'),
    u64(value.ownerLeaseGeneration, 'ownerLeaseGeneration'),
    text8(value.ownerMeshName, 'ownerMeshName'),
    rid(value.ownerNodeRid, 'ownerNodeRid'),
    u64(value.ownerNodeGeneration, 'ownerNodeGeneration'),
    conditional32(0, Buffer.alloc(0)),
    activationRecovery
  );
  const envelope = concat(
    AUTHORITY_MAGIC,
    Buffer.of(AUTHORITY_FORMAT_VERSION),
    u16(AUTHORITY_FLAGS),
    u32(body.byteLength),
    body
  );
  return concat(envelope, u32(crc32c(envelope)));
}

export function decodeServiceReadySpotAuthority(
  payload: Uint8Array
): ServiceReadySpotAuthority | undefined {
  const decoded = decodeSpotAuthority(payload);
  return decoded?.state === 'ready' && decoded.operationKind === 0
    ? decoded
    : undefined;
}

export function decodeServiceInstanceAuthorityPayload(
  payload: Uint8Array
): ServiceDecodedInstanceAuthority | undefined {
  const decoded = decodeSpotAuthority(payload);
  if (
    decoded?.kind !== 'instance_spot'
    || !(
      decoded.state === 'coldActivating' && decoded.operationKind === 1
      || decoded.state === 'ready' && decoded.operationKind === 0
      || decoded.state === 'closing' && decoded.operationKind === 3
    )
  ) {
    return undefined;
  }
  return {
    ...decoded,
    kind: 'instance_spot',
    state: decoded.state
  };
}

export function rewriteServiceAuthorityOwner(
  payload: Uint8Array,
  owner: ZLinkLocationOwnerToken
): Buffer | undefined {
  if (decodeSpotAuthority(payload) === undefined) return undefined;
  const bytes = Buffer.from(payload);
  const bodyLength = bytes.readUInt32BE(7);
  const bodyStart = 11;
  const objectLength = bytes.readUInt16BE(bodyStart + 2);
  const ownerLengthOffset = bodyStart + 4 + objectLength;
  if (ownerLengthOffset >= bytes.length) return undefined;
  const ownerLength = bytes[ownerLengthOffset];
  const leaseOffset = ownerLengthOffset + 1 + ownerLength;
  if (leaseOffset + 8 > bytes.length) return undefined;
  bytes.writeBigUInt64BE(owner.leaseGeneration, leaseOffset);
  const checksumOffset = bodyStart + bodyLength;
  bytes.writeUInt32BE(crc32c(bytes.subarray(0, checksumOffset)), checksumOffset);
  return bytes;
}

function decodeSpotAuthority(
  payload: Uint8Array
): (ServiceReadySpotAuthority & {
  readonly state: 'coldActivating' | 'ready' | 'closing' | 'other';
  readonly operationKind: number;
}) | undefined {
  try {
    const reader = new AuthorityReader(payload);
    reader.expect(AUTHORITY_MAGIC);
    if (reader.u8() !== AUTHORITY_FORMAT_VERSION || reader.u16() !== AUTHORITY_FLAGS) {
      return undefined;
    }
    const bodyLength = reader.u32();
    const body = reader.takeReader(bodyLength);
    const checksumOffset = reader.offset;
    const checksum = reader.u32();
    if (!reader.done || crc32c(reader.bytes.subarray(0, checksumOffset)) !== checksum) {
      return undefined;
    }
    const operationKind = body.u8();
    const objectKind = body.u8();
    const object = body.takeReader(body.u16());
    if (objectKind !== 2) return undefined;
    const spotKind = object.u8();
    const spot = object.takeReader(object.u16());
    if (!object.done) return undefined;

    let kind: ServiceReadySpotAuthority['kind'];
    let state: number;
    let stableType: string;
    let spotId: string;
    if (spotKind === 2) {
      kind = 'user_spot';
      spotId = spot.rid();
      stableType = spot.text8();
      state = spot.u8();
    } else if (spotKind === 3) {
      kind = 'instance_spot';
      state = spot.u8();
      const instance = spot.takeReader(spot.u16());
      stableType = instance.text8();
      spotId = instance.rid();
      if (!instance.done) return undefined;
    } else {
      return undefined;
    }
    if (!spot.done) return undefined;
    const ownerId = body.text8();
    const ownerLeaseGeneration = body.nonZeroU64();
    const ownerMeshName = body.text8();
    const ownerNodeRid = body.rid();
    const ownerNodeGeneration = body.nonZeroU64();
    const hasRelocation = body.bool8();
    const relocationLength = body.u32();
    if (hasRelocation || relocationLength !== 0) return undefined;
    const hasActivationRecovery = body.bool8();
    const activationRecoveryBody = body.takeReader(body.u32());
    let activationRecovery: ServiceActivationRecoveryState | undefined;
    if (hasActivationRecovery) {
      const reference = activationRecoveryBody.text16();
      const sha256 = activationRecoveryBody.sizedBytes8(32);
      const encodedSize = activationRecoveryBody.u32();
      if (encodedSize > 1024 * 1024) return undefined;
      const inboxSequence = activationRecoveryBody.nonZeroU64();
      const replayCursor = activationRecoveryBody.u64();
      if (replayCursor > inboxSequence) return undefined;
      activationRecovery = {
        reference,
        sha256,
        encodedSize,
        inboxSequence,
        replayCursor
      };
    }
    if (
      activationRecovery !== undefined
      && (
        kind !== 'instance_spot'
        || state !== 2
        || operationKind !== 0
      )
    ) {
      return undefined;
    }
    if (!activationRecoveryBody.done || !body.done) return undefined;
    return {
      kind,
      state: kind === 'instance_spot'
        ? state === 1
          ? 'coldActivating'
          : state === 2
            ? 'ready'
            : state === 3
              ? 'closing'
              : 'other'
        : state === 1 ? 'ready' : 'other',
      operationKind,
      stableType,
      spotId,
      ownerId,
      ownerLeaseGeneration,
      ownerMeshName,
      ownerNodeRid,
      ownerNodeGeneration,
      ...(activationRecovery === undefined ? {} : { activationRecovery })
    };
  } catch {
    return undefined;
  }
}

function conditional(discriminator: number, body: Uint8Array): Buffer {
  return concat(Buffer.of(discriminator), u16(body.byteLength), body);
}

function conditional32(discriminator: number, body: Uint8Array): Buffer {
  return concat(Buffer.of(discriminator), u32(body.byteLength), body);
}

function text8(value: string, name: string): Buffer {
  return sized8(value, name);
}

function rid(value: string, name: string): Buffer {
  return sized8(value, name);
}

function text16(value: string, name: string): Buffer {
  const bytes = Buffer.from(value);
  if (bytes.byteLength < 1 || bytes.byteLength > 0xffff || bytes.includes(0)) {
    throw new RangeError(`${name} must contain 1..65535 UTF-8 bytes without NUL.`);
  }
  return concat(u16(bytes.byteLength), bytes);
}

function sha256(value: Uint8Array): Buffer {
  if (value.byteLength !== 32) {
    throw new RangeError('activationRecovery.sha256 must contain exactly 32 bytes.');
  }
  return concat(Buffer.of(32), value);
}

function sized8(value: string, name: string): Buffer {
  const bytes = Buffer.from(value);
  if (bytes.byteLength < 1 || bytes.byteLength > 255 || bytes.includes(0)) {
    throw new RangeError(`${name} must contain 1..255 UTF-8 bytes without NUL.`);
  }
  return concat(Buffer.of(bytes.byteLength), bytes);
}

function u16(value: number): Buffer {
  const result = Buffer.alloc(2);
  result.writeUInt16BE(value);
  return result;
}

function u32(value: number): Buffer {
  const result = Buffer.alloc(4);
  result.writeUInt32BE(value);
  return result;
}

function boundedU32(value: number, name: string): Buffer {
  if (!Number.isInteger(value) || value < 0 || value > 1024 * 1024) {
    throw new RangeError(`${name} must be an integer in the range 0..1048576.`);
  }
  return u32(value);
}

function u64(value: bigint, name: string): Buffer {
  if (value <= 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new RangeError(`${name} must be a non-zero u64.`);
  }
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function ordinalU64(value: bigint, name: string): Buffer {
  if (value < 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new RangeError(`${name} must be a u64.`);
  }
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function concat(...parts: readonly Uint8Array[]): Buffer {
  return Buffer.concat(parts.map(part => Buffer.from(part)));
}

class AuthorityReader {
  readonly bytes: Buffer;
  offset = 0;

  constructor(payload: Uint8Array) {
    this.bytes = Buffer.from(payload);
  }

  get done(): boolean {
    return this.offset === this.bytes.byteLength;
  }

  expect(expected: Uint8Array): void {
    const value = this.take(expected.byteLength);
    if (!value.equals(Buffer.from(expected))) throw new Error('Authority payload magic mismatch.');
  }

  u8(): number {
    return this.take(1)[0]!;
  }

  bool8(): boolean {
    const value = this.u8();
    if (value !== 0 && value !== 1) throw new Error('Authority bool8 is invalid.');
    return value === 1;
  }

  u16(): number {
    return this.take(2).readUInt16BE(0);
  }

  u32(): number {
    return this.take(4).readUInt32BE(0);
  }

  nonZeroU64(): bigint {
    const value = this.take(8).readBigUInt64BE(0);
    if (value === 0n) throw new Error('Authority non-zero u64 is zero.');
    return value;
  }

  u64(): bigint {
    return this.take(8).readBigUInt64BE(0);
  }

  text8(): string {
    return this.sized8();
  }

  rid(): string {
    return this.sized8();
  }

  text16(): string {
    const length = this.u16();
    if (length === 0) throw new Error('Authority text is empty.');
    const value = this.take(length);
    if (value.includes(0)) throw new Error('Authority text contains NUL.');
    return FATAL_UTF8.decode(value);
  }

  sizedBytes8(expectedLength: number): Buffer {
    const length = this.u8();
    if (length !== expectedLength) throw new Error('Authority byte field length is invalid.');
    return Buffer.from(this.take(length));
  }

  takeReader(length: number): AuthorityReader {
    return new AuthorityReader(this.take(length));
  }

  skip(length: number): void {
    this.take(length);
  }

  private sized8(): string {
    const length = this.u8();
    if (length === 0) throw new Error('Authority text is empty.');
    const value = this.take(length);
    if (value.includes(0)) throw new Error('Authority text contains NUL.');
    return FATAL_UTF8.decode(value);
  }

  private take(length: number): Buffer {
    if (length < 0 || this.offset + length > this.bytes.byteLength) {
      throw new Error('Authority payload is truncated.');
    }
    const result = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return result;
  }
}
