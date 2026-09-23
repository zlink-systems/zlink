import type {
  ZlinkStreamCompression,
  ZlinkStreamDispatchMode,
  ZlinkStreamTransport
} from './ZlinkStreamEnums';
import type { ZlinkStreamEncodedPayload } from './ZlinkStreamModels';

export interface ZlinkStreamConnectorOptions {
  readonly endpoint: string;
  readonly codec?: ZlinkStreamPayloadCodec;
  readonly transport?: ZlinkStreamTransport;
  readonly transportFactory?: ZlinkStreamTransportFactory;
  readonly connectTimeoutMs?: number;
  readonly requestTimeoutMs?: number;
  readonly waitTimeoutMs?: number;
  readonly heartbeat?: ZlinkStreamHeartbeatOptions;
  readonly reconnect?: ZlinkStreamReconnectOptions;
  readonly maxSendPayloadSize?: number;
  readonly maxReceivePayloadSize?: number;
  readonly dispatchMode?: ZlinkStreamDispatchMode;
  readonly compression?: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver?: ZlinkStreamPacketNameResolver;
}

export interface ZlinkStreamHeartbeatOptions {
  readonly enabled?: boolean;
  readonly intervalMs?: number;
  readonly timeoutMs?: number;
}

export interface ZlinkStreamReconnectOptions {
  readonly enabled?: boolean;
  readonly initialDelayMs?: number;
  readonly maxDelayMs?: number;
  readonly backoffFactor?: number;
  /**
   * `null` means unlimited (spec stream-connector 32 §6: the option has to be
   * able to say "keep trying"); any other value must be a positive number.
   */
  readonly maxAttempts?: number | null;
}

export interface ZlinkStreamPacketNameResolver {
  resolve(payloadType: Function): string;
}

export interface ZlinkStreamPayloadCodec {
  encode(payload: unknown, messageType?: Function): ZlinkStreamEncodedPayload;
  decode<T = unknown>(payload: ZlinkStreamEncodedPayload, messageType?: Function): T;
}

export interface ZlinkStreamCompressionCodec {
  compress(payload: Uint8Array): Uint8Array;
  decompress(payload: Uint8Array, maxDecompressedSize: number): Uint8Array;
}

export interface ZlinkStreamTransportFactory {
  connect(
    options: RequiredZlinkStreamConnectorOptions,
    signal?: AbortSignal
  ): Promise<ZlinkStreamConnection>;
}

export interface ZlinkStreamConnection {
  write(frame: Uint8Array, signal?: AbortSignal): Promise<void>;
  read?(signal?: AbortSignal): Promise<Uint8Array | undefined>;
  close(signal?: AbortSignal): Promise<void>;
}

export interface RequiredZlinkStreamConnectorOptions {
  readonly endpoint: string;
  readonly transport: ZlinkStreamTransport;
  readonly connectTimeoutMs: number;
  readonly requestTimeoutMs: number;
  readonly waitTimeoutMs: number;
  readonly heartbeat: Required<ZlinkStreamHeartbeatOptions>;
  readonly reconnect: Required<ZlinkStreamReconnectOptions>;
  readonly maxSendPayloadSize: number;
  readonly maxReceivePayloadSize: number;
  readonly dispatchMode: ZlinkStreamDispatchMode;
  readonly compression: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver: ZlinkStreamPacketNameResolver;
  readonly transportFactory: ZlinkStreamTransportFactory;
  readonly codec?: ZlinkStreamPayloadCodec;
}
