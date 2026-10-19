import { encodeFrameworkPayload } from '../messaging/payload-codec';
import {
  decodeStreamHeader,
  encodeStreamHeader,
  streamCodecForContentType,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind
} from '../streams/protocol';

/** Ordinary dispatch and relocation use the same typed Actor reply envelope. */
export function encodeActorReply(
  requestHeaderBytes: Uint8Array,
  kind: ZLinkStreamMessageKind.Response | ZLinkStreamMessageKind.Error,
  payload: unknown,
  serializers: Parameters<typeof encodeFrameworkPayload>[1],
  flowEnabled = true
): readonly Buffer[] {
  const requestHeader = decodeStreamHeader(requestHeaderBytes, flowEnabled);
  const encoded = encodeFrameworkPayload(payload, serializers);
  try {
    const replyHeader = encodeStreamHeader({
      kind,
      codec: streamCodecForContentType(encoded.contentType),
      flags: ZLinkStreamHeaderFlags.None,
      requestSeq: requestHeader.requestSeq,
      name: '',
      metadata: new Map(),
      correlationId: requestHeader.correlationId,
      flowId: requestHeader.flowId,
      flowOrigin: requestHeader.flowOrigin
    });
    return [
      Buffer.from(replyHeader.buffer, replyHeader.byteOffset, replyHeader.byteLength),
      Buffer.from(encoded.message.data())
    ];
  } finally {
    encoded.message.close();
  }
}
