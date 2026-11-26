import { metadataExceedsLimit } from '../foundation/service-metadata-codec';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendMessageLike as MessageLike } from '../backend/runtime-values';
import { randomBytes } from 'node:crypto';
import * as protobuf from 'protobufjs/minimal';
import {
  type Type,
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkMessageSerializer,
  type ZLinkFlowOrigin
} from '../../contracts';
import { borrowEncodedPayload } from '../../contracts/Common/encoded-payload-storage';
import {
  isZLinkMessage,
  materializeZLinkMessageValue,
  readZLinkMessageDeclaredType
} from '../../contracts/Common/ZLinkMessage';
import { ZLinkConfigurationException } from '../configuration';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import {
  encodedPayloadFromOwned,
  selectSerializerWithContentType
} from '../messaging/payload-codec';
import { isCanonicalCodecContentType } from '../../contracts/Configuration/CodecContentType';
import { parseFrameworkJsonV1, stringifyFrameworkJsonV1 } from '../messaging/framework-json-v1';
import { currentOrCreateFlow } from '../diagnostics/flow-context';
import { encodeChannelMultipartApplicationPayload } from '../foundation/service-wire-m6a-codec';
import { SERVICE_WIRE_METADATA_BYTES } from '../foundation/service-wire-constants.generated';
import { zlink as ChannelHeaderProto } from './generated/channel-header-v1';
import { formatDeadlineText } from './channel-deadline-text';
import { ChannelHeaderPrefixes } from './channel-header-prefix';
import { codecsForFrameworkPacket } from './channel-framework-packets';
import { readZLinkPacketJsonContract } from '../../contracts/Handlers/Attributes';
import type {
  ZLinkJsonSchema,
  ZLinkPacketJsonContract
} from '../../contracts/Handlers/JsonContract';
import {
  resolveMessageTypeForDecode,
  type ZLinkHandlerMessageType
} from '../../contracts/Common/HandlerMessageType';

interface ChannelHeaderWriter {
  readonly buf: Uint8Array;
  readonly pos: number;
  finish(): Uint8Array;
  reset(): void;
  raw(value: Uint8Array): unknown;
}

const CHANNEL_HEADER_CODEC = (
  ChannelHeaderProto as unknown as {
    framework: {
      channel: {
        v1: {
          ChannelHeader: {
            create(): Record<string, unknown>;
            encode(
              value: unknown,
              writer?: ChannelHeaderWriter
            ): ChannelHeaderWriter & { len: number };
            decode(
              value: unknown,
              length?: number,
              end?: number,
              depth?: number,
              target?: unknown
            ): unknown;
          };
        };
      };
    };
  }
).framework.channel.v1.ChannelHeader;
// These codec calls are synchronous, so one process-local scratch set can be reused safely.
const CHANNEL_HEADER_WRITER = CHANNEL_HEADER_CODEC.encode({});
const CHANNEL_HEADER_READER = protobuf.Reader.create(Buffer.alloc(0));
const CHANNEL_HEADER_TARGET = CHANNEL_HEADER_CODEC.create();
// The four leading header fields repeat across messages; their bytes still come
// from the generated encoder, see channel-header-prefix.ts.
const CHANNEL_HEADER_PREFIXES = new ChannelHeaderPrefixes(
  (kind, channelName, messageName, contentType) =>
    Buffer.from(
      CHANNEL_HEADER_CODEC.encode({ kind, channelName, messageName, contentType }).finish()
    )
);
const EMPTY_PROTOBUF_METADATA: readonly Readonly<{ key: string; value: string }>[] = Object.freeze(
  []
);

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

const CHANNEL_CORRELATION_TEXT_LENGTH = 32;
const PREPARED_CHANNEL_CORRELATION = '0'.repeat(CHANNEL_CORRELATION_TEXT_LENGTH);
const CHANNEL_FLOW_TEXT = /^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

