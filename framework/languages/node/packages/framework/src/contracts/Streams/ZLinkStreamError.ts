export enum ZLinkStreamSessionError {
  Internal = 'internal',
  TransportError = 'transportError'
}

export interface ZLinkStreamError {
  readonly error: ZLinkStreamSessionError;
  readonly message?: string;
}
