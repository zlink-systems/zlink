import type {
  ZlinkStreamCodec,
  ZlinkStreamConnectionState,
  ZlinkStreamErrorCode,
  ZlinkStreamHeaderFlags,
  ZlinkStreamMessageKind,
  ZlinkFlowOrigin
} from './ZlinkStreamEnums';
import type { ZlinkStreamMetadata } from './ZlinkStreamMetadata';

export interface ZlinkStreamEncodedPayload {
  readonly codec: ZlinkStreamCodec;
  readonly payload: Uint8Array;
  readonly messageType?: Function;
}

export interface ZlinkStreamFlow {
  readonly flowId: string;
  readonly flowOrigin: ZlinkFlowOrigin;
}

export interface ZlinkStreamMessage<TPayload = unknown> extends ZlinkStreamFlow {
  readonly name: string;
  readonly metadata: ZlinkStreamMetadata;
  readonly payload: TPayload;
}

export interface ZlinkStreamHeader {
  readonly kind: ZlinkStreamMessageKind;
  readonly codec: ZlinkStreamCodec;
  readonly flags: ZlinkStreamHeaderFlags;
  readonly requestSeq?: bigint;
  readonly name: string;
  readonly metadata: ZlinkStreamMetadata;
  readonly correlationId?: string;
  readonly flowId?: string;
  readonly flowOrigin?: ZlinkFlowOrigin;
}

export interface ZlinkStreamError {
  readonly code: ZlinkStreamErrorCode;
  readonly message: string;
  readonly cause?: unknown;
}

export interface ZlinkStreamConnectionStateChanged {
  readonly previous: ZlinkStreamConnectionState;
  readonly current: ZlinkStreamConnectionState;
  readonly error?: ZlinkStreamError;
}

export interface ZlinkStreamInboundObservation {
  readonly kind: ZlinkStreamMessageKind;
  readonly name: string;
  readonly codec: ZlinkStreamCodec;
  readonly requestSeq?: bigint;
  readonly flowId?: string;
  readonly flowOrigin?: ZlinkFlowOrigin;
  readonly metadata: ZlinkStreamMetadata;
  readonly payloadLength: number;
  readonly isCompressed: boolean;
  readonly receivedAt: Date;
  readonly payloadPreview: Uint8Array;
}

export interface ZlinkStreamResult {
  readonly isSuccess: boolean;
  readonly error?: ZlinkStreamError;
}

export interface ZlinkStreamResultOf<T> extends ZlinkStreamResult {
  readonly value?: T;
}

export class ZlinkStreamException extends Error {
  constructor(readonly error: ZlinkStreamError) {
    super(error.message);
    this.name = 'ZlinkStreamException';
  }
}
