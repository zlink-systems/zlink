import type {
  ZLinkCodecExtension,
  ZLinkCodecRegistrar,
  ZLinkCodecRegistryBuilder,
  ZLinkMessageSerializer
} from '../Codecs';
import { ZLinkConfigurationException } from './ConfigurationException';
import type {
  ZLinkCodecRegistration,
  ZLinkCodecRegistryOptions,
  ZLinkCodecSerializerRegistration,
  ZLinkStreamCodecRegistration
} from './RegistrationTypes';

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
  constructor(private readonly options: MutableCodecRegistryOptions = { serializers: [], streamCodecs: [] }) {}

  get registeredSerializers(): ReadonlyMap<string, ZLinkMessageSerializer> {
    return new Map(this.options.serializers.map((entry) => [entry.contentType, entry.serializer]));
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

  addSerializer(contentType: string, serializer: ZLinkMessageSerializer): this {
    const normalized = normalizeCodecContentType(contentType);
    const existing = this.options.serializers.findIndex((entry) => entry.contentType === normalized);
    const registration = { contentType: normalized, serializer };
    if (existing >= 0) {
      this.options.serializers[existing] = registration;
    } else {
      this.options.serializers.push(registration);
    }
    return this;
  }

  addStreamCodec(contentType: string, codec: unknown): this {
    const normalized = normalizeCodecContentType(contentType);
    const existing = this.options.streamCodecs.findIndex((entry) => entry.contentType === normalized);
    const registration = { contentType: normalized, codec };
    if (existing >= 0) {
      this.options.streamCodecs[existing] = registration;
    } else {
      this.options.streamCodecs.push(registration);
    }
    return this;
  }
}

function normalizeCodecContentType(contentType: string): string {
  const normalized = contentType.trim();
  if (normalized.length === 0) {
    throw new ZLinkConfigurationException('Codec content type must not be empty.');
  }
  return normalized;
}