// A per-process random 64-bit prefix followed by a per-process counter, both
// as lowercase hex: unique among the runtime's pending requests (spec 06-obs/04
// §3) without drawing random bytes for every request — the shape Java
// ZLinkServiceOperationIds uses.
const CHANNEL_CORRELATION_PREFIX = randomBytes(CHANNEL_CORRELATION_TEXT_LENGTH / 4).toString('hex');
let channelCorrelationCounter = 0;

export function newChannelCorrelationId(): string {
  channelCorrelationCounter += 1;
  const suffix = channelCorrelationCounter.toString(16);
  return (
    CHANNEL_CORRELATION_PREFIX +
    PREPARED_CHANNEL_CORRELATION.slice(suffix.length, CHANNEL_CORRELATION_TEXT_LENGTH / 2) +
    suffix
  );
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
  metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA,
  forService = false
): readonly MessageLike[] {
  return encodeChannelEnvelopePartsAtDeadline(
    kind,
    channelName,
    packetName,
    payload,
    timeoutMs === undefined ? undefined : Date.now() + timeoutMs,
    topic,
    codecs,
    correlationId,
    createFlow,
    metadata,
    forService
  );
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
  deadlineUnixMs: number | undefined,
  topic?: string,
  codecs?: ZLinkChannelEnvelopeCodecRegistry,
  correlationId?: string,
  createFlow = true,
  metadata: ReadonlyMap<string, string> = EMPTY_OUTBOUND_METADATA,
  forService = false
): readonly MessageLike[] {
  if (
    deadlineUnixMs !== undefined &&
    (!Number.isSafeInteger(deadlineUnixMs) || deadlineUnixMs <= 0)
  ) {
    throw new RangeError('deadlineUnixMs must be a positive safe integer.');
  }
  const messageName = resolveFrameworkPacketName(payload, packetName, 'Channel');
  return encodePayload(
    payload,
    codecsForFrameworkPacket(messageName, codecs),
    messageName,
    'payload',
    kind,
    channelName,
    correlationId,
    deadlineUnixMs,
    topic,
    createFlow,
    metadata,
    forService
  );
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
  return encodePayload(
    payload,
    codecsForFrameworkPacket(messageName, codecs),
    messageName,
    'payload',
    ZLinkChannelMessageKind.Publish,
    channelName,
    undefined,
    undefined,
    topic,
    createFlow,
    metadata
  );
}

export function encodeChannelReplyParts(
  request: ZLinkChannelEnvelopeHeader,
  payload: unknown,
  codecs?: ZLinkChannelEnvelopeCodecRegistry,
  forService = false
): readonly MessageLike[] {
  return encodePayload(
    payload ?? Buffer.alloc(0),
    codecsForFrameworkPacket(request.messageName, codecs),
    request.messageName,
    'reply',
    ZLinkChannelMessageKind.Response,
    request.channelName,
    request.correlationId ?? undefined,
    undefined,
    undefined,
    false,
    EMPTY_OUTBOUND_METADATA,
    forService,
    request
  );
}

/**
 * Reply metadata marker for errors the framework generated itself (never for
 * application handler failures). Lets a requester distinguish a
 * framework-origin NOT_FOUND (e.g. stale route / missing handler) from an
 * application error that happens to use the same kind.
 */
export const ZLINK_FRAMEWORK_ORIGIN_METADATA_KEY = 'zlink.origin';
export const ZLINK_FRAMEWORK_ORIGIN_METADATA_VALUE = 'framework';

/**
 * Cross-language errorCode wire names, indexed by ZLinkFrameworkErrorKind.
 * Canonical table: C++ channel_reply_writer.cpp error_code_name(). Every
 * language encodes the snake_case name on the wire; numeric codes are a
 * process-local representation and never cross the wire.
 */
const ERROR_KIND_WIRE_NAMES: readonly string[] = [
  'not_found', // 0
  'already_exists', // 1
  'type_mismatch', // 2
  'not_configured', // 3
  'rejected', // 4
  'unavailable', // 5
  'deadline_exceeded', // 6
  'shutting_down', // 7
  'protocol_error', // 8
  'invalid_operation', // 9
  'data_lost', // 10
  'internal_failure' // 11
];

