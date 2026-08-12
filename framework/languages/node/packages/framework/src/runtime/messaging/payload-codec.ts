import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import {
  isZLinkMessage,
  ZLinkMessage,
  type Type,
  ZLinkEncodedPayload,
  type ZLinkMessageSerializer
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import {
  createZLinkMessageFromEncoded,
  materializeZLinkMessageValue,
  readZLinkMessageDeclaredType
} from '../../contracts/Common/ZLinkMessage';
import { adoptEncodedPayload, borrowEncodedPayload } from '../../contracts/Common/encoded-payload-storage';
import { ZLinkConfigurationException } from '../configuration';
import {
  parseFrameworkJsonV1,
  stringifyFrameworkJsonV1
} from './framework-json-v1';
import type { ZLinkJsonSchema } from '../../contracts/Handlers/JsonContract';
import { readZLinkPacketJsonContract } from '../../contracts/Handlers/Attributes';
import {
  readFrameworkPacketJsonContract,
  resolveFrameworkPacketJsonContract
} from './packet-name';
import { isCanonicalCodecContentType } from '../../contracts/Configuration/CodecContentType';
import {
  codecSerializerSelectionsOf,
  matchEveryDeclaredMessageType
} from '../../contracts/Configuration/CodecSerializerSelection';

export interface ZLinkSerializerRegistryLike {
  readonly serializers: ReadonlyMap<string, ZLinkMessageSerializer>;
}

interface ZLinkSerializerSelectionEntry {
  readonly contentType: string;
  readonly serializer: ZLinkMessageSerializer;
  readonly selector: (declaredType: Type) => boolean;
  readonly fallback: boolean;
}

interface ZLinkSerializerSelectionPlan {
  readonly serializers: readonly ZLinkSerializerSelectionEntry[];
  readonly defaultSerializer: ZLinkSerializerSelectionEntry | undefined;
  select(declaredType: Type | undefined): ZLinkSerializerSelectionEntry | undefined;
}

const noSerializer = Symbol('noSerializer');
const JSON_CONTENT_TYPE = 'application/json';

// Registration maps are created during host configuration and are immutable
// for the runtime lifetime. Compile the candidate list and reverse content
// type lookup once per map so the message path does not allocate arrays or
// scan the registry for every payload.
const serializerSelectionPlans = new WeakMap<object, ZLinkSerializerSelectionPlan>();
const encodedContentTypes = new WeakMap<object, string>();

export interface ZLinkEncodedFrameworkPayload {
  readonly message: Message;
  readonly contentType: string;
}

export function encodeFrameworkPayloadMessage(
  payload: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  packetName?: string,
  contractPart: 'payload' | 'reply' = 'payload'
): Message {
  return encodeFrameworkPayload(payload, registry, packetName, contractPart).message;
}

export function encodeFrameworkPayload(
  payload: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  packetName?: string,
  contractPart: 'payload' | 'reply' = 'payload'
): ZLinkEncodedFrameworkPayload {
  let declaredType: Type | undefined;
  if (isZLinkMessage(payload)) {
    if (payload.isEncoded()) {
      return {
        message: rememberContentType(toRuntimeMessage(payload.toEncodedPayload()), 'application/octet-stream'),
        contentType: 'application/octet-stream'
      };
    }
    declaredType = readZLinkMessageDeclaredType(payload);
    payload = payload.decode();
  }
  if (isMessage(payload)) {
    throw new ZLinkConfigurationException(
      'Raw binding Message is not accepted by the default framework payload API. Use ZLinkMessage.fromEncoded(...) for explicit raw forwarding.'
    );
  }
  if (Buffer.isBuffer(payload) || payload instanceof Uint8Array) {
    throw new ZLinkConfigurationException(
      'Raw Buffer payload is not accepted by the default framework payload API. Wrap a DTO with ZLinkMessage.from(...) or use ZLinkMessage.fromEncoded(...) for explicit raw forwarding.'
    );
  }

  const selected = selectSerializerWithContentType(payload, registry, declaredType);
  if (selected !== undefined) {
    return {
      message: rememberContentType(
        toRuntimeMessage(selected.serializer.serialize(payload)),
        selected.contentType
      ),
      contentType: selected.contentType
    };
  }

  return {
    message: rememberContentType(
      RuntimeMessage.fromOwned(Buffer.from(stringifyFrameworkJsonV1(
        payload,
        (resolveFrameworkPacketJsonContract(payload, packetName)
          ?? (packetName === undefined ? undefined : readZLinkPacketJsonContract(packetName)))?.[contractPart]
      ))),
      JSON_CONTENT_TYPE
    ),
    contentType: JSON_CONTENT_TYPE
  };
}

export function frameworkPayloadContentType(message: Message): string {
  return encodedContentTypes.get(message as object) ?? JSON_CONTENT_TYPE;
}

export function decodeFrameworkPayloadMessage<T>(
  message: Message,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  type?: Type<T>,
  contentType = JSON_CONTENT_TYPE,
  packetName?: string,
  contractPart: 'payload' | 'reply' = 'payload'
): T {
  return decodeFrameworkPayload(message, registry, type, contentType, packetName, contractPart);
}

export function decodeFrameworkTypedPayloadMessage<T>(
  message: Message,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  type?: Type<T>,
  contentType = JSON_CONTENT_TYPE,
  packetName?: string,
  contractPart: 'payload' | 'reply' = 'payload'
): T {
  return decodeFrameworkPayload(message, registry, type, contentType, packetName, contractPart);
}

function decodeFrameworkPayload<T>(
  message: Message,
  registry: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer> | undefined,
  type: Type<T> | undefined,
  contentType: string,
  packetName: string | undefined,
  contractPart: 'payload' | 'reply'
): T {
  if (!isCanonicalCodecContentType(contentType)) {
    throw unsupportedContentType(contentType);
  }
  if (contentType !== JSON_CONTENT_TYPE) {
    const serializer = serializerMapOf(registry)?.get(contentType);
    if (serializer === undefined) throw unsupportedContentType(contentType);
    if (message.isEmpty()) return undefined as T;
    return serializer.deserialize(
      encodedPayloadFromOwned(message.data()),
      (type ?? Object) as Type<T>
    );
  }

  if (message.isEmpty()) return undefined as T;

  const parsedPayload = parseJsonPayload(message, schemaForDecode(type, packetName, contractPart));
  if (parsedPayload.valid) {
    return materializeZLinkMessageValue(parsedPayload.value as T, type);
  }
  throw createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
    'PayloadDecodeFailed: framework payload is not valid JSON.',
    false,
    parsedPayload.error
  );
}

