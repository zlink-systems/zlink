import {
  RequiredZlinkStreamConnectorOptions,
  ZLINK_STREAM_DEFAULT_SEND_PAYLOAD_BYTES,
  ZlinkStreamCompression,
  ZlinkStreamConnectorOptions,
  ZlinkStreamDispatchMode,
  ZlinkStreamErrorCode,
  ZlinkStreamPacketNameResolver
} from '../Contracts';
import { connectorError } from './ZlinkStreamSupport';
import { inferTransport } from './Transport/ZlinkStreamEndpoint';

export function normalizeOptions(
  options: ZlinkStreamConnectorOptions,
  defaultTransportFactory: RequiredZlinkStreamConnectorOptions['transportFactory']
): RequiredZlinkStreamConnectorOptions {
  const endpoint = options.endpoint;
  if (endpoint.trim().length === 0) {
    throw connectorError(ZlinkStreamErrorCode.ConfigurationError, 'Endpoint must not be empty.');
  }
  const inferredTransport = inferTransport(endpoint);
  if (options.transport !== undefined && options.transport !== inferredTransport) {
    throw connectorError(
      ZlinkStreamErrorCode.ConfigurationError,
      'Configured transport conflicts with endpoint scheme.'
    );
  }
  const connectTimeoutMs = options.connectTimeoutMs ?? 5000;
  validatePositive(connectTimeoutMs, 'ConnectTimeout');
  const requestTimeoutMs = options.requestTimeoutMs ?? 30000;
  validatePositive(requestTimeoutMs, 'RequestTimeout');
  const waitTimeoutMs = options.waitTimeoutMs ?? 5000;
  validatePositive(waitTimeoutMs, 'WaitTimeout');
  const maxSendPayloadSize = options.maxSendPayloadSize ?? ZLINK_STREAM_DEFAULT_SEND_PAYLOAD_BYTES;
  validatePositive(maxSendPayloadSize, 'MaxSendPayloadSize');
  const maxReceivePayloadSize = options.maxReceivePayloadSize ?? 64 * 1024;
  validatePositive(maxReceivePayloadSize, 'MaxReceivePayloadSize');
  const heartbeatOptions = options.heartbeat;
  const heartbeatIntervalMs = heartbeatOptions?.intervalMs ?? 1000;
  const heartbeatTimeoutMs = heartbeatOptions?.timeoutMs ?? 5000;
  validatePositive(heartbeatIntervalMs, 'Heartbeat interval');
  validatePositive(heartbeatTimeoutMs, 'Heartbeat timeout');
  const reconnectOptions = options.reconnect;
  const initialDelayMs = reconnectOptions?.initialDelayMs ?? 250;
  validatePositive(initialDelayMs, 'Reconnect InitialDelay');
  const maxDelayMs = reconnectOptions?.maxDelayMs ?? 5000;
  validatePositive(maxDelayMs, 'Reconnect MaxDelay');
  const backoffFactor = reconnectOptions?.backoffFactor ?? 2.0;
  validatePositive(backoffFactor, 'Reconnect BackoffFactor');
  const maxAttempts =
    reconnectOptions?.maxAttempts === undefined ? 3 : reconnectOptions.maxAttempts;
  if (maxAttempts !== null && !(Number.isFinite(maxAttempts) && maxAttempts > 0)) {
    throw connectorError(
      ZlinkStreamErrorCode.ValidationFailed,
      'Reconnect MaxAttempts must be null or positive.'
    );
  }
  if (options.heartbeat?.enabled !== undefined && typeof options.heartbeat.enabled !== 'boolean') {
    throw connectorError(
      ZlinkStreamErrorCode.ValidationFailed,
      'Heartbeat enabled must be boolean.'
    );
  }
  if (options.reconnect?.enabled !== undefined && typeof options.reconnect.enabled !== 'boolean') {
    throw connectorError(
      ZlinkStreamErrorCode.ValidationFailed,
      'Reconnect enabled must be boolean.'
    );
  }
  if (
    options.compressionCodec !== undefined &&
    !hasMethods(options.compressionCodec, 'compress', 'decompress')
  ) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Compression codec is invalid.');
  }
  if (options.codec !== undefined && !hasMethods(options.codec, 'encode', 'decode')) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Payload codec is invalid.');
  }
  if (options.nameResolver !== undefined && !hasMethods(options.nameResolver, 'resolve')) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Packet name resolver is invalid.');
  }
  if (options.transportFactory !== undefined && !hasMethods(options.transportFactory, 'connect')) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Transport factory is invalid.');
  }
  if (
    !Object.values(ZlinkStreamDispatchMode).includes(
      options.dispatchMode ?? ZlinkStreamDispatchMode.Manual
    )
  ) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'DispatchMode is invalid.');
  }
  if (
    !Object.values(ZlinkStreamCompression).includes(
      options.compression ?? ZlinkStreamCompression.Lz4
    )
  ) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Compression is invalid.');
  }

  const heartbeat = {
    enabled: heartbeatOptions?.enabled ?? true,
    intervalMs: heartbeatIntervalMs,
    timeoutMs: heartbeatTimeoutMs
  };
  const reconnect = {
    enabled: reconnectOptions?.enabled ?? true,
    initialDelayMs,
    maxDelayMs,
    backoffFactor,
    maxAttempts
  };
  return {
    endpoint,
    transport: inferredTransport,
    connectTimeoutMs,
    requestTimeoutMs,
    waitTimeoutMs,
    heartbeat,
    reconnect,
    maxSendPayloadSize,
    maxReceivePayloadSize,
    dispatchMode: options.dispatchMode ?? ZlinkStreamDispatchMode.Manual,
    compression: options.compression ?? ZlinkStreamCompression.Lz4,
    compressionCodec: resolveCompressionCodec(options),
    nameResolver: options.nameResolver ?? defaultPacketNameResolver,
    transportFactory: options.transportFactory ?? defaultTransportFactory,
    codec: options.codec
  };
}

