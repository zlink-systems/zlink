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
import { ZLinkConfigurationException } from '../configuration';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import {
  contentTypeForSerializer,
  selectSerializer
} from '../messaging/payload-codec';
import { currentOrCreateFlow } from '../diagnostics/flow-context';
import { codecsForFrameworkPacket } from './channel-framework-packets';

export const ZLINK_CHANNEL_FORMAT_MARKER = 0xf2;

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
  metadata: ReadonlyMap<string, string> = new Map()
): readonly MessageLike[] {
  const messageName = resolveFrameworkPacketName(payload, packetName, 'Channel');
  const encoded = encodePayload(payload, codecsForFrameworkPacket(messageName, codecs));
  const flow = currentOrCreateFlow('Application', createFlow);
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
    ...(flow ?? {})
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
  metadata: ReadonlyMap<string, string> = new Map()
): readonly MessageLike[] {
  if (!Number.isSafeInteger(deadlineUnixMs) || deadlineUnixMs <= 0) {
    throw new RangeError('deadlineUnixMs must be a positive safe integer.');
  }
  const messageName = resolveFrameworkPacketName(payload, packetName, 'Channel');
  const encoded = encodePayload(payload, codecsForFrameworkPacket(messageName, codecs));
  const flow = currentOrCreateFlow('Application', createFlow);
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
    ...(flow ?? {})
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
  metadata: ReadonlyMap<string, string> = new Map()
): readonly MessageLike[] {
  const messageName = resolveFrameworkPacketName(payload, packetName, 'Channel');
  const encoded = encodePayload(payload, codecsForFrameworkPacket(messageName, codecs));
  const flow = currentOrCreateFlow('Application', createFlow);
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
    ...(flow ?? {})
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
    codecsForFrameworkPacket(request.messageName, codecs)
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
  const serializer = codecs?.serializers.get(header.contentType);
  if (
    serializer === undefined
    && header.contentType !== BINARY_CONTENT_TYPE
    && header.contentType !== JSON_CONTENT_TYPE
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
      `ProtocolError: unsupported channel content type '${header.contentType}'.`
    );
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
  return parseWireJson(parts[1].data().toString()) as TReply;
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
  decodedHeader?: ZLinkChannelEnvelopeHeader
): ZLinkChannelEnvelope {
  const header = decodedHeader ?? decodeChannelHeader(parts);
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
      return parseWireJson(envelope.payload.toString());
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
      `ProtocolError: unsupported channel content type '${envelope.header.contentType}'.`
    );
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

export function closeMessages(parts: readonly MessageLike[]): void {
  for (const part of parts) {
    if (typeof (part as { close?: unknown }).close === 'function') {
      (part as Message).close();
    }
  }
}

function encodePayload(
  value: unknown,
  codecs: ZLinkChannelEnvelopeCodecRegistry | undefined
): {
  readonly contentType: string;
  readonly message: MessageLike;
} {
  const serializer = selectSerializer(value, codecs);
  if (serializer !== undefined && !(Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value))) {
    const contentType = requireDefaultSerializerContentType(codecs, serializer);
    const payload = serializer.serialize(value);
    return {
      contentType,
      message: borrowEncodedPayload(payload) ?? payload.data()
    };
  }
  return { contentType: contentTypeOf(value), message: toMessageLike(value) };
}

function toMessageLike(value: unknown): MessageLike {
  if (Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value)) {
    return value;
  }
  return encodeJsonBytes(value);
}

function contentTypeOf(value: unknown): string {
  return Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value)
    ? BINARY_CONTENT_TYPE
    : JSON_CONTENT_TYPE;
}

function isMessage(value: unknown): value is Message {
  return typeof value === 'object' && value !== null && typeof (value as { data?: unknown }).data === 'function';
}

function encodeJsonBytes(value: unknown): Buffer {
  return Buffer.from(JSON.stringify(value ?? null, (_key, item) =>
    typeof item === 'bigint' ? item.toString() : item
  ));
}

function encodeChannelHeader(header: ZLinkChannelEnvelopeHeader): Buffer {
  return encodeJsonBytes({
    ...header,
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

export function decodeChannelHeader(parts: readonly Message[]): ZLinkChannelEnvelopeHeader {
  if (parts.length === 0) {
    throw new ZLinkConfigurationException('Channel envelope header part is missing.');
  }
  return validateChannelHeader(parseWireJson(parts[0].data().toString()));
}

function requireDefaultSerializerContentType(
  codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
  serializer: ZLinkMessageSerializer
): string {
  const contentType = contentTypeForSerializer(serializer, codecs);
  if (contentType === undefined) {
    throw new ZLinkConfigurationException(
      'Channel payload serializer is not registered under a content type.'
    );
  }
  return contentType;
}

function parseWireJson(payload: string): unknown {
  return JSON.parse(payload, (key, value) => {
    if (isPrototypeKey(key)) {
      throw new ZLinkConfigurationException(`Channel JSON key '${key}' is not allowed.`);
    }
    return value;
  });
}

function validateChannelHeader(value: unknown): ZLinkChannelEnvelopeHeader {
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
  const flowId = optionalFlowId(header.flowId);
  const flowOrigin = optionalFlowOrigin(header.flowOrigin);
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

function applicationMetadataRecord(metadata: ReadonlyMap<string, string>): Readonly<Record<string, string>> {
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
