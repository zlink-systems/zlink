import type {
  ZlinkStreamCodec,
  ZlinkStreamConnectionState,
  ZlinkStreamErrorCode,
  ZlinkStreamHeaderFlags,
  ZlinkStreamMessageKind
} from './ZlinkStreamEnums';
import type { ZlinkStreamMetadata } from './ZlinkStreamMetadata';

export interface ZlinkStreamEncodedPayload {
  readonly codec: ZlinkStreamCodec;
  readonly payload: Uint8Array;
  readonly messageType?: Function;
}

export interface ZlinkStreamMessage<TPayload = unknown> {
  readonly name: string;
  readonly metadata: ZlinkStreamMetadata;
  readonly payload: TPayload;
  readonly actorId?: string;
}

export interface ZlinkStreamRequestSendingContext {
  readonly requestPacketName: string;
  readonly actorId?: string;
  setMetadata(key: string, value: string): void;
}

export interface ZlinkStreamReplyReceivedContext {
  readonly requestPacketName: string;
  readonly actorId?: string;
  readonly succeeded: boolean;
  readonly reply?: ZlinkStreamMessage<ZlinkStreamEncodedPayload>;
  readonly error?: ZlinkStreamError;
  readonly elapsed: number;
}

export interface ZlinkStreamHeader {
  readonly kind: ZlinkStreamMessageKind;
  readonly codec: ZlinkStreamCodec;
  readonly flags: ZlinkStreamHeaderFlags;
  readonly requestSeq?: bigint;
  readonly name: string;
  readonly metadata: ZlinkStreamMetadata;
  readonly correlationId?: string;
  readonly actorSlot?: number;
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