/**
 * Spec stream-connector 32 §5 and the TypeScript projection §4: a payload type
 * carries its packet name in a static `packetName` member, and that name wins
 * over the type's own name.
 *
 * The `name` fallback is a convenience for a type that declares nothing, and it
 * is NOT trustworthy in a browser build: a minifier renames the constructor, so
 * the same type produces a different packet name after a build setting changes
 * and the server stops finding the handler — exactly the "name that changes
 * with the build environment" §5 forbids. A payload type that crosses the wire
 * declares `static readonly packetName`; leaving it out is only safe while the
 * bundle keeps class names.
 */
export const defaultPacketNameResolver: ZlinkStreamPacketNameResolver = {
  resolve(payloadType: Function): string {
    const declared = (payloadType as { readonly packetName?: unknown }).packetName;
    if (typeof declared === 'string' && declared.length > 0) {
      return declared;
    }
    return payloadType.name;
  }
};

function resolveCompressionCodec(options: ZlinkStreamConnectorOptions) {
  const compression = options.compression ?? ZlinkStreamCompression.Lz4;
  if (compression === ZlinkStreamCompression.None) {
    if (options.compressionCodec !== undefined) {
      throw connectorError(
        ZlinkStreamErrorCode.ConfigurationError,
        'compressionCodec cannot be set when compression is none.'
      );
    }
    return undefined;
  }
  return options.compressionCodec;
}

function validatePositive(value: number, name: string): void {
  if (!Number.isFinite(value) || value <= 0) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, `${name} must be positive.`);
  }
}

function hasMethods(value: unknown, ...names: string[]): boolean {
  if (value === null || typeof value !== 'object') return false;
  const candidate = value as Record<string, unknown>;
  return names.every((name) => typeof candidate[name] === 'function');
}