export function wrapFrameworkPayloadMessage(
  message: Message,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  contentType = JSON_CONTENT_TYPE,
  packetName?: string,
  contractPart: 'payload' | 'reply' = 'payload'
): ZLinkMessage {
  const payload = encodedPayloadFromOwned(message.data());
  return createZLinkMessageFromEncoded(payload, <T>(type?: Type<T>) =>
    decodeFrameworkEncodedPayload(payload, registry, type, contentType, packetName, contractPart));
}

function decodeFrameworkEncodedPayload<T>(
  payload: ZLinkEncodedPayload,
  registry: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer> | undefined,
  type: Type<T> | undefined,
  contentType: string,
  packetName: string | undefined,
  contractPart: 'payload' | 'reply'
): T {
  if (!isCanonicalCodecContentType(contentType)) {
    throw unsupportedContentType(contentType);
  }
  if (contentType !== JSON_CONTENT_TYPE) {
    const serializer = serializerMapOf(registry)?.get(contentType);
    if (serializer === undefined) throw unsupportedContentType(contentType);
    if (payload.isEmpty()) return undefined as T;
    return serializer.deserialize(payload, (type ?? Object) as Type<T>);
  }
  if (payload.isEmpty()) return undefined as T;
  const text = payload.getString('utf8');
  try {
    return materializeZLinkMessageValue(parseFrameworkJsonV1(
      text,
      {},
      schemaForDecode(type, packetName, contractPart)
    ) as T, type);
  } catch (error) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
      'PayloadDecodeFailed: framework payload is not valid JSON.',
      false,
      error
    );
  }
}

function schemaForDecode(
  type: Type<unknown> | undefined,
  packetName: string | undefined,
  contractPart: 'payload' | 'reply'
): ZLinkJsonSchema | undefined {
  const contract = type === undefined
    ? undefined
    : readFrameworkPacketJsonContract(type, packetName);
  return (contract ?? (packetName === undefined ? undefined : readZLinkPacketJsonContract(packetName)))?.[contractPart];
}

export function selectDefaultSerializer(
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): ZLinkMessageSerializer | undefined {
  return serializerSelectionPlanOf(registry)?.defaultSerializer?.serializer;
}

export function selectSerializer(
  value: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  declaredType?: Type
): ZLinkMessageSerializer | undefined {
  return selectSerializerWithContentType(value, registry, declaredType)?.serializer;
}

export function selectSerializerWithContentType(
  value: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  declaredType?: Type
): ZLinkSerializerSelectionEntry | undefined {
  return serializerSelectionPlanOf(registry)?.select(
    declaredType ?? outboundBusinessType(value)
  );
}

