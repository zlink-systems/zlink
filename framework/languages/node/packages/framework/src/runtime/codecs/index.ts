import type {
  ZLinkCodecExtension,
  ZLinkCodecRegistrar,
  ZLinkCodecRegistryBuilder,
  ZLinkMessageTypeSelector,
  ZLinkMessageSerializer
} from '../../contracts';
import { normalizeCodecContentType } from '../../contracts/Configuration/CodecContentType';
import {
  matchEveryDeclaredMessageType,
  rememberCodecSerializerSelections,
  type ZLinkCodecSerializerSelection
} from '../../contracts/Configuration/CodecSerializerSelection';

export class DefaultZLinkCodecRegistryBuilder implements ZLinkCodecRegistryBuilder, ZLinkCodecRegistrar {
  private readonly serializers = new Map<string, ZLinkMessageSerializer>();
  private readonly serializerSelections = new Map<string, ZLinkCodecSerializerSelection>();
  private readonly streamCodecs = new Map<string, unknown>();
  private readonly codecs = new Set<string>();

  get registeredCodecs(): readonly string[] {
    return [...this.codecs];
  }

  get registeredSerializers(): ReadonlyMap<string, ZLinkMessageSerializer> {
    return rememberCodecSerializerSelections(this.serializers, this.serializerSelections);
  }

  get registeredStreamCodecs(): ReadonlyMap<string, unknown> {
    return this.streamCodecs;
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
    this.serializers.delete(normalized);
    this.serializerSelections.delete(normalized);
    this.serializers.set(normalized, serializer);
    this.serializerSelections.set(normalized, {
      selector: canSerialize ?? matchEveryDeclaredMessageType,
      fallback: canSerialize === undefined
    });
    this.codecs.add(normalized);
    return this;
  }

  addStreamCodec(contentType: string, codec: unknown): this {
    const normalized = normalizeCodecContentType(contentType);
    this.streamCodecs.delete(normalized);
    this.streamCodecs.set(normalized, codec);
    this.codecs.add(normalized);
    return this;
  }

}
