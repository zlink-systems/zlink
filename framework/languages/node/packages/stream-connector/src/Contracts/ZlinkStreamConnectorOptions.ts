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
  readonly maxReceivedMessages?: number;
  readonly maxInboundObserverNotifications?: number;
  readonly maxInboundObserverPayloadPreviewBytes?: number;
  readonly dispatchMode?: ZlinkStreamDispatchMode;
  readonly compression?: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver?: ZlinkStreamPacketNameResolver;
  readonly meterProvider?: ZlinkStreamMeterProvider;
}

export interface ZlinkStreamMeterProvider {
  getMeter(name: string): {
    createCounter(name: string, options?: { readonly unit?: string }): {
      add(value: number, attributes?: Readonly<Record<string, string | number | boolean>>): void;
    };
    createHistogram(name: string, options?: { readonly unit?: string }): {
      record(value: number, attributes?: Readonly<Record<string, string | number | boolean>>): void;
    };
  };
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
  readonly maxAttempts?: number;
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
  connect(options: RequiredZlinkStreamConnectorOptions, signal?: AbortSignal): Promise<ZlinkStreamConnection>;
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
  readonly maxReceivedMessages: number;
  readonly maxInboundObserverNotifications: number;
  readonly maxInboundObserverPayloadPreviewBytes: number;
  readonly dispatchMode: ZlinkStreamDispatchMode;
  readonly compression: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver: ZlinkStreamPacketNameResolver;
  readonly transportFactory: ZlinkStreamTransportFactory;
  readonly codec?: ZlinkStreamPayloadCodec;
  readonly meterProvider?: ZlinkStreamMeterProvider;
}
