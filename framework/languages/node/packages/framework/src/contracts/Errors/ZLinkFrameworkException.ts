/** Application-visible Framework error categories. */
export enum ZLinkFrameworkErrorKind {
  NotFound = 0,
  AlreadyExists = 1,
  TypeMismatch = 2,
  NotConfigured = 3,
  Rejected = 4,
  Unavailable = 5,
  DeadlineExceeded = 6,
  ShuttingDown = 7,
  ProtocolError = 8,
  InvalidOperation = 9,
  DataLost = 10,
  InternalFailure = 11
}

export class ZLinkFrameworkException extends Error {
  constructor(
    public readonly kind: ZLinkFrameworkErrorKind,
    message: string,
    cause?: unknown
  ) {
    super(message, { cause });
    this.name = 'ZLinkFrameworkException';
  }
}
