export { ZlinkStreamMessageKind, ZlinkStreamHeaderFlags } from '@zlink-systems/stream-wire';
export enum ZlinkStreamTransport {
  WebSocket = 'webSocket',
  WebSocketSecure = 'webSocketSecure'
}

export { ZlinkStreamCodec } from '@zlink-systems/stream-wire';

export enum ZlinkStreamCompression {
  None = 'none',
  Lz4 = 'lz4'
}

export enum ZlinkStreamDispatchMode {
  Manual = 'manual',
  Immediate = 'immediate'
}

export type { ZlinkStreamCloseReason } from '@zlink-systems/stream-wire';

export enum ZlinkStreamErrorCode {
  Disconnected = 'disconnected',
  ConfigurationError = 'configurationError',
  ValidationFailed = 'validationFailed',
  RequestTimeout = 'requestTimeout',
  ConnectTimeout = 'connectTimeout',
  FrameDecodeFailed = 'frameDecodeFailed',
  FrameTooLarge = 'frameTooLarge',
  SendFailed = 'sendFailed',
  CompressionFailed = 'compressionFailed',
  DecompressionFailed = 'decompressionFailed',
  TlsValidationFailed = 'tlsValidationFailed',
  UserCallbackFailed = 'userCallbackFailed',
  RemoteError = 'remoteError'
}

export enum ZlinkStreamConnectionState {
  Created = 'created',
  Connecting = 'connecting',
  Connected = 'connected',
  Reconnecting = 'reconnecting',
  Disconnected = 'disconnected',
  Closed = 'closed'
}
