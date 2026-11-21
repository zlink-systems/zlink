export enum ZLinkStreamSessionError {
  Internal = 'internal',
  TransportError = 'transportError'
}

export enum ZLinkDisconnectReason {
  Unknown = 0,
  HandshakeFailed = 3,
  TransportError = 4,
  ContextTerminated = 5
}

export interface ZLinkStreamError {
  readonly error: ZLinkStreamSessionError;
  readonly message?: string;
  readonly disconnectReason?: ZLinkDisconnectReason;
}
