import { encodeFrameworkPayload } from '../messaging/payload-codec';
import {
  decodeStreamHeader,
  encodeStreamFrame,
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
  flowEnabled: boolean
): readonly Buffer[] {
  const requestHeader = decodeStreamHeader(requestHeaderBytes, flowEnabled);
  const encoded =
    kind === ZLinkStreamMessageKind.Response
      ? encodeFrameworkPayload(payload, serializers, requestHeader.name, 'payload')
      : encodeFrameworkPayload(payload);
  try {
    const frame = encodeStreamFrame(
      {
        kind,
        codec: streamCodecForContentType(encoded.contentType),
        flags: ZLinkStreamHeaderFlags.None,
        requestSeq: requestHeader.requestSeq,
        name: '',
        metadata: new Map(),
        correlationId: requestHeader.correlationId,
        flowId: requestHeader.flowId,
        flowOrigin: requestHeader.flowOrigin
      },
      encoded.message.data()
    );
    return [Buffer.from(frame.buffer, frame.byteOffset, frame.byteLength)];
  } finally {
    encoded.message.close();
  }
}
