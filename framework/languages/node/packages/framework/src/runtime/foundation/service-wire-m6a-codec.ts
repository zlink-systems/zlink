import {
  validateDescriptor,
  type ServiceNodeDescriptor,
  type ServiceNodeState,
  type ServiceObjectRole
} from './service-topology-registry';
import {
  SERVICE_WIRE_MAGIC,
  SERVICE_WIRE_MAJOR,
  SERVICE_WIRE_REQUIRED_CAPABILITY,
  ServiceWireCommand
} from './service-wire-constants.generated';

const PREFIX_SIZE = 5;
const MAX_U32 = 0xffff_ffff;
const MAX_U64 = 0xffff_ffff_ffff_ffffn;
export const M6A_SERVICE_WIRE_MAGIC = SERVICE_WIRE_MAGIC;
export const M6A_SERVICE_WIRE_MAJOR = SERVICE_WIRE_MAJOR;
export const M6A_SERVICE_WIRE_REQUIRED_CAPABILITY = SERVICE_WIRE_REQUIRED_CAPABILITY;
export const M6aServiceWireCommand = ServiceWireCommand;

export interface ServiceApplicationPayload {
  readonly packetName: string;
  readonly contentType: string;
  readonly payload: Uint8Array;
}

export interface ServiceWireHeader {
  readonly command: number;
  readonly flags: number;
}

export interface ServiceReplyHeader {
  readonly correlation: bigint;
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly tail: Buffer;
}

export class ServiceWireProtocolError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ServiceWireProtocolError';
  }
}

export function encodeNodeSendHeader(): Buffer {
  return prefix(M6aServiceWireCommand.nodeSend);
}

export function encodeNodeRequestHeader(correlation: bigint): Buffer {
  return withU64(prefix(M6aServiceWireCommand.nodeRequest), correlation, 'correlation');
}

export function decodeNodeRequestHeader(frame: Uint8Array): bigint {
  const header = decodeHeader(frame);
  if (header.command !== M6aServiceWireCommand.nodeRequest || header.flags !== 0 || frame.byteLength !== 13) {
    fail('Invalid nodeRequest header.');
  }
  return readNonZeroU64(asBuffer(frame), PREFIX_SIZE, 'correlation');
}

export function encodeChannelSendHeader(channelName: string): Buffer {
  return concat(prefix(M6aServiceWireCommand.channelSend), encodeText8(channelName, 'channelName'));
}

export function decodeChannelSendHeader(frame: Uint8Array): string {
  const header = decodeHeader(frame);
  if (header.command !== M6aServiceWireCommand.channelSend || header.flags !== 0) {
    fail('Invalid channelSend header.');
  }
  const reader = new Reader(frame, PREFIX_SIZE);
  const channelName = reader.text8('channelName');
  reader.end();
  return channelName;
}

export function encodeChannelRequestHeader(correlation: bigint, channelName: string): Buffer {
  return concat(
    withU64(prefix(M6aServiceWireCommand.channelRequest), correlation, 'correlation'),
    encodeText8(channelName, 'channelName')
  );
}

export function decodeChannelRequestHeader(
  frame: Uint8Array
): { readonly correlation: bigint; readonly channelName: string } {
  const header = decodeHeader(frame);
  if (header.command !== M6aServiceWireCommand.channelRequest || header.flags !== 0) {
    fail('Invalid channelRequest header.');
  }
  const reader = new Reader(frame, PREFIX_SIZE);
  const correlation = reader.nonZeroU64('correlation');
  const channelName = reader.text8('channelName');
  reader.end();
  return { correlation, channelName };
}

