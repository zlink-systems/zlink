import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendMessageLike as MessageLike } from '../backend/runtime-values';
import { randomUUID } from 'node:crypto';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkEncodedPayload,
  type ZLinkMessageSerializer,
  type ZLinkFlowOrigin
} from '../../contracts';
import { borrowEncodedPayload } from '../../contracts/Common/encoded-payload-storage';
import {
  isZLinkMessage,
  readZLinkMessageDeclaredType
} from '../../contracts/Common/ZLinkMessage';
import { ZLinkConfigurationException } from '../configuration';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import {
  selectSerializerWithContentType
} from '../messaging/payload-codec';
import { isCanonicalCodecContentType } from '../../contracts/Configuration/CodecContentType';
import {
  parseFrameworkJsonV1,
  stringifyFrameworkJsonV1
} from '../messaging/framework-json-v1';
import { currentOrCreateFlow } from '../diagnostics/flow-context';
import { codecsForFrameworkPacket } from './channel-framework-packets';
import { readZLinkPacketJsonContract } from '../../contracts/Handlers/Attributes';
import type { ZLinkJsonSchema } from '../../contracts/Handlers/JsonContract';

export const ZLINK_CHANNEL_FORMAT_MARKER = 0xf2;

//  Shared default for the dominant no-metadata message; avoids a Map
//  allocation per outbound envelope.
const EMPTY_OUTBOUND_METADATA: ReadonlyMap<string, string> = new Map();

export const JSON_CONTENT_TYPE = 'application/json';
export const BINARY_CONTENT_TYPE = 'application/octet-stream';

export const enum ZLinkChannelMessageKind {
  Request = 1,
  Response = 2,
  Command = 3,
  Publish = 4,
  Error = 5
}

export interface ZLinkChannelEnvelopeHeader {
  readonly formatMarker: number;
  readonly kind: ZLinkChannelMessageKind;
  readonly channelName: string;
  readonly messageName: string;
  readonly contentType: string;
  readonly correlationId: string | null;
  readonly deadline: string | null;
  readonly topic: string | null;
  readonly errorCode?: string | null;
  readonly errorMessage?: string | null;
  readonly source?: string | null;
  readonly metadata: Readonly<Record<string, string>>;
  readonly flowId?: string;
  readonly flowOrigin?: ZLinkFlowOrigin;
}

export interface ZLinkChannelEnvelope {
  readonly packetName?: string;
  readonly payload: Buffer;
  readonly header: ZLinkChannelEnvelopeHeader;
}

export interface ZLinkChannelEnvelopeCodecRegistry {
  readonly serializers: ReadonlyMap<string, ZLinkMessageSerializer>;
}

export function newChannelCorrelationId(): string {
  return randomUUID().replaceAll('-', '');
}

export function encodeChannelEnvelopeParts(
  kind: ZLinkChannelMessageKind,
  channelName: string,
  packetName: string | undefined,
  payload: unknown,
  timeoutMs?: number,
  topic?: string,
  codecs?: ZLinkChannelEnvelopeCodecRegistry,
  correlationId?: string,
  createFlow = true,
  metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
): readonly MessageLike[] {
  const messageName = resolveFrameworkPacketName(payload, packetName, 'Channel');
  const encoded = encodePayload(payload, codecsForFrameworkPacket(messageName, codecs), messageName, 'payload');
  const flow = createFlow ? currentOrCreateFlow('Application') : undefined;
  const envelopeCorrelationId = correlationIdForOutboundKind(kind, correlationId);
  const header: ZLinkChannelEnvelopeHeader = {
    formatMarker: ZLINK_CHANNEL_FORMAT_MARKER,
    kind,
    channelName,
    messageName,
    contentType: encoded.contentType,
    correlationId: envelopeCorrelationId,
    deadline: timeoutMs === undefined ? null : new Date(Date.now() + timeoutMs).toISOString(),
    topic: topic ?? null,
    errorCode: null,
    errorMessage: null,
    metadata: applicationMetadataRecord(metadata),
    flowId: flow?.flowId,
    flowOrigin: flow?.flowOrigin
  };
  return [encodeChannelHeader(header), encoded.message];
}

