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
import { createZLinkMessageFromEncoded } from '../../contracts/Common/ZLinkMessage';
import { adoptEncodedPayload, borrowEncodedPayload } from '../../contracts/Common/encoded-payload-storage';
import { ZLinkConfigurationException } from '../configuration';
import {
  parseFrameworkJsonV1,
  stringifyFrameworkJsonV1
} from './framework-json-v1';

export interface ZLinkSerializerRegistryLike {
  readonly serializers: ReadonlyMap<string, ZLinkMessageSerializer>;
}

interface ZLinkSelectableMessageSerializer extends ZLinkMessageSerializer {
  canSerialize?(value: unknown): boolean;
}

interface ZLinkSerializerSelectionPlan {
  readonly serializers: readonly ZLinkSelectableMessageSerializer[];
  readonly contentTypes: ReadonlyMap<ZLinkMessageSerializer, string>;
  readonly defaultSerializer: ZLinkMessageSerializer | undefined;
  select(value: unknown): ZLinkMessageSerializer | undefined;
  contentTypeOf(serializer: ZLinkMessageSerializer): string | undefined;
}

const noSerializer = Symbol('noSerializer');
const nullPayloadType = Object.freeze({ kind: 'null' });
const undefinedPayloadType = Object.freeze({ kind: 'undefined' });
const objectWithoutConstructorType = Object.freeze({ kind: 'objectWithoutConstructor' });
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
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): Message {
  return encodeFrameworkPayload(payload, registry).message;
}