export function encodeReplyHeader(
  correlation: bigint,
  terminalResult = 0,
  failureCode = 0,
  tail: Uint8Array = Buffer.alloc(0)
): Buffer {
  validateReplyFields(correlation, terminalResult, failureCode);
  if (tail.byteLength > 0xffff) fail('Reply tail exceeds u16.');
  const result = Buffer.alloc(PREFIX_SIZE + 18 + tail.byteLength);
  prefix(M6aServiceWireCommand.reply).copy(result);
  result.writeBigUInt64BE(correlation, PREFIX_SIZE);
  result.writeUInt32BE(terminalResult, PREFIX_SIZE + 8);
  result.writeUInt32BE(failureCode, PREFIX_SIZE + 12);
  result.writeUInt16BE(tail.byteLength, PREFIX_SIZE + 16);
  Buffer.from(tail).copy(result, PREFIX_SIZE + 18);
  return result;
}

export function decodeReplyHeader(frame: Uint8Array): ServiceReplyHeader {
  const header = decodeHeader(frame);
  if (header.command !== M6aServiceWireCommand.reply || header.flags !== 0 || frame.byteLength < 23) {
    fail('Invalid reply header.');
  }
  const bytes = asBuffer(frame);
  const tailLength = bytes.readUInt16BE(PREFIX_SIZE + 16);
  if (frame.byteLength !== PREFIX_SIZE + 18 + tailLength) {
    fail('Invalid reply tail.');
  }
  const result = {
    correlation: bytes.readBigUInt64BE(PREFIX_SIZE),
    terminalResult: bytes.readUInt32BE(PREFIX_SIZE + 8),
    failureCode: bytes.readUInt32BE(PREFIX_SIZE + 12),
    tail: Buffer.from(bytes.subarray(PREFIX_SIZE + 18))
  };
  validateReplyFields(result.correlation, result.terminalResult, result.failureCode);
  return result;
}

export function encodeApplicationPayload(payload: ServiceApplicationPayload): Buffer {
  const packetName = encodeText8(payload.packetName, 'packetName');
  const contentType = encodeText8(payload.contentType, 'contentType');
  if (payload.payload.byteLength > MAX_U32) fail('Application payload exceeds u32.');
  const bodyLength = packetName.byteLength
    + contentType.byteLength
    + 4
    + payload.payload.byteLength;
  if (bodyLength > MAX_U32) fail('Application payload body exceeds u32.');
  const result = Buffer.alloc(5 + bodyLength);
  result[0] = 1;
  result.writeUInt32BE(bodyLength, 1);
  let offset = 5;
  offset += packetName.copy(result, offset);
  offset += contentType.copy(result, offset);
  result.writeUInt32BE(payload.payload.byteLength, offset);
  offset += 4;
  Buffer.from(
    payload.payload.buffer,
    payload.payload.byteOffset,
    payload.payload.byteLength
  ).copy(result, offset);
  return result;
}

/**
 * Encodes the framework multipart payload directly into its M6A application
 * frame. The caller supplies borrowed message views; this function performs
 * the single ownership copy needed for the outbound frame.
 */
export function encodeMultipartApplicationPayload(
  parts: readonly Uint8Array[],
  packetName: string,
  contentType: string
): Buffer {
  if (parts.length === 0) fail('Multipart payload must contain at least one part.');
  if (parts.length > MAX_U32) fail('Multipart part count exceeds u32.');
  const packetNameBytes = encodeText8(packetName, 'packetName');
  const contentTypeBytes = encodeText8(contentType, 'contentType');
  let multipartLength = 4;
  for (const part of parts) {
    if (part.byteLength > MAX_U32) fail('Multipart part exceeds u32.');
    multipartLength += 4 + part.byteLength;
    if (multipartLength > MAX_U32) fail('Multipart payload exceeds u32.');
  }
  const bodyLength = packetNameBytes.byteLength
    + contentTypeBytes.byteLength
    + 4
    + multipartLength;
  if (bodyLength > MAX_U32) fail('Application payload body exceeds u32.');
  const result = Buffer.alloc(5 + bodyLength);
  result[0] = 1;
  result.writeUInt32BE(bodyLength, 1);
  let offset = 5;
  offset += packetNameBytes.copy(result, offset);
  offset += contentTypeBytes.copy(result, offset);
  result.writeUInt32BE(multipartLength, offset);
  offset += 4;
  result.writeUInt32BE(parts.length, offset);
  offset += 4;
  for (const part of parts) {
    result.writeUInt32BE(part.byteLength, offset);
    offset += 4;
    Buffer.from(part.buffer, part.byteOffset, part.byteLength).copy(result, offset);
    offset += part.byteLength;
  }
  return result;
}