const ERROR_KIND_BY_WIRE_NAME: ReadonlyMap<string, ZLinkFrameworkErrorKind> = new Map(
  ERROR_KIND_WIRE_NAMES.map((name, kind) => [name, kind as ZLinkFrameworkErrorKind])
);

export function channelErrorKindFromCode(code: string): ZLinkFrameworkErrorKind | undefined {
  return ERROR_KIND_BY_WIRE_NAME.get(code);
}

export function channelErrorCodeName(kind: ZLinkFrameworkErrorKind): string {
  return ERROR_KIND_WIRE_NAMES[kind] ?? 'internal_failure';
}

/**
 * A channel error decoded from an Error reply envelope. `origin` carries the
 * header metadata `zlink.origin` value when the peer marked the error (for
 * example 'framework' for framework-generated stale-route errors). It is
 * exposed for consumers only; the transport-level stale-route decision does
 * not read it.
 */
export interface ZLinkChannelDecodedError extends Error {
  origin?: string;
}

export function encodeChannelErrorReplyParts(
  request: ZLinkChannelEnvelopeHeader,
  error: unknown,
  metadata: Readonly<Record<string, string>> = EMPTY_APPLICATION_METADATA,
  forService = false
): readonly MessageLike[] {
  const errorCode = channelErrorCodeName(
    error instanceof ZLinkFrameworkException ? error.kind : ZLinkFrameworkErrorKind.InternalFailure
  );
  const errorMessage = error instanceof Error ? error.message : String(error);
  return writeChannelParts(
    ZLinkChannelMessageKind.Error,
    request.channelName,
    request.messageName,
    JSON_CONTENT_TYPE,
    request.correlationId,
    null,
    null,
    errorCode,
    errorMessage,
    undefined,
    metadata,
    request.flowId,
    request.flowOrigin,
    encodeJsonText(null),
    forService
  );
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
    serializer === undefined &&
    header.contentType !== BINARY_CONTENT_TYPE &&
    header.contentType !== JSON_CONTENT_TYPE
  ) {
    throw unsupportedChannelContentType(header.contentType);
  }
  const body = parts.length < 2 ? undefined : parts[1].data();
  if (body === undefined || body.length === 0) {
    return undefined as TReply;
  }
  if (header.contentType === BINARY_CONTENT_TYPE) {
    return body as TReply;
  }
  if (serializer !== undefined) {
    return serializer.deserialize<TReply>(encodedPayloadFromOwned(body), Object as never);
  }
  return parseWireJson(
    body.toString(),
    readZLinkPacketJsonContract(header.messageName)?.reply
  ) as TReply;
}

function decodeChannelError(header: ZLinkChannelEnvelopeHeader): ZLinkChannelDecodedError {
  const code = header.errorCode;
  let failure: ZLinkChannelDecodedError;
  if (code === undefined || code === null || code.trim().length === 0) {
    failure = new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'Channel Error reply does not contain a non-empty errorCode.'
    );
  } else {
    const message = header.errorMessage ?? 'ZLink channel request failed.';
    //  Cross-language contract: errorCode carries the snake_case wire name.
    //  An unknown name degrades to InternalFailure, matching the C++
    //  request_failure_mapper fallback for unrecognized codes.
    const publicKind = channelErrorKindFromCode(code) ?? ZLinkFrameworkErrorKind.InternalFailure;
    failure = new ZLinkFrameworkException(publicKind, message);
  }
  //  Cross-language contract (.NET ZLinkErrorOriginWire.RemoteReplyOrigin /
  //  ZLinkEnvelopeErrorMapper): every error DECODED FROM A REMOTE REPLY is
  //  classified, never left unmarked. The zlink.origin=framework marker
  //  means the framework itself produced the error (stale route, sealed
  //  admission, dispatch rejection); its absence means a remote application
  //  handler produced it. Local/transport failures that never round-trip a
  //  reply (this function's only caller, decodeChannelReply, is only
  //  reached for bytes that DID come back over the wire) are unaffected and
  //  stay unclassified (`origin: undefined`, i.e. "unspecified") because
  //  they never call this function at all.
  failure.origin =
    header.metadata[ZLINK_FRAMEWORK_ORIGIN_METADATA_KEY] === ZLINK_FRAMEWORK_ORIGIN_METADATA_VALUE
      ? ZLINK_FRAMEWORK_ORIGIN_METADATA_VALUE
      : 'application';
  return failure;
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