/**
 * Encodes an outbound channel envelope with a deadline that was established
 * before synchronous payload encoding began. This is an internal runtime
 * entry point for operations that must not extend their original deadline
 * while the message is being serialized.
 */
export function encodeChannelEnvelopePartsAtDeadline(
  kind: ZLinkChannelMessageKind,
  channelName: string,
  packetName: string | undefined,
  payload: unknown,
  deadlineUnixMs: number,
  topic?: string,
  codecs?: ZLinkChannelEnvelopeCodecRegistry,
  correlationId?: string,
  createFlow = true,
  metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
): readonly MessageLike[] {
  if (!Number.isSafeInteger(deadlineUnixMs) || deadlineUnixMs <= 0) {
    throw new RangeError('deadlineUnixMs must be a positive safe integer.');
  }
  const messageName = resolveFrameworkPacketName(payload, packetName, 'Channel');
  const encoded = encodePayload(payload, codecsForFrameworkPacket(messageName, codecs), messageName, 'payload');
  const flow = createFlow ? currentOrCreateFlow('Application') : undefined;
  const envelopeCorrelationId = correlationIdForOutboundKind(kind, correlationId);
  const header: ZLinkChannelEnvelopeHeader = {
    formatMarker: ZLINK_CHANNEL_FORMAT_MARKER,
    kind,
    channelName,
    messageName,
    contentType: encoded.contentType,
    correlationId: envelopeCorrelationId,
    deadline: new Date(deadlineUnixMs).toISOString(),
    topic: topic ?? null,
    errorCode: null,
    errorMessage: null,
    metadata: applicationMetadataRecord(metadata),
    flowId: flow?.flowId,
    flowOrigin: flow?.flowOrigin
  };
  return [encodeChannelHeader(header), encoded.message];
}

export function encodeChannelPublishEnvelopeParts(
  channelName: string,
  topic: string,
  packetName: string | undefined,
  payload: unknown,
  codecs?: ZLinkChannelEnvelopeCodecRegistry,
  createFlow = true,
  metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA
): readonly MessageLike[] {
  const messageName = resolveFrameworkPacketName(payload, packetName, 'Channel');
  const encoded = encodePayload(payload, codecsForFrameworkPacket(messageName, codecs), messageName, 'payload');
  const flow = createFlow ? currentOrCreateFlow('Application') : undefined;
  const header: ZLinkChannelEnvelopeHeader = {
    formatMarker: ZLINK_CHANNEL_FORMAT_MARKER,
    kind: ZLinkChannelMessageKind.Publish,
    channelName,
    messageName,
    contentType: encoded.contentType,
    correlationId: null,
    deadline: null,
    topic,
    errorCode: null,
    errorMessage: null,
    metadata: applicationMetadataRecord(metadata),
    flowId: flow?.flowId,
    flowOrigin: flow?.flowOrigin
  };
  return [encodeChannelHeader(header), encoded.message];
}

export function encodeChannelReplyParts(
  request: ZLinkChannelEnvelopeHeader,
  payload: unknown,
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): readonly MessageLike[] {
  const encoded = encodePayload(
    payload ?? Buffer.alloc(0),
    codecsForFrameworkPacket(request.messageName, codecs),
    request.messageName,
    'reply'
  );
  const header: ZLinkChannelEnvelopeHeader = {
    formatMarker: ZLINK_CHANNEL_FORMAT_MARKER,
    kind: ZLinkChannelMessageKind.Response,
    channelName: request.channelName,
    messageName: request.messageName,
    contentType: encoded.contentType,
    correlationId: request.correlationId,
    deadline: null,
    topic: null,
    metadata: {},
    flowId: request.flowId,
    flowOrigin: request.flowOrigin
  };
  return [encodeChannelHeader(header), encoded.message];
}

