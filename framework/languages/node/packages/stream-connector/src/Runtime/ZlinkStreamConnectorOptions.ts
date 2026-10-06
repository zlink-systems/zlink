import { ZLINK_STREAM_DEFAULT_PAYLOAD_BYTES } from '../Contracts/ZlinkStreamConnectorOptions';
import {
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamCompression,
  ZlinkStreamConnectorOptions,
  ZlinkStreamDispatchMode,
  ZlinkStreamErrorCode,
  ZlinkStreamHeartbeatOptions,
  ZlinkStreamPacketNameResolver,
  ZlinkStreamReconnectOptions
} from '../Contracts';
import { connectorError } from './ZlinkStreamSupport';
import { inferTransport } from './Transport/ZlinkStreamEndpoint';

export { ZLINK_STREAM_DEFAULT_PAYLOAD_BYTES as ZLINK_STREAM_DEFAULT_PAYLOAD_SIZE } from '../Contracts/ZlinkStreamConnectorOptions';

const DEFAULT_CONNECT_TIMEOUT_MS = 5000;
const DEFAULT_REQUEST_TIMEOUT_MS = 30000;
const DEFAULT_WAIT_TIMEOUT_MS = 5000;
const DEFAULT_HEARTBEAT_INTERVAL_MS = 1000;
const DEFAULT_HEARTBEAT_TIMEOUT_MS = 5000;
const DEFAULT_RECONNECT_INITIAL_DELAY_MS = 250;
const DEFAULT_RECONNECT_MAX_DELAY_MS = 5000;
const DEFAULT_RECONNECT_BACKOFF_FACTOR = 2.0;
const DEFAULT_RECONNECT_MAX_ATTEMPTS = 3;

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
  const normalized: RequiredZlinkStreamConnectorOptions = {
    endpoint,
    transport: inferredTransport,
    connectTimeoutMs: options.connectTimeoutMs ?? DEFAULT_CONNECT_TIMEOUT_MS,
    requestTimeoutMs: options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
    waitTimeoutMs: options.waitTimeoutMs ?? DEFAULT_WAIT_TIMEOUT_MS,
    heartbeat: {
      enabled: options.heartbeat?.enabled ?? true,
      intervalMs: options.heartbeat?.intervalMs ?? DEFAULT_HEARTBEAT_INTERVAL_MS,
      timeoutMs: options.heartbeat?.timeoutMs ?? DEFAULT_HEARTBEAT_TIMEOUT_MS
    },
    reconnect: {
      enabled: options.reconnect?.enabled ?? true,
      initialDelayMs: options.reconnect?.initialDelayMs ?? DEFAULT_RECONNECT_INITIAL_DELAY_MS,
      maxDelayMs: options.reconnect?.maxDelayMs ?? DEFAULT_RECONNECT_MAX_DELAY_MS,
      backoffFactor: options.reconnect?.backoffFactor ?? DEFAULT_RECONNECT_BACKOFF_FACTOR,
      maxAttempts:
        options.reconnect?.maxAttempts === undefined
          ? DEFAULT_RECONNECT_MAX_ATTEMPTS
          : options.reconnect.maxAttempts
    },
    maxSendPayloadSize: options.maxSendPayloadSize ?? ZLINK_STREAM_DEFAULT_PAYLOAD_BYTES,
    maxReceivePayloadSize: options.maxReceivePayloadSize ?? ZLINK_STREAM_DEFAULT_PAYLOAD_BYTES,
    dispatchMode: options.dispatchMode ?? ZlinkStreamDispatchMode.Manual,
    compression: options.compression ?? ZlinkStreamCompression.Lz4,
    compressionCodec: options.compressionCodec,
    nameResolver: options.nameResolver ?? defaultPacketNameResolver,
    transportFactory: options.transportFactory ?? defaultTransportFactory,
    codec: options.codec
  };
  validatePositive(normalized.connectTimeoutMs, 'ConnectTimeout');
  validatePositive(normalized.requestTimeoutMs, 'RequestTimeout');
  validatePositive(normalized.waitTimeoutMs, 'WaitTimeout');
  validatePositive(normalized.maxSendPayloadSize, 'MaxSendPayloadSize');
  validatePositive(normalized.maxReceivePayloadSize, 'MaxReceivePayloadSize');
  validateHeartbeat(normalized.heartbeat);
  validateReconnect(normalized.reconnect);
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
  if (!Object.values(ZlinkStreamDispatchMode).includes(normalized.dispatchMode)) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'DispatchMode is invalid.');
  }
  if (!Object.values(ZlinkStreamCompression).includes(normalized.compression)) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Compression is invalid.');
  }

  validateCompressionCodec(normalized);
  return normalized;
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

function validateCompressionCodec(options: RequiredZlinkStreamConnectorOptions): void {
  const compression = options.compression;
  if (compression === ZlinkStreamCompression.None) {
    if (options.compressionCodec !== undefined) {
      throw connectorError(
        ZlinkStreamErrorCode.ConfigurationError,
        'compressionCodec cannot be set when compression is none.'
      );
    }
  }
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

function validateHeartbeat(options: Required<ZlinkStreamHeartbeatOptions>): void {
  const intervalMs = options.intervalMs;
  const timeoutMs = options.timeoutMs;
  validatePositive(intervalMs, 'Heartbeat interval');
  validatePositive(timeoutMs, 'Heartbeat timeout');
}

function validateReconnect(options: Required<ZlinkStreamReconnectOptions>): void {
  validatePositive(options.initialDelayMs, 'Reconnect InitialDelay');
  validatePositive(options.maxDelayMs, 'Reconnect MaxDelay');
  validatePositive(options.backoffFactor, 'Reconnect BackoffFactor');
  // Spec stream-connector 32 §6: `null` is how this option says "unlimited".
  // Only a stated number is range-checked.
  const maxAttempts = options.maxAttempts;
  if (maxAttempts !== null && !(Number.isFinite(maxAttempts) && maxAttempts > 0)) {
    throw connectorError(
      ZlinkStreamErrorCode.ValidationFailed,
      'Reconnect MaxAttempts must be null or positive.'
    );
  }
}
