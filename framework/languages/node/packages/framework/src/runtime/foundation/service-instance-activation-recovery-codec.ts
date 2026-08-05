import { crc32c } from './service-relocation-runtime';
import {
  validateApplicationPayloadFrame
} from './service-wire-m6a-codec';
import type { ServiceInstanceActivationTarget } from './service-stateful-wire-codec';
import { validateServiceMetadataFrame } from './service-metadata-codec';

const MAGIC = Buffer.from([0x5a, 0x4c, 0x49, 0x41]);
const VERSION = 1;
const FLAGS = 0;
const MAX_ENCODED_BYTES = 1024 * 1024;
const FATAL_UTF8 = new TextDecoder('utf-8', { fatal: true });

export interface ServiceInstanceActivationRecoveryEnvelope {
  readonly target: ServiceInstanceActivationTarget;
  readonly targetMeshName: string;
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: bigint;
  readonly sourceSpotId?: string;
  readonly operationKind: 'send' | 'request';
  readonly operation: { readonly high: bigint; readonly low: bigint };
  readonly replyRouteId?: bigint;
  readonly deadlineUnixMs: bigint;
  readonly metadataFrame?: Uint8Array;
  readonly applicationPayloadFrame: Uint8Array;
}

export function encodeInstanceActivationRecoveryEnvelope(
  value: ServiceInstanceActivationRecoveryEnvelope
): Buffer {
  requireOperation(value);
  validateApplicationPayloadFrame(value.applicationPayloadFrame);
  const targetSpotId = textBytes(value.target.targetSpotId, 'targetSpotId');
  const stableType = textBytes(value.target.stableType, 'stableType');
  const targetMeshName = textBytes(value.targetMeshName, 'targetMeshName');
  const targetNodeRid = textBytes(value.target.targetNodeRid, 'targetNodeRid');
  const descriptorVersion = textBytes(value.target.descriptorVersion, 'targetDescriptorVersion');
  const sourceNodeRid = textBytes(value.sourceNodeRid, 'sourceNodeRid');
  const sourceSpotId = value.sourceSpotId === undefined
    ? undefined
    : textBytes(value.sourceSpotId, 'sourceSpotId');
  validatePositiveU64(value.target.targetNodeGeneration, 'targetNodeGeneration');
  validatePositiveU64(value.sourceNodeGeneration, 'sourceNodeGeneration');
  validateU64(value.operation.high, 'operation.high');
  validateU64(value.operation.low, 'operation.low');
  if (value.operationKind === 'request') {
    validatePositiveU64(value.replyRouteId!, 'replyRouteId');
  }
  validatePositiveU64(value.deadlineUnixMs, 'deadlineUnixMs');
  const metadataFrame = value.metadataFrame === undefined
    ? undefined
    : validateServiceMetadataFrame(value.metadataFrame);
  const bodyLength =
    text8Size(targetSpotId)
    + text8Size(stableType)
    + text8Size(targetMeshName)
    + text8Size(targetNodeRid)
    + 8
    + text8Size(descriptorVersion)
    + text8Size(sourceNodeRid)
    + 8
    + 1 + (sourceSpotId === undefined ? 0 : text8Size(sourceSpotId))
    + 1
    + 8
    + 8
    + (value.operationKind === 'request' ? 8 : 0)
    + 8
    + 1 + (metadataFrame?.byteLength ?? 0)
    + value.applicationPayloadFrame.byteLength;
  const envelopeLength = 4 + 1 + 2 + 4 + bodyLength;
  const encodedLength = envelopeLength + 4;
  if (encodedLength > MAX_ENCODED_BYTES) {
    throw new RangeError('Instance activation recovery envelope exceeds 1 MiB.');
  }
  const encoded = Buffer.allocUnsafe(encodedLength);
  let offset = 0;
  encoded.set(MAGIC, offset);
  offset += MAGIC.byteLength;
  encoded.writeUInt8(VERSION, offset);
  offset += 1;
  encoded.writeUInt16BE(FLAGS, offset);
  offset += 2;
  encoded.writeUInt32BE(bodyLength, offset);
  offset += 4;
  offset = writeText8(encoded, offset, targetSpotId);
  offset = writeText8(encoded, offset, stableType);
  offset = writeText8(encoded, offset, targetMeshName);
  offset = writeText8(encoded, offset, targetNodeRid);
  offset = writePositiveU64(encoded, offset, value.target.targetNodeGeneration);
  offset = writeText8(encoded, offset, descriptorVersion);
  offset = writeText8(encoded, offset, sourceNodeRid);
  offset = writePositiveU64(encoded, offset, value.sourceNodeGeneration);
  encoded.writeUInt8(sourceSpotId === undefined ? 0 : 1, offset);
  offset += 1;
  if (sourceSpotId !== undefined) offset = writeText8(encoded, offset, sourceSpotId);
  encoded.writeUInt8(value.operationKind === 'send' ? 1 : 2, offset);
  offset += 1;
  offset = writeU64(encoded, offset, value.operation.high);
  offset = writeU64(encoded, offset, value.operation.low);
  if (value.operationKind === 'request') {
    offset = writePositiveU64(encoded, offset, value.replyRouteId!);
  }
  offset = writePositiveU64(encoded, offset, value.deadlineUnixMs);
  encoded.writeUInt8(metadataFrame === undefined ? 0 : 1, offset);
  offset += 1;
  if (metadataFrame !== undefined) {
    encoded.set(metadataFrame, offset);
    offset += metadataFrame.byteLength;
  }
  encoded.set(value.applicationPayloadFrame, offset);
  offset += value.applicationPayloadFrame.byteLength;
  if (offset !== envelopeLength) {
    throw new Error('Instance activation recovery envelope length calculation is invalid.');
  }
  encoded.writeUInt32BE(crc32c(encoded.subarray(0, envelopeLength)), envelopeLength);
  return encoded;
}

