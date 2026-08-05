import type { ZLinkClientServerServerDescriptor } from '../../contracts/Locations';
import { ZLinkFrameworkRuntimeState } from '../../contracts/Locations';

const MAGIC_0 = 0x5a;
const MAGIC_1 = 0x4d;
const WIRE_MAJOR = 1;
const TOPOLOGY_CLIENT_SERVER = 2;
const ROLE_CLIENT = 1;
const ROLE_SERVER = 2;
const DIRECTION_CLIENT_TO_SERVER = 1;
const COMMAND_HELLO = 1;
const COMMAND_ADMIT = 2;
const COMMAND_REJECT = 3;
const COMMAND_UPDATE = 4;
const COMMAND_LIVENESS_PROBE = 5;
const COMMAND_LIVENESS_ACK = 6;
const MAX_DESCRIPTOR_BYTES = 1024 * 1024;
const MAX_U32 = 0xffff_ffff;
const MAX_GENERATION = 0x7fff_ffff_ffff_ffffn;

export interface ZLinkClientServerHello {
  readonly channelName: string;
  readonly securityIdentity: string;
  readonly normalizedEffectiveMaxMessageBytes: number;
}

export interface ZLinkClientServerAdmission {
  readonly channelName: string;
  readonly serverRid: string;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly weight: number;
  readonly state: ZLinkFrameworkRuntimeState;
  readonly securityIdentity: string;
  readonly normalizedEffectiveMaxMessageBytes: number;
  readonly advertisedEndpoint: string;
}

export type ZLinkClientServerControlRecord =
  | { readonly kind: 'hello'; readonly hello: ZLinkClientServerHello }
  | { readonly kind: 'admit'; readonly admission: ZLinkClientServerAdmission }
  | { readonly kind: 'update'; readonly admission: ZLinkClientServerAdmission }
  | { readonly kind: 'reject'; readonly reason: number }
  | { readonly kind: 'livenessProbe'; readonly probeId: bigint }
  | { readonly kind: 'livenessAck'; readonly probeId: bigint };

export class ZLinkClientServerServiceWireError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ZLinkClientServerServiceWireError';
  }
}

export function encodeClientServerHello(value: ZLinkClientServerHello): Buffer {
  const roleBody = concat(
    encodeText8(value.channelName, 'channelName'),
    Buffer.of(DIRECTION_CLIENT_TO_SERVER),
    encodeText8(value.securityIdentity, 'securityIdentity'),
    encodeNonZeroU32(
      value.normalizedEffectiveMaxMessageBytes,
      'normalizedEffectiveMaxMessageBytes'
    )
  );
  return encodeAdmission(COMMAND_HELLO, ROLE_CLIENT, roleBody);
}

export function encodeClientServerAdmit(
  descriptor: ZLinkClientServerServerDescriptor,
  normalizedEffectiveMaxMessageBytes: number
): Buffer {
  return encodeServerAdmission(COMMAND_ADMIT, descriptor, normalizedEffectiveMaxMessageBytes);
}

export function encodeClientServerUpdate(
  descriptor: ZLinkClientServerServerDescriptor,
  normalizedEffectiveMaxMessageBytes: number
): Buffer {
  return encodeServerAdmission(COMMAND_UPDATE, descriptor, normalizedEffectiveMaxMessageBytes);
}

export function encodeClientServerReject(reason: number): Buffer {
  if (!Number.isInteger(reason) || reason < 1 || reason > 12) {
    fail('ClientServer reject reason must be between 1 and 12.');
  }
  return concat(prefix(COMMAND_REJECT), encodeU32(reason));
}

export function encodeClientServerLivenessProbe(probeId: bigint): Buffer {
  return encodeLiveness(COMMAND_LIVENESS_PROBE, probeId);
}

export function encodeClientServerLivenessAck(probeId: bigint): Buffer {
  return encodeLiveness(COMMAND_LIVENESS_ACK, probeId);
}