export function decodeApplicationPayload(frame: Uint8Array): ServiceApplicationPayload {
  const decoded = decodeApplicationPayloadView(frame);
  return {
    ...decoded,
    payload: Buffer.from(decoded.payload)
  };
}

/**
 * Decodes only the M6A envelope fields and returns a view of its payload.
 * Callers use this after their transport or binding admission decision when
 * they must hand the multipart bytes to another runtime-owned queue.
 */
export function decodeApplicationPayloadView(frame: Uint8Array): ServiceApplicationPayload {
  const reader = new Reader(frame);
  if (reader.u8('version') !== 1) fail('Invalid application payload version.');
  const bodyLength = reader.u32('bodyLength');
  if (bodyLength !== reader.remaining) fail('Application payload body length mismatch.');
  const packetName = reader.text8('packetName');
  const contentType = reader.text8('contentType');
  const payloadLength = reader.u32('payloadLength');
  if (payloadLength !== reader.remaining) fail('Application payload length mismatch.');
  const payload = reader.view(payloadLength, 'payload');
  reader.end();
  return { packetName, contentType, payload };
}

/**
 * Validates the M6A envelope without retaining a second payload buffer. The
 * stateful activation path uses this before storing a durable request; the
 * application payload itself remains owned by the original frame until the
 * message is admitted to an execution queue.
 */
export function validateApplicationPayloadFrame(frame: Uint8Array): void {
  const reader = new Reader(frame);
  if (reader.u8('version') !== 1) fail('Invalid application payload version.');
  const bodyLength = reader.u32('bodyLength');
  if (bodyLength !== reader.remaining) fail('Application payload body length mismatch.');
  reader.text8('packetName');
  reader.text8('contentType');
  const payloadLength = reader.u32('payloadLength');
  reader.skip(payloadLength, 'payload');
  reader.end();
}

export function encodeRouteMeshAdmission(
  command: number,
  descriptor: ServiceNodeDescriptor
): Buffer {
  validateAdmissionCommand(command);
  validateDescriptor(descriptor);
  const routeParts: Buffer[] = [
    encodeText8(descriptor.meshName, 'meshName'),
    encodeText8(descriptor.securityIdentity, 'securityIdentity'),
    encodeU32(descriptor.effectiveMaxMessageBytes),
    encodeU64(descriptor.lifecycleGeneration),
    encodeU64(descriptor.descriptorRevision),
    encodeText16(descriptor.advertisedEndpoint, 'advertisedEndpoint'),
    encodeU16(descriptor.channels.length)
  ];
  for (const channel of descriptor.channels) {
    routeParts.push(encodeText8(channel.name, 'channelName'), encodeU32(channel.weight));
  }
  const extension = concat(
    tlv(1, Buffer.of(stateToWire(descriptor.state))),
    tlv(2, encodeU64(descriptor.applicationVersion)),
    tlv(6, concat(
      encodeU16(descriptor.protocolCapabilities.length),
      ...descriptor.protocolCapabilities.map(value => encodeText8(value, 'protocolCapability'))
    )),
    tlv(7, Buffer.of(roleToWire(descriptor.objectRole))),
    tlv(8, encodeU32(descriptor.placementWeight)),
    tlv(9, encodeU32(descriptor.activeCapacityLimit)),
    tlv(10, encodeU32(descriptor.pendingCapacityLimit)),
    tlv(11, encodeU32(descriptor.activeCapacityUsed)),
    tlv(12, encodeU32(descriptor.pendingCapacityUsed))
  );
  const route = concat(...routeParts, encodeU32(extension.byteLength), extension);
  return concat(prefix(command), Buffer.of(1), encodeU32(route.byteLength), route);
}