export function decodeInstanceActivationRecoveryEnvelope(
  encoded: Uint8Array
): ServiceInstanceActivationRecoveryEnvelope {
  if (encoded.byteLength > MAX_ENCODED_BYTES) {
    throw new RangeError('Instance activation recovery envelope exceeds 1 MiB.');
  }
  const reader = new RecoveryReader(encoded);
  reader.expect(MAGIC);
  if (reader.u8() !== VERSION || reader.u16() !== FLAGS) {
    throw new TypeError('Instance activation recovery envelope version or flags are invalid.');
  }
  const body = reader.takeReader(reader.u32());
  const checksumOffset = reader.offset;
  const checksum = reader.u32();
  if (!reader.done || crc32c(reader.bytes.subarray(0, checksumOffset)) !== checksum) {
    throw new TypeError('Instance activation recovery envelope checksum is invalid.');
  }
  const targetSpotId = body.text8();
  const stableType = body.text8();
  const targetMeshName = body.text8();
  const targetNodeRid = body.text8();
  const targetNodeGeneration = body.nonZeroU64();
  const descriptorVersion = body.text8();
  const sourceNodeRid = body.text8();
  const sourceNodeGeneration = body.nonZeroU64();
  const sourceSpotId = body.optionalText8();
  const operationDiscriminator = body.u8();
  if (operationDiscriminator !== 1 && operationDiscriminator !== 2) {
    throw new TypeError('Instance activation recovery operation kind is invalid.');
  }
  const operation = { high: body.u64(), low: body.u64() };
  if (operation.high === 0n && operation.low === 0n) {
    throw new TypeError('Instance activation recovery operation identity is zero.');
  }
  const operationKind = operationDiscriminator === 1 ? 'send' : 'request';
  const replyRouteId = operationKind === 'request' ? body.nonZeroU64() : undefined;
  const deadlineUnixMs = body.nonZeroU64();
  const hasMetadata = body.u8();
  if (hasMetadata !== 0 && hasMetadata !== 1) {
    throw new TypeError('Instance activation recovery metadata flag is invalid.');
  }
  const metadataFrame = hasMetadata === 1
    ? body.metadataFrame()
    : undefined;
  const applicationPayloadFrame = body.takeRemaining();
  validateApplicationPayloadFrame(applicationPayloadFrame);
  if (!body.done) {
    throw new TypeError('Instance activation recovery envelope has trailing bytes.');
  }
  return {
    target: {
      targetSpotId,
      stableType,
      targetNodeRid,
      targetNodeGeneration,
      descriptorVersion
    },
    targetMeshName,
    sourceNodeRid,
    sourceNodeGeneration,
    ...(sourceSpotId === undefined ? {} : { sourceSpotId }),
    operationKind,
    operation,
    ...(replyRouteId === undefined ? {} : { replyRouteId }),
    deadlineUnixMs,
    ...(metadataFrame === undefined ? {} : { metadataFrame }),
    applicationPayloadFrame
  };
}