export type ZLinkChannelPayloadDecoder = (envelope: ZLinkChannelEnvelope) => unknown;

export function prepareChannelPayloadDecoder(
  packetName: string,
  codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
  messageType: ZLinkHandlerMessageType | undefined
): ZLinkChannelPayloadDecoder {
  const contract = readZLinkPacketJsonContract(packetName);
  return (envelope) =>
    decodeChannelPayload(envelope, codecs, resolveMessageTypeForDecode(messageType), contract);
}

export function decodeChannelPayload(
  envelope: ZLinkChannelEnvelope,
  codecs?: ZLinkChannelEnvelopeCodecRegistry,
  messageType?: Type,
  contract?: ZLinkPacketJsonContract
): unknown {
  try {
    const { header, payload } = envelope;
    const { contentType } = header;
    if (
      contentType !== JSON_CONTENT_TYPE &&
      contentType !== BINARY_CONTENT_TYPE &&
      !isCanonicalCodecContentType(contentType)
    ) {
      throw unsupportedChannelContentType(contentType);
    }
    const serializer = codecs?.serializers.get(contentType);
    if (serializer !== undefined) {
      return serializer.deserialize(
        encodedPayloadFromOwned(payload),
        (messageType ?? Object) as never
      );
    }
    if (contentType === BINARY_CONTENT_TYPE) {
      // The receive loop owns the Message for the duration of dispatch.
      return payload;
    }
    if (contentType === JSON_CONTENT_TYPE) {
      return materializeZLinkMessageValue(
        parseWireJson(payload.toString(), schemaForInboundChannelEnvelope(header, contract)),
        messageType
      );
    }
    throw unsupportedChannelContentType(contentType);
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
  contractPart: 'payload' | 'reply',
  kind: ZLinkChannelMessageKind,
  channelName: string,
  correlationId: string | undefined,
  deadlineUnixMs: number | undefined,
  topic: string | undefined,
  createFlow: boolean,
  metadata: ReadonlyMap<string, string>,
  forService = false,
  request?: ZLinkChannelEnvelopeHeader
): readonly MessageLike[] {
  let declaredType;
  let contentType: string;
  let message: MessageLike;
  if (isZLinkMessage(value) && value.isEncoded()) {
    const payload = value.toEncodedPayload();
    contentType = BINARY_CONTENT_TYPE;
    message = borrowEncodedPayload(payload) ?? payload.data();
  } else {
    if (isZLinkMessage(value)) {
      declaredType = readZLinkMessageDeclaredType(value);
      value = value.decode();
    }
    const selected = selectSerializerWithContentType(value, codecs, declaredType);
    if (
      selected !== undefined &&
      !(Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value))
    ) {
      const payload = selected.serializer.serialize(value);
      contentType = selected.contentType;
      message = borrowEncodedPayload(payload) ?? payload.data();
    } else {
      contentType = contentTypeOf(value);
      message = toMessageLike(value, readZLinkPacketJsonContract(packetName)?.[contractPart]);
    }
  }
  const flow = request ?? (createFlow ? currentOrCreateFlow('Application') : undefined);
  return writeChannelParts(
    kind,
    channelName,
    packetName,
    contentType,
    request === undefined
      ? correlationIdForOutboundKind(kind, correlationId)
      : request.correlationId,
    deadlineUnixMs === undefined ? null : formatDeadlineText(deadlineUnixMs),
    topic ?? null,
    contractPart === 'reply' ? undefined : null,
    contractPart === 'reply' ? undefined : null,
    undefined,
    applicationMetadataRecord(metadata),
    flow?.flowId,
    flow?.flowOrigin,
    message,
    forService
  );
}

