import type { ZLinkMessageSerializer } from './ZLinkMessageSerializer';

export interface ZLinkCodecExtension {
  register(codecs: ZLinkCodecRegistrar): void;
}

export interface ZLinkCodecRegistryBuilder {
  use(extension: ZLinkCodecExtension): this;
}

export interface ZLinkCodecRegistrar {
  addSerializer(contentType: string, serializer: ZLinkMessageSerializer): this;
  addStreamCodec(contentType: string, codec: unknown): this;
}