export function decodeRouteMeshAdmission(
  frame: Uint8Array,
  expectedCommand: number,
  sourceRoutingId: string
): ServiceNodeDescriptor {
  validateAdmissionCommand(expectedCommand);
  const header = decodeHeader(frame);
  if (header.command !== expectedCommand || header.flags !== 0) fail('Unexpected admission header.');
  const reader = new Reader(frame, PREFIX_SIZE);
  if (reader.u8('topologyKind') !== 1) fail('Admission is not RouteMesh topology.');
  const routeLength = reader.u32('routeLength');
  if (routeLength !== reader.remaining) fail('RouteMesh admission length mismatch.');
  const meshName = reader.text8('meshName');
  const securityIdentity = reader.text8('securityIdentity');
  const effectiveMaxMessageBytes = reader.u32('effectiveMaxMessageBytes');
  const lifecycleGeneration = reader.nonZeroU64('lifecycleGeneration');
  const descriptorRevision = reader.nonZeroU64('descriptorRevision');
  const advertisedEndpoint = reader.text16('advertisedEndpoint');
  const channelCount = reader.u16('channelCount');
  const channels: Array<{ name: string; weight: number }> = [];
  for (let index = 0; index < channelCount; index++) {
    channels.push({ name: reader.text8('channelName'), weight: reader.u32('channelWeight') });
  }
  const extensionLength = reader.u32('extensionLength');
  const extensionEnd = reader.offset + extensionLength;
  if (extensionEnd !== frame.byteLength) fail('Descriptor extension length mismatch.');

  let state: ServiceNodeState | undefined;
  let applicationVersion: bigint | undefined;
  let protocolCapabilities: string[] | undefined;
  let objectRole: ServiceObjectRole | undefined;
  const capacities = new Map<number, number>();
  let previousId = 0;
  while (reader.offset < extensionEnd) {
    const id = reader.u8('extensionId');
    const length = reader.u32('extensionFieldLength');
    if (id <= previousId || length > extensionEnd - reader.offset) fail('Invalid descriptor TLV.');
    previousId = id;
    const value = new Reader(reader.bytes(length, 'extensionValue'));
    switch (id) {
      case 1:
        state = stateFromWire(value.u8('state'));
        break;
      case 2:
        applicationVersion = value.u64('applicationVersion');
        break;
      case 6: {
        const count = value.u16('capabilityCount');
        protocolCapabilities = [];
        for (let index = 0; index < count; index++) {
          protocolCapabilities.push(value.text8('protocolCapability'));
        }
        break;
      }
      case 7:
        objectRole = roleFromWire(value.u8('objectRole'));
        break;
      case 8:
      case 9:
      case 10:
      case 11:
      case 12:
        capacities.set(id, value.u32('capacity'));
        break;
      default:
        value.skipRemaining();
        break;
    }
    value.end();
  }
  reader.end();
  if (
    state === undefined
    || applicationVersion === undefined
    || protocolCapabilities === undefined
    || objectRole === undefined
    || ![8, 9, 10, 11, 12].every(id => capacities.has(id))
  ) {
    fail('Descriptor extension omits a required field.');
  }
  const descriptor: ServiceNodeDescriptor = {
    meshName,
    nodeRoutingId: sourceRoutingId,
    lifecycleGeneration,
    descriptorRevision,
    advertisedEndpoint,
    channels,
    state,
    securityIdentity,
    effectiveMaxMessageBytes,
    applicationVersion,
    protocolCapabilities,
    objectRole,
    placementWeight: capacities.get(8)!,
    activeCapacityLimit: capacities.get(9)!,
    pendingCapacityLimit: capacities.get(10)!,
    activeCapacityUsed: capacities.get(11)!,
    pendingCapacityUsed: capacities.get(12)!
  };
  validateDescriptor(descriptor);
  return descriptor;
}