function toMessageLike(value: unknown, schema?: ZLinkJsonSchema): MessageLike {
  if (Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value)) {
    return value;
  }
  return encodeJsonText(value, schema);
}

function contentTypeOf(value: unknown): string {
  return Buffer.isBuffer(value) || value instanceof Uint8Array || isMessage(value)
    ? BINARY_CONTENT_TYPE
    : JSON_CONTENT_TYPE;
}

function isMessage(value: unknown): value is Message {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as { data?: unknown }).data === 'function'
  );
}

function encodeJsonText(value: unknown, schema?: ZLinkJsonSchema): string {
  return stringifyFrameworkJsonV1(value, schema);
}

function writeChannelParts(
  kind: ZLinkChannelMessageKind,
  channelName: string,
  messageName: string,
  contentType: string,
  correlationId: string | null,
  deadline: string | null,
  topic: string | null,
  errorCode: string | null | undefined,
  errorMessage: string | null | undefined,
  source: string | null | undefined,
  metadata: Readonly<Record<string, string>>,
  flowId: string | undefined,
  flowOrigin: ZLinkFlowOrigin | undefined,
  payload: MessageLike,
  forService: boolean
): readonly MessageLike[] {
  const sortedMetadata =
    metadata === EMPTY_APPLICATION_METADATA
      ? EMPTY_PROTOBUF_METADATA
      : Object.keys(metadata)
          .sort((left, right) => Buffer.compare(Buffer.from(left), Buffer.from(right)))
          .map((key) => ({ key, value: metadata[key]! }));
  const prefix = CHANNEL_HEADER_PREFIXES.forFields(kind, channelName, messageName, contentType);
  // OUTBOUND_CHANNEL_HEADER keeps kind, channelName, messageName and contentType at their
  // proto3 defaults, so the encoder skips them; the prefix bytes stand in for them.
  const payloadView = forService && !isMessage(payload) ? payload : undefined;
  const header = OUTBOUND_CHANNEL_HEADER;
  header.correlationId = correlationId;
  header.deadline = deadline;
  header.errorCode = errorCode ?? null;
  header.errorMessage = errorMessage ?? null;
  header.flowId = flowId ?? null;
  header.flowOrigin = flowOrigin === undefined ? null : encodeFlowOrigin(flowOrigin);
  header.source = source ?? null;
  header.topic = topic;
  header.metadata = sortedMetadata;
  if (sortedMetadata.length !== 0) {
    if (metadataExceedsLimit(sortedMetadata)) {
      throw new ZLinkConfigurationException(
        `Channel application metadata exceeds the ${SERVICE_WIRE_METADATA_BYTES}-byte limit.`
      );
    }
  }
  try {
    CHANNEL_HEADER_WRITER.raw(prefix.bytes);
    const writer = CHANNEL_HEADER_CODEC.encode(header, CHANNEL_HEADER_WRITER);
    if (payloadView !== undefined) {
      return encodeChannelMultipartApplicationPayload(writer.buf, writer.pos, payloadView);
    }
    const bytes = writer.finish();
    return [Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes), payload];
  } finally {
    CHANNEL_HEADER_WRITER.reset();
  }
}

const OUTBOUND_CHANNEL_HEADER: Record<string, unknown> = {
  kind: 0,
  channelName: '',
  messageName: '',
  contentType: '',
  correlationId: null,
  deadline: null,
  errorCode: null,
  errorMessage: null,
  flowId: null,
  flowOrigin: null,
  source: null,
  topic: null,
  metadata: EMPTY_PROTOBUF_METADATA
};