export function encodeChannelErrorReplyParts(request: ZLinkChannelEnvelopeHeader, error: unknown): readonly MessageLike[] {
  const errorCode = error instanceof ZLinkFrameworkException
    ? String(error.kind)
    : error instanceof Error
      ? String(ZLinkFrameworkErrorKind.InternalFailure)
      : String(ZLinkFrameworkErrorKind.InternalFailure);
  const errorMessage = error instanceof Error ? error.message : String(error);
  const header: ZLinkChannelEnvelopeHeader = {
    formatMarker: ZLINK_CHANNEL_FORMAT_MARKER,
    kind: ZLinkChannelMessageKind.Error,
    channelName: request.channelName,
    messageName: request.messageName,
    contentType: JSON_CONTENT_TYPE,
    correlationId: request.correlationId,
    deadline: null,
    topic: null,
    errorCode,
    errorMessage,
    metadata: {},
    flowId: request.flowId,
    flowOrigin: request.flowOrigin
  };
  return [encodeChannelHeader(header), encodeJsonBytes(null)];
}

export function decodeChannelReply<TReply>(
  parts: readonly Message[],
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): TReply {
  const header = decodeChannelHeader(parts);
  if (header.kind === ZLinkChannelMessageKind.Error) {
    throw decodeChannelError(header);
  }
  if (header.kind !== ZLinkChannelMessageKind.Response) {
    throw new ZLinkConfigurationException(`Channel reply kind '${header.kind}' is not a response.`);
  }
  if (!isCanonicalCodecContentType(header.contentType)) {
    throw unsupportedChannelContentType(header.contentType);
  }
  const serializer = codecs?.serializers.get(header.contentType);
  if (
    serializer === undefined
    && header.contentType !== BINARY_CONTENT_TYPE
    && header.contentType !== JSON_CONTENT_TYPE
  ) {
    throw unsupportedChannelContentType(header.contentType);
  }
  if (parts.length < 2 || parts[1].data().length === 0) {
    return undefined as TReply;
  }
  if (header.contentType === BINARY_CONTENT_TYPE) {
    return Buffer.from(parts[1].data()) as TReply;
  }
  if (serializer !== undefined) {
    return serializer.deserialize<TReply>(ZLinkEncodedPayload.from(parts[1].data()), Object as never);
  }
  return parseWireJson(
    parts[1].data().toString(),
    readZLinkPacketJsonContract(header.messageName)?.reply
  ) as TReply;
}

function decodeChannelError(header: ZLinkChannelEnvelopeHeader): Error {
  const code = header.errorCode;
  if (code === undefined || code === null || code.trim().length === 0) {
    return new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'Channel Error reply does not contain a non-empty errorCode.'
    );
  }
  const message = header.errorMessage ?? 'ZLink channel request failed.';
  const publicKind = Number(code);
  if (Number.isInteger(publicKind) && publicKind >= 0 && publicKind <= 12) {
    return new ZLinkFrameworkException(publicKind as ZLinkFrameworkErrorKind, message);
  }
  return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.InternalFailure, message);
}

export function decodeChannelEnvelope(
  parts: readonly Message[],
  decodedHeader?: ZLinkChannelEnvelopeHeader,
  flowEnabled = true
): ZLinkChannelEnvelope {
  let header = decodedHeader ?? decodeChannelHeader(parts, flowEnabled);
  if (!flowEnabled && (header.flowId !== undefined || header.flowOrigin !== undefined)) {
    // Spec 27 §4: with tracing Off the processing point neither reads the
    // inbound flow fields into context nor copies them into the reply or any
    // forwarded message. Dropping them here keeps every downstream consumer
    // (dispatch fields, reply encoders, traces) naturally flow-free.
    header = { ...header, flowId: undefined, flowOrigin: undefined };
  }
  if (parts.length < 2) {
    throw new ZLinkConfigurationException('Channel envelope body part is missing.');
  }
  // The owning receive loop keeps the Message open until dispatch completes.
  // Borrow its Buffer so envelope admission does not copy the whole payload.
  return { header, packetName: header.messageName, payload: parts[1].data() };
}

