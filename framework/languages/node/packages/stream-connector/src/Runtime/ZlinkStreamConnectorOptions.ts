import {
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamCompression,
  ZlinkStreamConnectorOptions,
  ZlinkStreamDiagnosticsLevel,
  ZlinkStreamDispatchMode,
  ZlinkStreamErrorCode,
  ZlinkStreamHeartbeatOptions,
  ZlinkStreamPacketNameResolver,
  ZlinkStreamReconnectOptions
} from '../Contracts';
import { connectorError } from './ZlinkStreamSupport';
import { inferTransport } from './Transport/ZlinkStreamEndpoint';

const validDiagnosticsLevels: ReadonlySet<ZlinkStreamDiagnosticsLevel> = new Set([
  ZlinkStreamDiagnosticsLevel.Off,
  ZlinkStreamDiagnosticsLevel.Errors,
  ZlinkStreamDiagnosticsLevel.Normal,
  ZlinkStreamDiagnosticsLevel.Detailed
]);

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
  validateHeartbeat(options.heartbeat);
  validateReconnect(options.reconnect);
  validateDiagnosticsLevel(options.diagnosticsLevel);

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
      maxAttempts: options.reconnect?.maxAttempts === undefined ? 3 : options.reconnect.maxAttempts
    },
    maxSendPayloadSize: options.maxSendPayloadSize ?? 64 * 1024,
    maxReceivePayloadSize: options.maxReceivePayloadSize ?? 64 * 1024,
    dispatchMode: options.dispatchMode ?? ZlinkStreamDispatchMode.Manual,
    compression: options.compression ?? ZlinkStreamCompression.Lz4,
    compressionCodec: resolveCompressionCodec(options),
    nameResolver: options.nameResolver ?? defaultPacketNameResolver,
    transportFactory: options.transportFactory ?? defaultTransportFactory,
    codec: options.codec,
    // Spec 26 §4: the default diagnostics level is Errors, which preserves
    // the connector's established wire behavior.
    diagnosticsLevel: options.diagnosticsLevel ?? ZlinkStreamDiagnosticsLevel.Errors
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
      throw connectorError(ZlinkStreamErrorCode.ConfigurationError, 'compressionCodec cannot be set when compression is none.');
    }
    return undefined;
  }
  return options.compressionCodec;
}


export function validateDiagnosticsLevel(level: ZlinkStreamDiagnosticsLevel | undefined): void {
  if (level !== undefined && !validDiagnosticsLevels.has(level)) {
    throw connectorError(ZlinkStreamErrorCode.ConfigurationError, 'DiagnosticsLevel is invalid.');
  }
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
  // Spec stream-connector 32 §6: `null` is how this option says "unlimited".
  // Only a stated number is range-checked.
  const maxAttempts = options?.maxAttempts === undefined ? 3 : options.maxAttempts;
  if (maxAttempts !== null && !(Number.isFinite(maxAttempts) && maxAttempts > 0)) {
    throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Reconnect MaxAttempts must be null or positive.');
  }
}