export function isClientServerControlFrame(frame: Uint8Array): boolean {
  return frame.byteLength >= 5
    && frame[0] === MAGIC_0
    && frame[1] === MAGIC_1
    && frame[2] === WIRE_MAJOR;
}

export function decodeClientServerControl(frame: Uint8Array): ZLinkClientServerControlRecord {
  if (frame.byteLength > MAX_DESCRIPTOR_BYTES) fail('ClientServer control record is oversized.');
  const reader = new Reader(frame);
  if (reader.u8('magic[0]') !== MAGIC_0
    || reader.u8('magic[1]') !== MAGIC_1
    || reader.u8('wireMajor') !== WIRE_MAJOR) {
    fail('ClientServer control record prefix is invalid.');
  }
  const command = reader.u8('command');
  if (reader.u8('flags') !== 0) fail('ClientServer control flags are invalid.');
  if (command === COMMAND_REJECT) {
    const reason = reader.u32('reason');
    reader.end();
    if (reason < 1 || reason > 12) fail('ClientServer reject reason is invalid.');
    return { kind: 'reject', reason };
  }
  if (command === COMMAND_LIVENESS_PROBE || command === COMMAND_LIVENESS_ACK) {
    const probeId = reader.nonZeroU64('probeId');
    reader.end();
    return command === COMMAND_LIVENESS_PROBE
      ? { kind: 'livenessProbe', probeId }
      : { kind: 'livenessAck', probeId };
  }
  if (command !== COMMAND_HELLO && command !== COMMAND_ADMIT && command !== COMMAND_UPDATE) {
    fail('ClientServer control command is invalid.');
  }
  if (reader.u8('topologyKind') !== TOPOLOGY_CLIENT_SERVER) {
    fail('Control record is not ClientServer topology.');
  }
  const admissionLength = reader.u32('admissionLength');
  if (admissionLength !== reader.remaining) fail('ClientServer admission length is invalid.');
  const role = reader.u8('role');
  const roleLength = reader.u16('roleLength');
  if (roleLength !== reader.remaining) fail('ClientServer role body length is invalid.');
  if (command === COMMAND_HELLO) {
    if (role !== ROLE_CLIENT) fail('ClientServer hello role is invalid.');
    const hello = decodeClientHello(reader);
    reader.end();
    return { kind: 'hello', hello };
  }
  if (role !== ROLE_SERVER) fail('ClientServer server admission role is invalid.');
  const admission = decodeServerAdmission(reader);
  reader.end();
  return command === COMMAND_ADMIT
    ? { kind: 'admit', admission }
    : { kind: 'update', admission };
}

function encodeLiveness(command: number, probeId: bigint): Buffer {
  return concat(prefix(command), encodeNonZeroU64(probeId, 'probeId'));
}

function encodeServerAdmission(
  command: number,
  descriptor: ZLinkClientServerServerDescriptor,
  normalizedEffectiveMaxMessageBytes: number
): Buffer {
  const roleBody = concat(
    encodeText8(descriptor.channelName, 'channelName'),
    Buffer.of(DIRECTION_CLIENT_TO_SERVER),
    encodeBytes8(Buffer.from(String(descriptor.serverRid), 'utf8'), 'serverRid'),
    encodeNonZeroU64(descriptor.lifecycleGeneration, 'lifecycleGeneration'),
    encodeNonZeroU64(descriptor.descriptorRevision, 'descriptorRevision'),
    encodeU32(descriptor.weight),
    Buffer.of(runtimeStateToWire(descriptor.state)),
    encodeText8(descriptor.securityIdentity, 'securityIdentity'),
    encodeNonZeroU32(
      normalizedEffectiveMaxMessageBytes,
      'normalizedEffectiveMaxMessageBytes'
    ),
    encodeText16(descriptor.endpoint, 'advertisedEndpoint')
  );
  return encodeAdmission(command, ROLE_SERVER, roleBody);
}