function encodeFlowOrigin(origin: ZLinkFlowOrigin): number {
  switch (origin) {
    case 'Inbound':
      return 1;
    case 'Timer':
      return 2;
    case 'Application':
      return 3;
    case 'Lifecycle':
      return 4;
  }
}

/** Parses the sole channel header wire format; semantic checks follow in Framework validation. */
export function decodeChannelHeaderRecord(payload: Buffer): Record<string, unknown> {
  const decoded = decodeChannelHeaderRecordIntoScratch(payload);
  return {
    ...decoded,
    metadata: Array.isArray(decoded.metadata) ? decoded.metadata.slice() : decoded.metadata
  };
}

function decodeChannelHeaderRecordIntoScratch(payload: Buffer): Record<string, unknown> {
  try {
    const target = CHANNEL_HEADER_TARGET;
    // A header that starts with a known prefix already has its first four fields decoded.
    const known = CHANNEL_HEADER_PREFIXES.matching(payload);
    target.kind = known === undefined ? 0 : known.kind;
    target.channelName = known === undefined ? '' : known.channelName;
    target.messageName = known === undefined ? '' : known.messageName;
    target.contentType = known === undefined ? '' : known.contentType;
    target.correlationId = null;
    target.deadline = null;
    target.errorCode = null;
    target.errorMessage = null;
    target.flowId = null;
    target.flowOrigin = null;
    target.source = null;
    target.topic = null;
    target.$unknowns = undefined;
    const metadata = target.metadata;
    if (Array.isArray(metadata)) metadata.length = 0;
    CHANNEL_HEADER_READER.buf = payload;
    CHANNEL_HEADER_READER.pos = known === undefined ? 0 : known.bytes.length;
    CHANNEL_HEADER_READER.len = payload.length;
    const decoded = CHANNEL_HEADER_CODEC.decode(
      CHANNEL_HEADER_READER,
      undefined,
      undefined,
      undefined,
      target
    ) as Record<string, unknown>;
    if (known === undefined) {
      CHANNEL_HEADER_PREFIXES.learn(
        payload,
        decoded.kind,
        decoded.channelName,
        decoded.messageName,
        decoded.contentType
      );
    }
    return decoded;
  } catch (error) {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'Channel envelope header protobuf is malformed.',
      { cause: error }
    );
  }
}

const CHANNEL_JSON_PARSE_OPTIONS = Object.freeze({ rejectPropertyName: isPrototypeKey });

function parseWireJson(payload: string, schema?: ZLinkJsonSchema): unknown {
  return parseFrameworkJsonV1(payload, CHANNEL_JSON_PARSE_OPTIONS, schema);
}

function isPrototypeKey(key: string): boolean {
  return key === '__proto__' || key === 'constructor' || key === 'prototype';
}

export function decodeChannelHeader(
  parts: readonly Message[],
  flowEnabled = true
): ZLinkChannelEnvelopeHeader {
  if (parts.length === 0) {
    throw new ZLinkConfigurationException('Channel envelope header part is missing.');
  }
  const decoded = decodeChannelHeaderRecordIntoScratch(parts[0].data());
  try {
    return validateChannelHeader(decoded, flowEnabled);
  } catch (error) {
    if (
      error instanceof ZLinkFrameworkException &&
      error.kind === ZLinkFrameworkErrorKind.ProtocolError
    ) {
      throw error;
    }
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      error instanceof Error ? error.message : 'Channel envelope header contains invalid values.',
      error
    );
  }
}

function schemaForInboundChannelEnvelope(
  header: ZLinkChannelEnvelopeHeader,
  contract = readZLinkPacketJsonContract(header.messageName)
): ZLinkJsonSchema | undefined {
  return header.kind === ZLinkChannelMessageKind.Response ? contract?.reply : contract?.payload;
}