export function encodeReject(reason: number): Buffer {
  if (!Number.isInteger(reason) || reason < 1 || reason > 12) fail('Invalid reject reason.');
  return concat(prefix(M6aServiceWireCommand.reject), encodeU32(reason));
}

export function decodeReject(frame: Uint8Array): number {
  const header = decodeHeader(frame);
  if (header.command !== M6aServiceWireCommand.reject || header.flags !== 0 || frame.byteLength !== 9) {
    fail('Invalid reject record.');
  }
  const reason = asBuffer(frame).readUInt32BE(PREFIX_SIZE);
  if (reason < 1 || reason > 12) fail('Invalid reject reason.');
  return reason;
}

export function decodeHeader(frame: Uint8Array): ServiceWireHeader {
  if (frame.byteLength < PREFIX_SIZE) fail('Truncated service wire prefix.');
  if (frame[0] !== M6A_SERVICE_WIRE_MAGIC[0] || frame[1] !== M6A_SERVICE_WIRE_MAGIC[1]) {
    fail('Invalid service wire magic.');
  }
  if (frame[2] !== M6A_SERVICE_WIRE_MAJOR) fail('Unsupported service wire major.');
  return { command: frame[3]!, flags: frame[4]! };
}

export function requiredServiceCapability(): string {
  return M6A_SERVICE_WIRE_REQUIRED_CAPABILITY;
}

function prefix(command: number): Buffer {
  return Buffer.from([
    M6A_SERVICE_WIRE_MAGIC[0],
    M6A_SERVICE_WIRE_MAGIC[1],
    M6A_SERVICE_WIRE_MAJOR,
    command,
    0
  ]);
}

function withU64(head: Buffer, value: bigint, field: string): Buffer {
  if (value <= 0n || value > MAX_U64) fail(`${field} must be a non-zero u64.`);
  return concat(head, encodeU64(value));
}

function encodeU16(value: number): Buffer {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff) fail('Value exceeds u16.');
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

function encodeU64(value: bigint): Buffer {
  if (value < 0n || value > MAX_U64) fail('Value exceeds u64.');
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function encodeText8(value: string, field: string): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength === 0 || bytes.byteLength > 0xff || value.includes('\0')) {
    fail(`${field} must be bounded non-empty UTF-8 without NUL.`);
  }
  return concat(Buffer.of(bytes.byteLength), bytes);
}

function encodeText16(value: string, field: string): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength === 0 || bytes.byteLength > 4096 || value.includes('\0')) {
    fail(`${field} must be bounded non-empty UTF-8 without NUL.`);
  }
  return concat(encodeU16(bytes.byteLength), bytes);
}

function tlv(id: number, value: Buffer): Buffer {
  return concat(Buffer.of(id), encodeU32(value.byteLength), value);
}

function concat(...parts: readonly Uint8Array[]): Buffer {
  return Buffer.concat(parts.map(part => Buffer.from(part)));
}

function readNonZeroU64(bytes: Buffer, offset: number, field: string): bigint {
  const result = bytes.readBigUInt64BE(offset);
  if (result === 0n) fail(`${field} must be non-zero.`);
  return result;
}

function asBuffer(value: Uint8Array): Buffer {
  return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
}

function validateAdmissionCommand(command: number): void {
  if (
    command !== M6aServiceWireCommand.hello
    && command !== M6aServiceWireCommand.admit
    && command !== M6aServiceWireCommand.update
  ) {
    fail('Command is not an admission record.');
  }
}