function requireOperation(value: ServiceInstanceActivationRecoveryEnvelope): void {
  if (value.operation.high === 0n && value.operation.low === 0n) {
    throw new TypeError('Instance activation recovery operation identity is zero.');
  }
  if (
    value.operationKind === 'request'
      ? value.replyRouteId === undefined || value.replyRouteId <= 0n
      : value.replyRouteId !== undefined
  ) {
    throw new TypeError('Instance activation recovery reply route does not match the operation.');
  }
}

function textBytes(value: string, name: string): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength < 1 || bytes.byteLength > 255 || bytes.includes(0)) {
    throw new RangeError(`${name} must contain 1..255 UTF-8 bytes without NUL.`);
  }
  return bytes;
}

function text8Size(value: Uint8Array): number {
  return 1 + value.byteLength;
}

function writeText8(target: Buffer, offset: number, value: Uint8Array): number {
  target.writeUInt8(value.byteLength, offset);
  target.set(value, offset + 1);
  return offset + 1 + value.byteLength;
}

function validatePositiveU64(value: bigint, name: string): void {
  if (value <= 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new RangeError(`${name} is out of range.`);
  }
}

function validateU64(value: bigint, name: string): void {
  if (value < 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new RangeError(`${name} is out of range.`);
  }
}

function writeU64(target: Buffer, offset: number, value: bigint): number {
  target.writeBigUInt64BE(value, offset);
  return offset + 8;
}

function writePositiveU64(target: Buffer, offset: number, value: bigint): number {
  target.writeBigUInt64BE(value, offset);
  return offset + 8;
}

class RecoveryReader {
  readonly bytes: Uint8Array;
  offset = 0;

  constructor(value: Uint8Array) {
    this.bytes = value;
  }

  get done(): boolean {
    return this.offset === this.bytes.byteLength;
  }

  expect(expected: Uint8Array): void {
    const actual = this.take(expected.byteLength);
    if (!Buffer.from(actual).equals(Buffer.from(expected))) {
      throw new TypeError('Instance activation recovery envelope magic is invalid.');
    }
  }

  u8(): number {
    return this.take(1)[0]!;
  }

  u16(): number {
    return Buffer.from(this.take(2)).readUInt16BE();
  }

  u32(): number {
    return Buffer.from(this.take(4)).readUInt32BE();
  }

  u64(): bigint {
    return Buffer.from(this.take(8)).readBigUInt64BE();
  }

  nonZeroU64(): bigint {
    const value = this.u64();
    if (value === 0n) throw new TypeError('Instance activation recovery field is zero.');
    return value;
  }

  text8(): string {
    const bytes = this.take(this.u8());
    if (bytes.byteLength === 0 || bytes.includes(0)) {
      throw new TypeError('Instance activation recovery text is invalid.');
    }
    return FATAL_UTF8.decode(bytes);
  }

  optionalText8(): string | undefined {
    const present = this.u8();
    if (present === 0) return undefined;
    if (present !== 1) throw new TypeError('Instance activation recovery optional flag is invalid.');
    return this.text8();
  }

  metadataFrame(): Buffer {
    const start = this.offset;
    if (this.u8() !== 1) {
      throw new TypeError('Application metadata frame version is invalid.');
    }
    const count = this.u8();
    for (let index = 0; index < count; index += 1) {
      this.take(this.u8());
      this.take(this.u16());
    }
    return validateServiceMetadataFrame(this.bytes.subarray(start, this.offset));
  }

  takeRemaining(): Uint8Array {
    return this.take(this.bytes.byteLength - this.offset);
  }

  takeReader(length: number): RecoveryReader {
    return new RecoveryReader(this.take(length));
  }

  private take(length: number): Uint8Array {
    if (length < 0 || this.offset + length > this.bytes.byteLength) {
      throw new TypeError('Instance activation recovery envelope is truncated.');
    }
    const value = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return value;
  }
}
