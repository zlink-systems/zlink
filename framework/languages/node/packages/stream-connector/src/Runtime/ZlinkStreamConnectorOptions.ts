import {
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamCompression,
  ZlinkStreamConnectorOptions,
  ZlinkStreamDispatchMode,
  ZlinkStreamErrorCode,
  ZlinkStreamHeartbeatOptions,
  ZlinkStreamReconnectOptions
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
    throw connectorError(ZlinkStreamErrorCode.ConfigurationError, 'Configured transport conflicts with endpoint scheme.');
  }
  validatePositive(options.connectTimeoutMs ?? 5000, 'ConnectTimeout');
  validatePositive(options.requestTimeoutMs ?? 30000, 'RequestTimeout');
  validatePositive(options.waitTimeoutMs ?? 5000, 'WaitTimeout');
  validatePositive(options.maxSendPayloadSize ?? 64 * 1024, 'MaxSendPayloadSize');
  validatePositive(options.maxReceivePayloadSize ?? 64 * 1024, 'MaxReceivePayloadSize');
  validatePositive(options.maxReceivedMessages ?? 1024, 'MaxReceivedMessages');
  validatePositive(options.maxInboundObserverNotifications ?? 1024, 'MaxInboundObserverNotifications');
  if ((options.maxInboundObserverPayloadPreviewBytes ?? 0) < 0) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'MaxInboundObserverPayloadPreviewBytes must not be negative.');
  }
  validateHeartbeat(options.heartbeat);
  validateReconnect(options.reconnect);

  return {
    endpoint,
    transport: inferredTransport,
    connectTimeoutMs: options.connectTimeoutMs ?? 5000,
    requestTimeoutMs: options.requestTimeoutMs ?? 30000,
    waitTimeoutMs: options.waitTimeoutMs ?? 5000,
    heartbeat: {
      enabled: options.heartbeat?.enabled ?? true,
      intervalMs: options.heartbeat?.intervalMs ?? 1000,
      timeoutMs: options.heartbeat?.timeoutMs ?? 5000
    },
    reconnect: {
      enabled: options.reconnect?.enabled ?? true,
      initialDelayMs: options.reconnect?.initialDelayMs ?? 250,
      maxDelayMs: options.reconnect?.maxDelayMs ?? 5000,
      backoffFactor: options.reconnect?.backoffFactor ?? 2.0,
      maxAttempts: options.reconnect?.maxAttempts ?? 3
    },
    maxSendPayloadSize: options.maxSendPayloadSize ?? 64 * 1024,
    maxReceivePayloadSize: options.maxReceivePayloadSize ?? 64 * 1024,
    maxReceivedMessages: options.maxReceivedMessages ?? 1024,
    maxInboundObserverNotifications: options.maxInboundObserverNotifications ?? 1024,
    maxInboundObserverPayloadPreviewBytes: options.maxInboundObserverPayloadPreviewBytes ?? 0,
    dispatchMode: options.dispatchMode ?? ZlinkStreamDispatchMode.Manual,
    compression: options.compression ?? ZlinkStreamCompression.Lz4,
    compressionCodec: resolveCompressionCodec(options),
    nameResolver: options.nameResolver ?? { resolve: (type) => type.name },
    transportFactory: options.transportFactory ?? defaultTransportFactory,
    codec: options.codec,
    meterProvider: options.meterProvider
  };
}

function resolveCompressionCodec(options: ZlinkStreamConnectorOptions) {
  const compression = options.compression ?? ZlinkStreamCompression.Lz4;
  if (compression === ZlinkStreamCompression.None) {
    if (options.compressionCodec !== undefined) {
      throw connectorError(ZlinkStreamErrorCode.ConfigurationError, 'compressionCodec cannot be set when compression is none.');
    }
    return undefined;
  }
  return options.compressionCodec;
}


function validatePositive(value: number, name: string): void {
  if (value <= 0) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, `${name} must be positive.`);
  }
}

function validateHeartbeat(options: ZlinkStreamHeartbeatOptions | undefined): void {
  const enabled = options?.enabled ?? true;
  const intervalMs = options?.intervalMs ?? 1000;
  const timeoutMs = options?.timeoutMs ?? 5000;
  if (!enabled) {
    return;
  }
  validatePositive(intervalMs, 'Heartbeat interval');
  validatePositive(timeoutMs, 'Heartbeat timeout');
  if (timeoutMs <= intervalMs) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Heartbeat timeout must be greater than the heartbeat interval.');
  }
}

function validateReconnect(options: ZlinkStreamReconnectOptions | undefined): void {
  const enabled = options?.enabled ?? true;
  if (!enabled) {
    return;
  }
  validatePositive(options?.initialDelayMs ?? 250, 'Reconnect InitialDelay');
  validatePositive(options?.maxDelayMs ?? 5000, 'Reconnect MaxDelay');
  if ((options?.backoffFactor ?? 2.0) < 1.0) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Reconnect BackoffFactor must be at least 1.0.');
  }
  if ((options?.maxAttempts ?? 3) <= 0) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Reconnect MaxAttempts must be null or positive.');
  }
}
