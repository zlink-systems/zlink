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

export enum ZlinkStreamMessageKind {
  Send = 1,
  Request = 2,
  Response = 3,
  Error = 4,
  Control = 5
}

export enum ZlinkStreamHeaderFlags {
  None = 0,
  HasRequestSeq = 0x01,
  HasMetadata = 0x02,
  PayloadCompressed = 0x04,
  HasCorrelationId = 0x08,
  HasFlowId = 0x10
}

export type ZlinkFlowOrigin = 'Inbound' | 'Timer' | 'Application' | 'Lifecycle';
export type ZlinkStreamCloseReason =
  | 'ClientClose'
  | 'IdleTimeout'
  | 'HeartbeatTimeout'
  | 'ServerDrain'
  | 'ProtocolError'
  | 'TransportError';

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
  UserCallbackFailed = 'userCallbackFailed',
  ObserverFailed = 'observer-failed',
  ObserverDropped = 'observer-dropped',
  ReceivedMessageDropped = 'received-message-dropped',
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
