/** Application-visible Framework error categories. */
export enum ZLinkFrameworkErrorKind {
  NotFound = 0,
  AlreadyExists = 1,
  TypeMismatch = 2,
  NotConfigured = 3,
  Rejected = 4,
  Unavailable = 5,
  CapacityExceeded = 6,
  DeadlineExceeded = 7,
  ShuttingDown = 8,
  ProtocolError = 9,
  InvalidOperation = 10,
  DataLost = 11,
  InternalFailure = 12
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