function encodeAdmission(command: number, role: number, roleBody: Buffer): Buffer {
  if (roleBody.byteLength > 0xffff) fail('ClientServer role body is oversized.');
  const admission = concat(Buffer.of(role), encodeU16(roleBody.byteLength), roleBody);
  const result = concat(
    prefix(command),
    Buffer.of(TOPOLOGY_CLIENT_SERVER),
    encodeU32(admission.byteLength),
    admission
  );
  if (result.byteLength > MAX_DESCRIPTOR_BYTES) fail('ClientServer admission is oversized.');
  return result;
}

function decodeClientHello(reader: Reader): ZLinkClientServerHello {
  const channelName = reader.text8('channelName');
  if (reader.u8('direction') !== DIRECTION_CLIENT_TO_SERVER) {
    fail('ClientServer direction is invalid.');
  }
  return {
    channelName,
    securityIdentity: reader.text8('securityIdentity'),
    normalizedEffectiveMaxMessageBytes: reader.nonZeroU32(
      'normalizedEffectiveMaxMessageBytes'
    )
  };
}

function decodeServerAdmission(reader: Reader): ZLinkClientServerAdmission {
  const channelName = reader.text8('channelName');
  if (reader.u8('direction') !== DIRECTION_CLIENT_TO_SERVER) {
    fail('ClientServer direction is invalid.');
  }
  const serverRid = reader.bytes8('serverRid').toString('utf8');
  if (Buffer.byteLength(serverRid, 'utf8') === 0 || serverRid.includes('\0')) {
    fail('ClientServer serverRid is invalid.');
  }
  const lifecycleGeneration = reader.nonZeroU64('lifecycleGeneration');
  const descriptorRevision = reader.nonZeroU64('descriptorRevision');
  const weight = reader.u32('weight');
  if (weight > 10_000) fail('ClientServer weight is invalid.');
  return {
    channelName,
    serverRid,
    lifecycleGeneration,
    descriptorRevision,
    weight,
    state: runtimeStateFromWire(reader.u8('runtimeState')),
    securityIdentity: reader.text8('securityIdentity'),
    normalizedEffectiveMaxMessageBytes: reader.nonZeroU32(
      'normalizedEffectiveMaxMessageBytes'
    ),
    advertisedEndpoint: reader.text16('advertisedEndpoint')
  };
}

function prefix(command: number): Buffer {
  return Buffer.from([MAGIC_0, MAGIC_1, WIRE_MAJOR, command, 0]);
}

function encodeU16(value: number): Buffer {
  const result = Buffer.alloc(2);
  result.writeUInt16BE(value);
  return result;
}

function encodeU32(value: number): Buffer {
  if (!Number.isInteger(value) || value < 0 || value > MAX_U32) fail('Value exceeds u32.');
  const result = Buffer.alloc(4);
  result.writeUInt32BE(value);
  return result;
}

function encodeNonZeroU32(value: number, field: string): Buffer {
  if (!Number.isInteger(value) || value < 1 || value > MAX_U32) {
    fail(`${field} must be a non-zero u32.`);
  }
  return encodeU32(value);
}

function encodeNonZeroU64(value: bigint, field: string): Buffer {
  if (value < 1n || value > MAX_GENERATION) fail(`${field} must be a non-zero i63.`);
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function encodeText8(value: string, field: string): Buffer {
  return encodeBytes8(Buffer.from(value, 'utf8'), field, value);
}

function encodeBytes8(bytes: Buffer, field: string, text?: string): Buffer {
  if (bytes.byteLength < 1
    || bytes.byteLength > 0xff
    || (text !== undefined && text.includes('\0'))) {
    fail(`${field} must be bounded non-empty bytes without NUL.`);
  }
  return concat(Buffer.of(bytes.byteLength), bytes);
}

function encodeText16(value: string, field: string): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength < 1 || bytes.byteLength > 4096 || value.includes('\0')) {
    fail(`${field} must be bounded non-empty UTF-8 without NUL.`);
  }
  return concat(encodeU16(bytes.byteLength), bytes);
}

