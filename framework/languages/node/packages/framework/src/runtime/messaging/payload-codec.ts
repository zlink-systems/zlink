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

// Registration maps are created during host configuration and are immutable
// for the runtime lifetime. Compile the candidate list and reverse content
// type lookup once per map so the message path does not allocate arrays or
// scan the registry for every payload.
const serializerSelectionPlans = new WeakMap<object, ZLinkSerializerSelectionPlan>();

export function encodeFrameworkPayloadMessage(
  payload: unknown,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): Message {
  if (isZLinkMessage(payload)) {
    if (payload.isEncoded()) {
      return toRuntimeMessage(payload.toEncodedPayload());
    }
    return encodeFrameworkPayloadMessage(payload.decode(), registry);
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
    return toRuntimeMessage(serializer.serialize(payload));
  }

  return RuntimeMessage.from(Buffer.from(JSON.stringify(payload ?? null)));
}

export function decodeFrameworkPayloadMessage<T>(
  message: Message,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  type?: Type<T>
): T {
  return decodeFrameworkPayload(message, registry, type, false);
}

export function decodeFrameworkTypedPayloadMessage<T>(
  message: Message,
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>,
  type?: Type<T>
): T {
  return decodeFrameworkPayload(message, registry, type, true);
}

function decodeFrameworkPayload<T>(
  message: Message,
  registry: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer> | undefined,
  type: Type<T> | undefined,
  rejectInvalidJson: boolean
): T {
  if (message.isEmpty()) {
    return undefined as T;
  }

  const serializer = selectDefaultSerializer(registry);
  if (serializer !== undefined) {
    // A selective extension is also the only available decoder for an
    // inbound non-JSON stream frame. Only inspect JSON-shaped bytes for the
    // framework fallback; binary payloads go directly to the extension.
    if (isSelectableSerializer(serializer) && looksLikeJson(message.data())) {
      const parsedPayload = parseJsonPayload(message);
      if (parsedPayload.valid) {
        return parsedPayload.value as T;
      }
    }
    return serializer.deserialize(
      encodedPayloadFromOwned(message.data()),
      (type ?? Object) as Type<T>
    );
  }

  const parsedPayload = parseJsonPayload(message);
  if (parsedPayload.valid) {
    return parsedPayload.value as T;
  }
  const text = message.getString('utf8');
  if (!rejectInvalidJson) {
    return text as T;
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
  registry?: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer>
): ZLinkMessage {
  const payload = encodedPayloadFromOwned(message.data());
  return createZLinkMessageFromEncoded(payload, <T>(type?: Type<T>) =>
    decodeFrameworkEncodedPayload(payload, registry, type));
}

function decodeFrameworkEncodedPayload<T>(
  payload: ZLinkEncodedPayload,
  registry: ZLinkSerializerRegistryLike | ReadonlyMap<string, ZLinkMessageSerializer> | undefined,
  type: Type<T> | undefined
): T {
  if (payload.isEmpty()) return undefined as T;
  const serializer = selectDefaultSerializer(registry);
  if (serializer !== undefined) {
    if (isSelectableSerializer(serializer) && looksLikeJson(payload.data())) {
      try {
        return JSON.parse(payload.getString('utf8')) as T;
      } catch {
        // JSON-shaped extension payloads fall through to their registered decoder.
      }
    }
    return serializer.deserialize(payload, (type ?? Object) as Type<T>);
  }
  const text = payload.getString('utf8');
  try {
    return JSON.parse(text) as T;
  } catch {
    return text as T;
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

function looksLikeJson(bytes: Uint8Array): boolean {
  let index = 0;
  while (index < bytes.byteLength) {
    const value = bytes[index]!;
    if (value !== 0x20 && value !== 0x09 && value !== 0x0a && value !== 0x0d) break;
    index += 1;
  }
  if (index >= bytes.byteLength) return false;
  const first = bytes[index]!;
  return first === 0x7b // {
    || first === 0x5b // [
    || first === 0x22 // "
    || first === 0x2d // -
    || (first >= 0x30 && first <= 0x39)
    || first === 0x74 // t
    || first === 0x66 // f
    || first === 0x6e; // n
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
    return { valid: true, value: JSON.parse(message.getString('utf8')) };
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