export function decodeChannelPayload(
  envelope: ZLinkChannelEnvelope,
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): unknown {
  try {
    if (!isCanonicalCodecContentType(envelope.header.contentType)) {
      throw unsupportedChannelContentType(envelope.header.contentType);
    }
    const serializer = codecs?.serializers.get(envelope.header.contentType);
    if (serializer !== undefined) {
      return serializer.deserialize(ZLinkEncodedPayload.from(envelope.payload), Object as never);
    }
    if (envelope.header.contentType === BINARY_CONTENT_TYPE) {
      // The receive loop owns the Message for the duration of dispatch, so
      // the envelope's borrowed payload remains valid until this returns.
      return envelope.payload;
    }
    if (envelope.header.contentType === JSON_CONTENT_TYPE) {
      return parseWireJson(
        envelope.payload.toString(),
        schemaForInboundChannelEnvelope(envelope.header)
      );
    }
    throw unsupportedChannelContentType(envelope.header.contentType);
  } catch (error) {
    if (error instanceof ZLinkFrameworkException) {
      throw error;
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
      `PayloadDecodeFailed: failed to decode channel payload for '${envelope.header.channelName}:${envelope.header.messageName}'.`,
      error
    );
  }
}

function unsupportedChannelContentType(contentType: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
    `ProtocolError: unsupported channel content type '${contentType}'.`
  );
}

export function closeMessages(parts: readonly MessageLike[]): void {
  for (const part of parts) {
    if (typeof (part as { close?: unknown }).close === 'function') {
      (part as Message).close();
    }
  }
}

function encodePayload(
  value: unknown,
  codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
  packetName: string,
  contractPart: 'payload' | 'reply'
): {
  readonly contentType: string;
  readonly message: MessageLike;
} {
  if (isZLinkMessage(value) && value.isEncoded()) {
    const payload = value.toEncodedPayload();
    return {
      contentType: BINARY_CONTENT_TYPE,
      message: borrowEncodedPayload(payload) ?? payload.data()
    };
  }
  const declaredType = isZLinkMessage(value)
    ? readZLinkMessageDeclaredType(value)
    : undefined;
  if (isZLinkMessage(value)) value = value.decode();
  const selected = selectSerializerWithContentType(value, codecs, declaredType);
  if (selected !== undefined && !(Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value))) {
    const payload = selected.serializer.serialize(value);
    return {
      contentType: selected.contentType,
      message: borrowEncodedPayload(payload) ?? payload.data()
    };
  }
  return {
    contentType: contentTypeOf(value),
    message: toMessageLike(value, readZLinkPacketJsonContract(packetName)?.[contractPart])
  };
}

function toMessageLike(value: unknown, schema?: ZLinkJsonSchema): MessageLike {
  if (Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value)) {
    return value;
  }
  return encodeJsonBytes(value, schema);
}

function contentTypeOf(value: unknown): string {
  return Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value)
    ? BINARY_CONTENT_TYPE
    : JSON_CONTENT_TYPE;
}

function isMessage(value: unknown): value is Message {
  return typeof value === 'object' && value !== null && typeof (value as { data?: unknown }).data === 'function';
}

function encodeJsonBytes(value: unknown, schema?: ZLinkJsonSchema): Buffer {
  return Buffer.from(stringifyFrameworkJsonV1(value, schema));
}

function encodeChannelHeader(header: ZLinkChannelEnvelopeHeader): Buffer {
  //  Explicit construction: this runs for every outbound envelope, so avoid a
  //  per-message spread of the whole header.
  return encodeJsonBytes({
    formatMarker: header.formatMarker,
    kind: header.kind,
    channelName: header.channelName,
    messageName: header.messageName,
    contentType: header.contentType,
    correlationId: header.correlationId,
    deadline: header.deadline,
    topic: header.topic,
    errorCode: header.errorCode,
    errorMessage: header.errorMessage,
    source: header.source,
    metadata: header.metadata,
    flowId: header.flowId,
    flowOrigin: header.flowOrigin === undefined ? undefined : encodeFlowOrigin(header.flowOrigin)
  });
}

function encodeFlowOrigin(origin: ZLinkFlowOrigin): number {
  switch (origin) {
    case 'Inbound': return 1;
    case 'Timer': return 2;
    case 'Application': return 3;
    case 'Lifecycle': return 4;
  }
}

export function decodeChannelHeader(
  parts: readonly Message[],
  flowEnabled = true
): ZLinkChannelEnvelopeHeader {
  if (parts.length === 0) {
    throw new ZLinkConfigurationException('Channel envelope header part is missing.');
  }
  return validateChannelHeader(parseWireJson(parts[0].data().toString()), flowEnabled);
}