function validateChannelHeader(value: unknown, flowEnabled = true): ZLinkChannelEnvelopeHeader {
  if (!isRecord(value)) {
    throw new ZLinkConfigurationException('Channel envelope header must be a protobuf message.');
  }
  const raw = value as Record<string, unknown>;
  const kind = requireChannelMessageKind(raw.kind);
  const contentType = requireDecodedString(raw.contentType, 'contentType');
  const flowId = flowEnabled ? optionalFlowId(raw.flowId) : undefined;
  const flowOrigin = flowEnabled ? optionalFlowOrigin(raw.flowOrigin) : undefined;
  if ((flowId === undefined) !== (flowOrigin === undefined)) {
    throw new ZLinkConfigurationException(
      'Channel envelope flowId and flowOrigin must both be present or absent.'
    );
  }
  const correlationId =
    raw.correlationId === undefined
      ? null
      : requireDecodedNullableString(raw.correlationId, 'correlationId');
  if (correlationId !== null) validateCorrelationValue(correlationId);
  const channelName = requireDecodedString(raw.channelName, 'channelName');
  const messageName = requireDecodedString(raw.messageName, 'messageName');
  const deadline =
    raw.deadline === undefined ? null : requireDecodedNullableString(raw.deadline, 'deadline');
  const topic = raw.topic === undefined ? null : requireDecodedNullableString(raw.topic, 'topic');
  const errorCode =
    raw.errorCode === undefined ? null : requireDecodedNullableString(raw.errorCode, 'errorCode');
  const errorMessage =
    raw.errorMessage === undefined
      ? null
      : requireDecodedNullableString(raw.errorMessage, 'errorMessage');
  const source = raw.source == null ? undefined : requireDecodedString(raw.source, 'source');
  const metadataEntries = raw.metadata;
  if (!Array.isArray(metadataEntries)) {
    throw new ZLinkConfigurationException('Channel application metadata must be repeated entries.');
  }
  let checkedMetadata: Readonly<Record<string, string>>;
  if (metadataEntries.length === 0) {
    checkedMetadata = EMPTY_APPLICATION_METADATA;
  } else {
    const metadata: Record<string, unknown> = Object.create(null) as Record<string, unknown>;
    for (const entry of metadataEntries) {
      if (!isRecord(entry))
        throw new ZLinkConfigurationException('Channel metadata entry is invalid.');
      const key = requireDecodedString(entry.key, 'metadata.key');
      const entryValue = requireDecodedString(entry.value, `metadata.${key}`);
      if (key.length === 0) {
        throw new ZLinkConfigurationException(
          'Channel application metadata key must not be empty.'
        );
      }
      if (Object.prototype.hasOwnProperty.call(metadata, key)) {
        throw new ZLinkConfigurationException(
          'Channel application metadata contains a duplicate key.'
        );
      }
      metadata[key] = entryValue;
    }
    checkedMetadata = Object.freeze(metadata as Record<string, string>);
    if (metadataExceedsLimit(metadataEntries as { key: string; value: string }[])) {
      throw new ZLinkConfigurationException(
        `Channel application metadata exceeds the ${SERVICE_WIRE_METADATA_BYTES}-byte limit.`
      );
    }
  }
  return {
    kind,
    channelName,
    messageName,
    contentType,
    correlationId,
    deadline,
    topic,
    errorCode,
    errorMessage,
    source,
    metadata: checkedMetadata,
    flowId,
    flowOrigin
  };
}

