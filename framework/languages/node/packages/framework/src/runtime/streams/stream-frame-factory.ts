import type {
  ZLinkStreamCompressionCodec,
  ZLinkStreamCompressionOptions
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkBufferMessage as ZLinkBindingMessage } from '../backend/runtime-message';
import { currentOrCreateFlow } from '../diagnostics/flow-context';
import {
  createStreamReplyHeader,
  encodeStreamFrame,
  lz4Pickle,
  lz4Unpickle,
  utf8Decode,
  utf8Encode,
  ZLinkStreamCodec,
  type ZLinkStreamFrameHeader,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind,
  type ZLinkStreamReplyMessageKind
} from './protocol';

const DEFAULT_MAX_DECOMPRESSED_STREAM_PAYLOAD_SIZE = 64 * 1024;

export interface ZLinkStreamFramePayloadCodec {
  encode(payload: unknown, context?: {
    readonly messageType?: Function;
    readonly packetName?: string;
    readonly direction: 'Request' | 'Response' | 'Error' | 'Send';
  } | Function): {
    readonly codec: ZLinkStreamCodec;
    readonly payload: Uint8Array;
  };
}

export interface ZLinkStreamFrameMessageFactoryOptions {
  readonly messageFactory?: ZLinkStreamFrameMessageFactorySource;
  readonly streamPayloadCodec?: ZLinkStreamFramePayloadCodec;
  readonly streamCompression?: ZLinkStreamCompressionOptions;
  readonly flowCreationEnabled?: () => boolean;
}

export interface ZLinkStreamFrameMessageFactorySource {
  createTextMessage(payload: string): Message;
  createBinaryMessage?(payload: Uint8Array): Message;
}

export class ZLinkStreamFrameMessageFactory {
  private readonly compressionCodec: ZLinkStreamCompressionCodec | undefined;

  constructor(private readonly options: ZLinkStreamFrameMessageFactoryOptions) {
    this.compressionCodec = resolveStreamCompressionCodec(options.streamCompression);
  }

  createTextMessage(payload: string): Message {
    return this.requireMessageFactory().createTextMessage(payload);
  }

  createBinaryMessage(payload: Uint8Array): Message {
    const factory = this.requireMessageFactory();
    if (factory.createBinaryMessage !== undefined) {
      return factory.createBinaryMessage(payload);
    }
    return factory.createTextMessage(utf8Decode(payload));
  }

  createJsonFrameMessage(
    kind: ZLinkStreamMessageKind,
    packetName: string,
    metadata: ReadonlyMap<string, string>,
    compressed: boolean,
    requestSeq: bigint | undefined,
    payload: unknown,
    correlationId?: string
  ): Message {
    const flow = currentOrCreateFlow('Application', this.options.flowCreationEnabled?.() ?? true);
    return this.createJsonFrameMessageWithHeader(
      payload,
      compressed,
      packetName,
      streamDirection(kind),
      (codec, flags) => ({
        kind,
        codec,
        flags,
        requestSeq,
        name: kind === ZLinkStreamMessageKind.Response || kind === ZLinkStreamMessageKind.Error
          ? ''
          : packetName,
        metadata,
        correlationId,
        ...(flow ?? {})
      })
    );
  }

  createJsonReplyFrameMessage(
    requestHeader: ZLinkStreamFrameHeader,
    kind: ZLinkStreamReplyMessageKind,
    metadata: ReadonlyMap<string, string>,
    compressed: boolean,
    payload: unknown
  ): Message {
    return this.createJsonFrameMessageWithHeader(
      payload,
      compressed,
      requestHeader.name,
      streamDirection(kind),
      (codec, flags) => createStreamReplyHeader(requestHeader, kind, codec, flags, metadata)
    );
  }