function parseWireJson(payload: string, schema?: ZLinkJsonSchema): unknown {
  return parseFrameworkJsonV1(payload, {
    rejectPropertyName: isPrototypeKey
  }, schema);
}

function schemaForInboundChannelEnvelope(header: ZLinkChannelEnvelopeHeader): ZLinkJsonSchema | undefined {
  const contract = readZLinkPacketJsonContract(header.messageName);
  return header.kind === ZLinkChannelMessageKind.Response ? contract?.reply : contract?.payload;
}

function validateChannelHeader(value: unknown, flowEnabled = true): ZLinkChannelEnvelopeHeader {
  if (!isRecord(value)) {
    throw new ZLinkConfigurationException('Channel envelope header must be a JSON object.');
  }
  const header = value as Record<string, unknown>;
  if (header.formatMarker !== ZLINK_CHANNEL_FORMAT_MARKER) {
    throw new ZLinkConfigurationException('Channel envelope format marker is invalid.');
  }
  const kind = requireChannelMessageKind(header.kind);
  const contentType = requireString(header.contentType, 'contentType');
  if (contentType.trim().length === 0) {
    throw new ZLinkConfigurationException('Channel envelope contentType must not be empty.');
  }
  // Spec 27 §4: flow fields are observation-only. With tracing Off the
  // processing point neither reads nor validates them; correlation_id below
  // stays mandatory at every level.
  const flowId = flowEnabled ? optionalFlowId(header.flowId) : undefined;
  const flowOrigin = flowEnabled ? optionalFlowOrigin(header.flowOrigin) : undefined;
  if ((flowId === undefined) !== (flowOrigin === undefined)) {
    throw new ZLinkConfigurationException('Channel envelope flowId and flowOrigin must both be present or absent.');
  }
  const correlationId = requireNullableString(header.correlationId, 'correlationId');
  validateCorrelationForKind(kind, correlationId);
  return {
    formatMarker: ZLINK_CHANNEL_FORMAT_MARKER,
    kind,
    channelName: requireString(header.channelName, 'channelName'),
    messageName: requireString(header.messageName, 'messageName'),
    contentType,
    correlationId,
    deadline: requireNullableString(header.deadline, 'deadline'),
    topic: requireNullableString(header.topic, 'topic'),
    errorCode: header.errorCode === undefined ? null : requireNullableString(header.errorCode, 'errorCode'),
    errorMessage: header.errorMessage === undefined ? null : requireNullableString(header.errorMessage, 'errorMessage'),
    source: header.source === undefined ? undefined : requireNullableString(header.source, 'source'),
    metadata: requireApplicationMetadata(header.metadata),
    flowId,
    flowOrigin
  };
}

function correlationIdForOutboundKind(
  kind: ZLinkChannelMessageKind,
  correlationId: string | null | undefined
): string | null {
  if (kind === ZLinkChannelMessageKind.Request) {
    const selected = correlationId ?? newChannelCorrelationId();
    validateCorrelationValue(selected);
    return selected;
  }
  if (kind === ZLinkChannelMessageKind.Command || kind === ZLinkChannelMessageKind.Publish) {
    if (correlationId !== undefined && correlationId !== null) {
      throw new ZLinkConfigurationException(
        `Channel ${kind === ZLinkChannelMessageKind.Publish ? 'publish' : 'send'} must not contain correlationId.`
      );
    }
    return null;
  }
  const selected = correlationId ?? null;
  validateCorrelationForKind(kind, selected);
  return selected;
}

function validateCorrelationForKind(
  kind: ZLinkChannelMessageKind,
  correlationId: string | null
): void {
  if (kind === ZLinkChannelMessageKind.Request
    || kind === ZLinkChannelMessageKind.Response
    || kind === ZLinkChannelMessageKind.Error) {
    if (correlationId === null) {
      throw new ZLinkConfigurationException(
        `Channel ${channelKindName(kind)} requires correlationId.`
      );
    }
    validateCorrelationValue(correlationId);
    return;
  }
  if (correlationId !== null) {
    throw new ZLinkConfigurationException(
      `Channel ${channelKindName(kind)} must not contain correlationId.`
    );
  }
}