function serializerSelectionPlanOf(
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): ZLinkSerializerSelectionPlan | undefined {
  const serializers = serializerMapOf(registry);
  if (serializers === undefined || serializers.size === 0) {
    return undefined;
  }
  const key = serializers as object;
  const cached = serializerSelectionPlans.get(key);
  if (cached !== undefined) {
    return cached;
  }
  const entries: ZLinkSerializerSelectionEntry[] = [];
  const registeredSelections = codecSerializerSelectionsOf(serializers);
  for (const [contentType, serializer] of serializers) {
    const selection = registeredSelections?.get(contentType);
    entries.push({
      contentType,
      serializer,
      selector: selection?.selector ?? matchEveryDeclaredMessageType,
      fallback: selection?.fallback ?? true
    });
  }
  const frozenEntries = Object.freeze(entries);
  const outboundByBusinessType = new WeakMap<
    object,
    ZLinkSerializerSelectionEntry | typeof noSerializer
  >();
  let cachedBusinessTypeCount = 0;
  const defaultSerializer = selectDefaultSerializerFromEntries(frozenEntries);
  const plan: ZLinkSerializerSelectionPlan = {
    serializers: frozenEntries,
    defaultSerializer,
    select: (declaredType) => {
      if (declaredType === undefined) return defaultSerializer;
      const cached = outboundByBusinessType.get(declaredType);
      if (cached !== undefined) {
        return cached === noSerializer ? undefined : cached;
      }
      const selected = selectSerializerFromEntries(frozenEntries, declaredType);
      if (cachedBusinessTypeCount < 1024) {
        outboundByBusinessType.set(declaredType, selected ?? noSerializer);
        cachedBusinessTypeCount += 1;
      }
      return selected;
    }
  };
  serializerSelectionPlans.set(key, plan);
  return plan;
}

function outboundBusinessType(value: unknown): Type | undefined {
  if (value === null || value === undefined) return undefined;
  switch (typeof value) {
    case 'string': return String as unknown as Type;
    case 'number': return Number as unknown as Type;
    case 'boolean': return Boolean as unknown as Type;
    case 'bigint': return BigInt as unknown as Type;
    case 'symbol': return Symbol as unknown as Type;
    case 'function': return Function as unknown as Type;
    case 'object': {
      const constructor = (value as { constructor?: unknown }).constructor;
      return typeof constructor === 'function'
        ? constructor as Type
        : Object as Type;
    }
  }
  throw new TypeError('Unsupported payload type.');
}

function selectSerializerFromEntries(
  serializers: readonly ZLinkSerializerSelectionEntry[],
  declaredType: Type
): ZLinkSerializerSelectionEntry | undefined {
  for (let index = serializers.length - 1; index >= 0; index--) {
    const entry = serializers[index]!;
    if (entry.selector(declaredType)) return entry;
  }
  return undefined;
}

function selectDefaultSerializerFromEntries(
  serializers: readonly ZLinkSerializerSelectionEntry[]
): ZLinkSerializerSelectionEntry | undefined {
  for (let index = serializers.length - 1; index >= 0; index--) {
    const entry = serializers[index]!;
    if (entry.fallback) return entry;
  }
  return undefined;
}

function unsupportedContentType(contentType: string): Error {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
    `PayloadDecodeFailed: unsupported framework content type '${contentType}'.`
  );
}

function parseJsonPayload(message: Message, schema?: ZLinkJsonSchema): {
  readonly valid: true;
  readonly value: unknown;
  readonly error?: undefined;
} | {
  readonly valid: false;
  readonly value?: undefined;
  readonly error: unknown;
} {
  try {
    return { valid: true, value: parseFrameworkJsonV1(message.getString('utf8'), {}, schema) };
  } catch (error) {
    return { valid: false, error };
  }
}

function serializerMapOf(
  registry: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer> | undefined
): ReadonlyMap<string, ZLinkMessageSerializer> | undefined {
  if (registry === undefined) {
    return undefined;
  }
  return 'serializers' in registry ? registry.serializers : registry;
}

function isMessage(value: unknown): value is Message {
  return typeof value === 'object'
    && value !== null
    && typeof (value as { data?: unknown }).data === 'function';
}

function toRuntimeMessage(payload: ZLinkEncodedPayload): Message {
  const owned = borrowEncodedPayload(payload);
  return owned === undefined
    ? RuntimeMessage.from(payload.data())
    : RuntimeMessage.fromOwned(owned);
}

function encodedPayloadFromOwned(bytes: Uint8Array): ZLinkEncodedPayload {
  if (!Buffer.isBuffer(bytes)) {
    return ZLinkEncodedPayload.from(bytes);
  }
  const payload = ZLinkEncodedPayload.from(Buffer.alloc(0));
  adoptEncodedPayload(payload, bytes);
  return payload;
}

function rememberContentType(message: Message, contentType: string): Message {
  encodedContentTypes.set(message as object, contentType);
  return message;
}