function validateReplyFields(correlation: bigint, terminal: number, failure: number): void {
  const typedFailure = terminal === 102 || (terminal >= 104 && terminal <= 107);
  const validFailure = (failure >= 0 && failure <= 22) || failure === 35;
  if (
    correlation <= 0n
    || correlation > MAX_U64
    || (terminal !== 0 && (terminal < 101 || terminal > 113))
    || !validFailure
    || (terminal === 0 && failure !== 0)
    || (typedFailure && failure === 0)
    || (!typedFailure && failure !== 0)
  ) {
    fail('Invalid reply terminal fields.');
  }
}

function stateToWire(value: ServiceNodeState): number {
  // The service descriptor uses the shared framework state values. `retiring`
  // is a host-internal transition and has the same remote admission meaning
  // as `draining`; the service wire does not expose a separate retiring value.
  switch (value) {
    case 'preparing': return 0;
    case 'serving': return 1;
    case 'retiring':
    case 'draining': return 2;
    case 'stopped': return 3;
    case 'error': return 4;
  }
}

function stateFromWire(value: number): ServiceNodeState {
  switch (value) {
    case 0: return 'preparing';
    case 1: return 'serving';
    case 2: return 'draining';
    case 3: return 'stopped';
    case 4: return 'error';
    default: fail('Invalid runtime state.');
  }
}

function roleToWire(value: ServiceObjectRole): number {
  return ['none', 'client', 'server'].indexOf(value);
}

function roleFromWire(value: number): ServiceObjectRole {
  if (!Number.isInteger(value) || value < 0 || value > 2) fail('Invalid object role.');
  return ['none', 'client', 'server'][value] as ServiceObjectRole;
}

class Reader {
  readonly buffer: Buffer;
  offset: number;

  constructor(frame: Uint8Array, offset = 0) {
    this.buffer = asBuffer(frame);
    this.offset = offset;
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
    const result = this.buffer.readUInt16BE(this.offset);
    this.offset += 2;
    return result;
  }

  u32(field: string): number {
    this.require(4, field);
    const result = this.buffer.readUInt32BE(this.offset);
    this.offset += 4;
    return result;
  }

  u64(field: string): bigint {
    this.require(8, field);
    const result = this.buffer.readBigUInt64BE(this.offset);
    this.offset += 8;
    return result;
  }

  nonZeroU64(field: string): bigint {
    const result = this.u64(field);
    if (result === 0n) fail(`${field} must be non-zero.`);
    return result;
  }

  text8(field: string): string {
    const length = this.u8(`${field}.length`);
    if (length === 0) fail(`${field} must be non-empty.`);
    return decodeText(this.bytes(length, field), field);
  }

  text16(field: string): string {
    const length = this.u16(`${field}.length`);
    if (length === 0 || length > 4096) fail(`${field} has invalid length.`);
    return decodeText(this.bytes(length, field), field);
  }

  bytes(length: number, field: string): Buffer {
    this.require(length, field);
    const result = Buffer.from(this.buffer.subarray(this.offset, this.offset + length));
    this.offset += length;
    return result;
  }

  skipRemaining(): void {
    this.offset = this.buffer.byteLength;
  }

  end(): void {
    if (this.remaining !== 0) fail('Trailing bytes are forbidden.');
  }

  skip(length: number, field: string): void {
    this.require(length, field);
    this.offset += length;
  }

  view(length: number, field: string): Buffer {
    this.require(length, field);
    const result = this.buffer.subarray(this.offset, this.offset + length);
    this.offset += length;
    return result;
  }

  private require(length: number, field: string): void {
    if (length < 0 || this.remaining < length) fail(`Truncated ${field}.`);
  }
}

function decodeText(bytes: Buffer, field: string): string {
  const value = bytes.toString('utf8');
  if (value.includes('\uFFFD') || value.includes('\0') || Buffer.from(value).compare(bytes) !== 0) {
    fail(`${field} is not canonical UTF-8.`);
  }
  return value;
}

function fail(message: string): never {
  throw new ServiceWireProtocolError(message);
}
