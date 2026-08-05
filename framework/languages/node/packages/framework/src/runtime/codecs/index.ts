import type {
  ZLinkCodecExtension,
  ZLinkCodecRegistrar,
  ZLinkCodecRegistryBuilder,
  ZLinkMessageSerializer
} from '../../contracts';

export class DefaultZLinkCodecRegistryBuilder implements ZLinkCodecRegistryBuilder, ZLinkCodecRegistrar {
  private readonly serializers = new Map<string, ZLinkMessageSerializer>();
  private readonly streamCodecs = new Map<string, unknown>();
  private readonly codecs = new Set<string>();

  get registeredCodecs(): readonly string[] {
    return [...this.codecs];
  }

  get registeredSerializers(): ReadonlyMap<string, ZLinkMessageSerializer> {
    return this.serializers;
  }

  get registeredStreamCodecs(): ReadonlyMap<string, unknown> {
    return this.streamCodecs;
  }

  use(extension: ZLinkCodecExtension): this {
    extension.register(this);
    return this;
  }

  addSerializer(contentType: string, serializer: ZLinkMessageSerializer): this {
    const normalized = normalizeContentType(contentType);
    this.serializers.set(normalized, serializer);
    this.codecs.add(normalized);
    return this;
  }

  addStreamCodec(contentType: string, codec: unknown): this {
    const normalized = normalizeContentType(contentType);
    this.streamCodecs.set(normalized, codec);
    this.codecs.add(normalized);
    return this;
  }

}

function normalizeContentType(contentType: string): string {
  const normalized = contentType.trim();
  if (normalized.length === 0) {
    throw new Error('Codec content type must not be empty.');
  }
  return normalized;
}