  private createJsonFrameMessageWithHeader(
    payload: unknown,
    compressed: boolean,
    packetName: string,
    direction: 'Request' | 'Response' | 'Error' | 'Send',
    createHeader: (codec: ZLinkStreamCodec, flags: ZLinkStreamHeaderFlags) => ZLinkStreamFrameHeader
  ): Message {
    const encoded = this.encodePayload(payload, packetName, direction);
    let body = encoded.payload;
    if (compressed) {
      body = compressStreamPayload(body, this.compressionCodec);
    }
    const flags = compressed ? ZLinkStreamHeaderFlags.PayloadCompressed : ZLinkStreamHeaderFlags.None;
    const frame = encodeStreamFrame(createHeader(encoded.codec, flags), body);
    return this.createBinaryMessage(frame);
  }

  private encodePayload(
    payload: unknown,
    packetName: string,
    direction: 'Request' | 'Response' | 'Error' | 'Send'
  ): { codec: ZLinkStreamCodec; payload: Uint8Array } {
    const codec = this.options.streamPayloadCodec;
    if (codec !== undefined) {
      if (direction !== 'Error') {
        return codec.encode(payload, { packetName, direction });
      }
    }
    return {
      codec: ZLinkStreamCodec.Json,
      payload: utf8Encode(JSON.stringify(payload))
    };
  }

  private requireMessageFactory(): ZLinkStreamFrameMessageFactorySource {
    return this.options.messageFactory ?? defaultStreamMessageFactory;
  }
}

function streamDirection(kind: ZLinkStreamMessageKind): 'Request' | 'Response' | 'Error' | 'Send' {
  switch (kind) {
    case ZLinkStreamMessageKind.Request: return 'Request';
    case ZLinkStreamMessageKind.Response: return 'Response';
    case ZLinkStreamMessageKind.Error: return 'Error';
    default: return 'Send';
  }
}

export const zlinkStreamLz4CompressionCodec: ZLinkStreamCompressionCodec = {
  compress(payload: Uint8Array): Uint8Array {
    return lz4Pickle(payload);
  },
  decompress(payload: Uint8Array, maxDecompressedSize: number): Uint8Array {
    return lz4Unpickle(payload, maxDecompressedSize);
  }
};

export function resolveStreamCompressionCodec(
  options: ZLinkStreamCompressionOptions | undefined
): ZLinkStreamCompressionCodec | undefined {
  if (options?.disabled === true) {
    return undefined;
  }
  return options?.codec ?? zlinkStreamLz4CompressionCodec;
}

function compressStreamPayload(
  payload: Uint8Array,
  codec: ZLinkStreamCompressionCodec | undefined
): Uint8Array {
  if (codec === undefined) {
    throw new Error('Compression codec is not configured.');
  }
  try {
    return codec.compress(payload);
  } catch (error) {
    throw new Error(`Compression failed: ${error instanceof Error ? error.message : String(error)}`);
  }
}

export function decompressStreamPayload(
  payload: Uint8Array,
  codec: ZLinkStreamCompressionCodec | undefined,
  maxDecompressedSize = DEFAULT_MAX_DECOMPRESSED_STREAM_PAYLOAD_SIZE
): Uint8Array {
  if (codec === undefined) {
    throw new Error('Compression codec is not configured.');
  }
  let decompressed: Uint8Array;
  try {
    decompressed = codec.decompress(payload, maxDecompressedSize);
  } catch (error) {
    throw new Error(`Decompression failed: ${error instanceof Error ? error.message : String(error)}`);
  }
  if (decompressed.length > maxDecompressedSize) {
    throw new Error('Decompressed stream payload exceeds maximum stream payload size.');
  }
  return decompressed;
}

const defaultStreamMessageFactory: ZLinkStreamFrameMessageFactorySource = {
  createTextMessage(payload: string): Message {
    return ZLinkBindingMessage.from(Buffer.from(payload));
  },
  createBinaryMessage(payload: Uint8Array): Message {
    return ZLinkBindingMessage.from(Buffer.from(payload));
  }
};