function correlationIdForOutboundKind(
  kind: ZLinkChannelMessageKind,
  correlationId: string | null | undefined
): string | null {
  if (kind === ZLinkChannelMessageKind.Request) {
    if (correlationId === undefined || correlationId === null) return newChannelCorrelationId();
    validateCorrelationValue(correlationId);
    return correlationId;
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
  if (
    kind === ZLinkChannelMessageKind.Request ||
    kind === ZLinkChannelMessageKind.Response ||
    kind === ZLinkChannelMessageKind.Error
  ) {
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

const MAXIMUM_CORRELATION_ID_BYTES = 64;

const NON_ASCII_CORRELATION_ID = /[^\x00-\x7f]/;

export function isValidChannelCorrelationId(value: string): boolean {
  return (
    value.length >= 1 &&
    value.length <= MAXIMUM_CORRELATION_ID_BYTES &&
    !value.includes('\0') &&
    !NON_ASCII_CORRELATION_ID.test(value)
  );
}

function validateCorrelationValue(correlationId: string): void {
  if (!isValidChannelCorrelationId(correlationId)) {
    throw new ZLinkConfigurationException(
      'Channel envelope correlationId must be a non-empty ASCII value of at most 64 bytes.'
    );
  }
}

function channelKindName(kind: ZLinkChannelMessageKind): string {
  switch (kind) {
    case ZLinkChannelMessageKind.Request:
      return 'request';
    case ZLinkChannelMessageKind.Response:
      return 'response';
    case ZLinkChannelMessageKind.Command:
      return 'send';
    case ZLinkChannelMessageKind.Publish:
      return 'publish';
    case ZLinkChannelMessageKind.Error:
      return 'error';
  }
}

const EMPTY_APPLICATION_METADATA: Readonly<Record<string, string>> = Object.freeze({});

function applicationMetadataRecord(
  metadata: ReadonlyMap<string, string>
): Readonly<Record<string, string>> {
  if (metadata.size === 0) return EMPTY_APPLICATION_METADATA;
  const record: Record<string, unknown> = Object.create(null) as Record<string, unknown>;
  for (const [key, value] of metadata) record[key] = value;
  return validateApplicationMetadata(record);
}

const INVALID_UNICODE = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/u;

function validateApplicationMetadata(
  metadata: Record<string, unknown>
): Readonly<Record<string, string>> {
  for (const key of Object.keys(metadata)) {
    const value = requireString(metadata[key], `metadata.${key}`);
    if (
      key.length === 0 ||
      key.includes('\0') ||
      value.includes('\0') ||
      INVALID_UNICODE.test(key) ||
      INVALID_UNICODE.test(value)
    ) {
      throw new ZLinkConfigurationException(
        'Channel application metadata keys must be non-empty and keys and values must be valid Unicode without NUL.'
      );
    }
  }
  return Object.freeze(metadata as Record<string, string>);
}

function optionalFlowId(value: unknown): string | undefined {
  return value === undefined || value === null ? undefined : requireFlowId(value);
}

function optionalFlowOrigin(value: unknown): ZLinkFlowOrigin | undefined {
  return value === undefined || value === null ? undefined : requireFlowOrigin(value);
}

function requireFlowId(value: unknown): string {
  const flowId = requireDecodedString(value, 'flowId');
  if (!CHANNEL_FLOW_TEXT.test(flowId)) {
    throw new ZLinkConfigurationException('Channel envelope flowId must be a lowercase UUIDv7.');
  }
  return flowId;
}

function requireFlowOrigin(value: unknown): ZLinkFlowOrigin {
  switch (value) {
    case 1:
      return 'Inbound';
    case 2:
      return 'Timer';
    case 3:
      return 'Application';
    case 4:
      return 'Lifecycle';
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
  if (value.includes('\0') || INVALID_UNICODE.test(value)) {
    throw new ZLinkConfigurationException(
      `Channel envelope ${fieldName} must be valid Unicode without NUL.`
    );
  }
  return value;
}

function requireDecodedString(value: unknown, fieldName: string): string {
  if (typeof value !== 'string') {
    throw new ZLinkConfigurationException(`Channel envelope ${fieldName} must be a string.`);
  }
  if (value.includes('\0')) {
    throw new ZLinkConfigurationException(`Channel envelope ${fieldName} must not contain NUL.`);
  }
  return value;
}

function requireDecodedNullableString(value: unknown, fieldName: string): string | null {
  if (value === null) return null;
  return requireDecodedString(value, fieldName);
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