function runtimeStateToWire(value: ZLinkFrameworkRuntimeState): number {
  switch (value) {
    case ZLinkFrameworkRuntimeState.Preparing: return 0;
    case ZLinkFrameworkRuntimeState.Serving: return 1;
    case ZLinkFrameworkRuntimeState.Relocating:
    case ZLinkFrameworkRuntimeState.Relocated:
    case ZLinkFrameworkRuntimeState.Draining: return 2;
    case ZLinkFrameworkRuntimeState.Stopped: return 3;
    case ZLinkFrameworkRuntimeState.Error: return 4;
    default: fail('ClientServer runtime state is invalid.');
  }
}

function runtimeStateFromWire(value: number): ZLinkFrameworkRuntimeState {
  switch (value) {
    case 0: return ZLinkFrameworkRuntimeState.Preparing;
    case 1: return ZLinkFrameworkRuntimeState.Serving;
    case 2: return ZLinkFrameworkRuntimeState.Draining;
    case 3: return ZLinkFrameworkRuntimeState.Stopped;
    case 4: return ZLinkFrameworkRuntimeState.Error;
    default: fail('ClientServer runtime state is invalid.');
  }
}

function concat(...parts: readonly Uint8Array[]): Buffer {
  return Buffer.concat(parts.map(part => Buffer.from(part)));
}

function fail(message: string): never {
  throw new ZLinkClientServerServiceWireError(message);
}

class Reader {
  private readonly buffer: Buffer;
  private offset = 0;

  constructor(value: Uint8Array) {
    this.buffer = Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  }

  get remaining(): number {
    return this.buffer.byteLength - this.offset;
  }

  u8(field: string): number {
    this.require(1, field);
    return this.buffer[this.offset++]!;
  }

  u16(field: string): number {
    this.require(2, field);
    const value = this.buffer.readUInt16BE(this.offset);
    this.offset += 2;
    return value;
  }

  u32(field: string): number {
    this.require(4, field);
    const value = this.buffer.readUInt32BE(this.offset);
    this.offset += 4;
    return value;
  }

  nonZeroU32(field: string): number {
    const value = this.u32(field);
    if (value === 0) fail(`${field} must be non-zero.`);
    return value;
  }

  nonZeroU64(field: string): bigint {
    this.require(8, field);
    const value = this.buffer.readBigUInt64BE(this.offset);
    this.offset += 8;
    if (value < 1n || value > MAX_GENERATION) fail(`${field} is invalid.`);
    return value;
  }

  bytes(length: number, field: string): Buffer {
    this.require(length, field);
    const value = this.buffer.subarray(this.offset, this.offset + length);
    this.offset += length;
    return value;
  }

  bytes8(field: string): Buffer {
    const length = this.u8(`${field}.length`);
    if (length === 0) fail(`${field} must be non-empty.`);
    return this.bytes(length, field);
  }

  text8(field: string): string {
    return decodeText(this.bytes8(field), field);
  }

  text16(field: string): string {
    const length = this.u16(`${field}.length`);
    if (length < 1 || length > 4096) fail(`${field} length is invalid.`);
    return decodeText(this.bytes(length, field), field);
  }

  end(): void {
    if (this.remaining !== 0) fail('ClientServer control record has trailing bytes.');
  }

  private require(length: number, field: string): void {
    if (length < 0 || length > this.remaining) fail(`${field} is truncated.`);
  }
}

function decodeText(bytes: Buffer, field: string): string {
  const value = bytes.toString('utf8');
  if (value.includes('\0') || !Buffer.from(value, 'utf8').equals(bytes)) {
    fail(`${field} is not valid UTF-8 text.`);
  }
  return value;
}