function validateCorrelationValue(correlationId: string): void {
  const byteLength = Buffer.byteLength(correlationId, 'utf8');
  if (byteLength < 1 || byteLength > 64 || !/^[\x00-\x7f]*$/.test(correlationId)) {
    throw new ZLinkConfigurationException(
      'Channel envelope correlationId must be a non-empty ASCII value of at most 64 bytes.'
    );
  }
}

function channelKindName(kind: ZLinkChannelMessageKind): string {
  switch (kind) {
    case ZLinkChannelMessageKind.Request: return 'request';
    case ZLinkChannelMessageKind.Response: return 'response';
    case ZLinkChannelMessageKind.Command: return 'send';
    case ZLinkChannelMessageKind.Publish: return 'publish';
    case ZLinkChannelMessageKind.Error: return 'error';
  }
}

const EMPTY_APPLICATION_METADATA: Readonly<Record<string, string>> = Object.freeze({});

function applicationMetadataRecord(metadata: ReadonlyMap<string, string>): Readonly<Record<string, string>> {
  //  The common case carries no metadata; skip the record + JSON.stringify
  //  byte-limit walk entirely.
  if (metadata.size === 0) return EMPTY_APPLICATION_METADATA;
  const record: Record<string, string> = {};
  for (const [key, value] of metadata) {
    if (key.length === 0 || key.includes('\0') || value.includes('\0')) {
      throw new ZLinkConfigurationException(
        'Channel application metadata keys must be non-empty and keys and values must not contain NUL.'
      );
    }
    record[key] = value;
  }
  if (Buffer.byteLength(JSON.stringify(record), 'utf8') > 1024) {
    throw new ZLinkConfigurationException('Channel application metadata exceeds the 1024-byte limit.');
  }
  return Object.freeze(record);
}

function requireApplicationMetadata(value: unknown): Readonly<Record<string, string>> {
  if (value === undefined) {
    return Object.freeze({});
  }
  if (!isRecord(value)) {
    throw new ZLinkConfigurationException('Channel application metadata must be a JSON object.');
  }
  const metadata = new Map<string, string>();
  for (const [key, selectedValue] of Object.entries(value)) {
    metadata.set(key, requireString(selectedValue, `metadata.${key}`));
  }
  return applicationMetadataRecord(metadata);
}

function optionalFlowId(value: unknown): string | undefined {
  return value === undefined || value === null ? undefined : requireFlowId(value);
}

function optionalFlowOrigin(value: unknown): ZLinkFlowOrigin | undefined {
  return value === undefined || value === null ? undefined : requireFlowOrigin(value);
}

function requireFlowId(value: unknown): string {
  const flowId = requireString(value, 'flowId');
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(flowId)) {
    throw new ZLinkConfigurationException('Channel envelope flowId must be a lowercase UUIDv7.');
  }
  return flowId;
}

function requireFlowOrigin(value: unknown): ZLinkFlowOrigin {
  switch (value) {
    case 1: return 'Inbound';
    case 2: return 'Timer';
    case 3: return 'Application';
    case 4: return 'Lifecycle';
  }
  throw new ZLinkConfigurationException('Channel envelope flowOrigin is invalid.');
}

function requireChannelMessageKind(value: unknown): ZLinkChannelMessageKind {
  if (
    value === ZLinkChannelMessageKind.Request ||
    value === ZLinkChannelMessageKind.Response ||
    value === ZLinkChannelMessageKind.Command ||
    value === ZLinkChannelMessageKind.Publish ||
    value === ZLinkChannelMessageKind.Error
  ) {
    return value;
  }
  throw new ZLinkConfigurationException('Channel envelope kind is not supported.');
}

function requireString(value: unknown, fieldName: string): string {
  if (typeof value !== 'string') {
    throw new ZLinkConfigurationException(`Channel envelope ${fieldName} must be a string.`);
  }
  return value;
}

function requireNullableString(value: unknown, fieldName: string): string | null {
  if (value === null || typeof value === 'string') {
    return value;
  }
  throw new ZLinkConfigurationException(`Channel envelope ${fieldName} must be a string or null.`);
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function isPrototypeKey(key: string): boolean {
  return key === '__proto__' || key === 'constructor' || key === 'prototype';
}
