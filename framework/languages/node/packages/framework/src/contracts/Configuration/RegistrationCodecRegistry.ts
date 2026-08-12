import type {
  ZLinkCodecExtension,
  ZLinkCodecRegistrar,
  ZLinkCodecRegistryBuilder,
  ZLinkMessageTypeSelector,
  ZLinkMessageSerializer
} from '../Codecs';
import type {
  ZLinkCodecRegistration,
  ZLinkCodecRegistryOptions,
  ZLinkCodecSerializerRegistration,
  ZLinkStreamCodecRegistration
} from './RegistrationTypes';
import { normalizeCodecContentType } from './CodecContentType';
import {
  matchEveryDeclaredMessageType,
  rememberCodecSerializerSelections
} from './CodecSerializerSelection';

export interface MutableCodecRegistryOptions {
  serializers: ZLinkCodecSerializerRegistration[];
  streamCodecs: ZLinkStreamCodecRegistration[];
}

export function createCodecRegistry(
  options: ZLinkCodecRegistryOptions | undefined
): RegistrationCodecRegistryBuilder {
  return new RegistrationCodecRegistryBuilder({
    serializers: [...(options?.serializers ?? [])],
    streamCodecs: [...(options?.streamCodecs ?? [])]
  });
}

export class RegistrationCodecRegistryBuilder implements ZLinkCodecRegistryBuilder, ZLinkCodecRegistrar {
  private readonly options: MutableCodecRegistryOptions;

  constructor(options: MutableCodecRegistryOptions = { serializers: [], streamCodecs: [] }) {
    this.options = options;
    const serializers = [...options.serializers];
    const streamCodecs = [...options.streamCodecs];
    options.serializers.length = 0;
    options.streamCodecs.length = 0;
    for (const entry of serializers) {
      if (entry.canSerialize === undefined) {
        this.addSerializer(entry.contentType, entry.serializer);
      } else {
        this.addSerializer(entry.contentType, entry.serializer, entry.canSerialize);
      }
    }
    for (const entry of streamCodecs) {
      this.addStreamCodec(entry.contentType, entry.codec);
    }
  }

  get registeredSerializers(): ReadonlyMap<string, ZLinkMessageSerializer> {
    const serializers = new Map(
      this.options.serializers.map((entry) => [entry.contentType, entry.serializer])
    );
    const selections = new Map(
      this.options.serializers.map((entry) => [
        entry.contentType,
        {
          selector: entry.canSerialize ?? matchEveryDeclaredMessageType,
          fallback: entry.canSerialize === undefined
        }
      ])
    );
    return rememberCodecSerializerSelections(serializers, selections);
  }

  get registeredStreamCodecs(): ReadonlyMap<string, unknown> {
    return new Map(this.options.streamCodecs.map((entry) => [entry.contentType, entry.codec]));
  }

  get registration(): ZLinkCodecRegistration {
    return {
      serializers: this.registeredSerializers,
      streamCodecs: this.registeredStreamCodecs
    };
  }

  get registeredCodecs(): readonly string[] {
    return this.options.serializers.map((entry) => entry.contentType);
  }

  use(extension: ZLinkCodecExtension): this {
    extension.register(this);
    return this;
  }

  addSerializer(contentType: string, serializer: ZLinkMessageSerializer): this;
  addSerializer(
    contentType: string,
    serializer: ZLinkMessageSerializer,
    canSerialize: ZLinkMessageTypeSelector
  ): this;
  addSerializer(
    contentType: string,
    serializer: ZLinkMessageSerializer,
    canSerialize?: ZLinkMessageTypeSelector
  ): this {
    const normalized = normalizeCodecContentType(contentType);
    const existing = this.options.serializers.findIndex((entry) => entry.contentType === normalized);
    const registration = { contentType: normalized, serializer, canSerialize };
    if (existing >= 0) this.options.serializers.splice(existing, 1);
    this.options.serializers.push(registration);
    return this;
  }

  addStreamCodec(contentType: string, codec: unknown): this {
    const normalized = normalizeCodecContentType(contentType);
    const existing = this.options.streamCodecs.findIndex((entry) => entry.contentType === normalized);
    const registration = { contentType: normalized, codec };
    if (existing >= 0) this.options.streamCodecs.splice(existing, 1);
    this.options.streamCodecs.push(registration);
    return this;
  }
}
