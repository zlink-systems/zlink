export interface ZLinkStreamCompressionCodec {
  compress(payload: Uint8Array): Uint8Array;
  decompress(payload: Uint8Array, maxDecompressedSize: number): Uint8Array;
}

export interface ZLinkStreamCompressionOptions {
  readonly disabled?: boolean;
  readonly codec?: ZLinkStreamCompressionCodec;
}