export function encodeFrameworkPayload(
  payload: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): ZLinkEncodedFrameworkPayload {
  if (isZLinkMessage(payload)) {
    if (payload.isEncoded()) {
      return {
        message: rememberContentType(toRuntimeMessage(payload.toEncodedPayload()), 'application/octet-stream'),
        contentType: 'application/octet-stream'
      };
    }
    return encodeFrameworkPayload(payload.decode(), registry);
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

  const serializer = selectSerializer(payload, registry);
  if (serializer !== undefined) {
    const contentType = contentTypeForSerializer(serializer, registry);
    if (contentType === undefined) {
      throw new ZLinkConfigurationException(
        'Payload serializer is not registered under a content type.'
      );
    }
    return {
      message: rememberContentType(toRuntimeMessage(serializer.serialize(payload)), contentType),
      contentType
    };
  }

  return {
    message: rememberContentType(
      RuntimeMessage.from(Buffer.from(stringifyFrameworkJsonV1(payload))),
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
  contentType = JSON_CONTENT_TYPE
): T {
  return decodeFrameworkPayload(message, registry, type, contentType);
}

export function decodeFrameworkTypedPayloadMessage<T>(
  message: Message,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  type?: Type<T>,
  contentType = JSON_CONTENT_TYPE
): T {
  return decodeFrameworkPayload(message, registry, type, contentType);
}

function decodeFrameworkPayload<T>(
  message: Message,
  registry: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer> | undefined,
  type: Type<T> | undefined,
  contentType: string
): T {
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

  const parsedPayload = parseJsonPayload(message);
  if (parsedPayload.valid) {
    return parsedPayload.value as T;
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
  contentType = JSON_CONTENT_TYPE
): ZLinkMessage {
  const payload = encodedPayloadFromOwned(message.data());
  return createZLinkMessageFromEncoded(payload, <T>(type?: Type<T>) =>
    decodeFrameworkEncodedPayload(payload, registry, type, contentType));
}

function decodeFrameworkEncodedPayload<T>(
  payload: ZLinkEncodedPayload,
  registry: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer> | undefined,
  type: Type<T> | undefined,
  contentType: string
): T {
  if (contentType !== JSON_CONTENT_TYPE) {
    const serializer = serializerMapOf(registry)?.get(contentType);
    if (serializer === undefined) throw unsupportedContentType(contentType);
    if (payload.isEmpty()) return undefined as T;
    return serializer.deserialize(payload, (type ?? Object) as Type<T>);
  }
  if (payload.isEmpty()) return undefined as T;
  const text = payload.getString('utf8');
  try {
    return parseFrameworkJsonV1(text) as T;
  } catch (error) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
      'PayloadDecodeFailed: framework payload is not valid JSON.',
      false,
      error
    );
  }
}

export function selectDefaultSerializer(
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): ZLinkMessageSerializer | undefined {
  return serializerSelectionPlanOf(registry)?.defaultSerializer;
}

export function selectSerializer(
  value: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): ZLinkMessageSerializer | undefined {
  return serializerSelectionPlanOf(registry)?.select(value);
}

export function contentTypeForSerializer(
  serializer: ZLinkMessageSerializer,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): string | undefined {
  return serializerSelectionPlanOf(registry)?.contentTypeOf(serializer);
}

function isSelectableSerializer(serializer: ZLinkMessageSerializer): serializer is ZLinkSelectableMessageSerializer & { canSerialize: (value: unknown) => boolean } {
  return typeof (serializer as ZLinkSelectableMessageSerializer).canSerialize === 'function';
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
  const entries: ZLinkSelectableMessageSerializer[] = [];
  const contentTypes = new Map<ZLinkMessageSerializer, string>();
  for (const [contentType, serializer] of serializers) {
    entries.push(serializer as ZLinkSelectableMessageSerializer);
    contentTypes.set(serializer, contentType);
  }
  const frozenEntries = Object.freeze(entries);
  const outboundByBusinessType = new WeakMap<object, ZLinkMessageSerializer | typeof noSerializer>();
  const defaultSerializer = frozenEntries.length === 1
    ? frozenEntries[0]
    : selectSerializerFromEntries(frozenEntries, undefined);
  const plan: ZLinkSerializerSelectionPlan = {
    serializers: frozenEntries,
    contentTypes,
    defaultSerializer,
    select: (value) => {
      const businessType = outboundBusinessType(value);
      const cached = outboundByBusinessType.get(businessType);
      if (cached !== undefined) {
        return cached === noSerializer ? undefined : cached;
      }
      const selected = selectSerializerFromEntries(frozenEntries, value);
      outboundByBusinessType.set(businessType, selected ?? noSerializer);
      return selected;
    },
    contentTypeOf: (serializer) => contentTypes.get(serializer)
  };
  serializerSelectionPlans.set(key, plan);
  return plan;
}

function outboundBusinessType(value: unknown): object {
  if (value === null) return nullPayloadType;
  if (value === undefined) return undefinedPayloadType;
  switch (typeof value) {
    case 'string': return String;
    case 'number': return Number;
    case 'boolean': return Boolean;
    case 'bigint': return BigInt;
    case 'symbol': return Symbol;
    case 'function': return value;
    case 'object': {
      const constructor = (value as { constructor?: unknown }).constructor;
      return typeof constructor === 'function'
        ? constructor
        : objectWithoutConstructorType;
    }
  }
  throw new TypeError('Unsupported payload type.');
}

function selectSerializerFromEntries(
  serializers: readonly ZLinkSelectableMessageSerializer[],
  value: unknown
): ZLinkMessageSerializer | undefined {
  let matching: ZLinkMessageSerializer | undefined;
  let matchingCount = 0;
  let allSelectable = true;
  for (const serializer of serializers) {
    if (!isSelectableSerializer(serializer)) {
      allSelectable = false;
      continue;
    }
    if (serializer.canSerialize(value) === true) {
      matching = serializer;
      matchingCount += 1;
      if (matchingCount > 1) {
        throw new ZLinkConfigurationException(
          'Payload serializer is ambiguous because more than one serializer accepts the payload.'
        );
      }
    }
  }
  if (matchingCount === 1) return matching;
  if (serializers.length === 1) {
    return isSelectableSerializer(serializers[0]!) ? undefined : serializers[0];
  }
  if (allSelectable) return undefined;
  throw new ZLinkConfigurationException(
    'Payload serializer is ambiguous because more than one serializer is registered.'
  );
}

function unsupportedContentType(contentType: string): Error {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed,
    `PayloadDecodeFailed: unsupported framework content type '${contentType}'.`
  );
}

function parseJsonPayload(message: Message): {
  readonly valid: true;
  readonly value: unknown;
  readonly error?: undefined;
} | {
  readonly valid: false;
  readonly value?: undefined;
  readonly error: unknown;
} {
  try {
    return { valid: true, value: parseFrameworkJsonV1(message.getString('utf8')) };
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
