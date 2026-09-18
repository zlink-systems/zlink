import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '@zlink-systems/framework';

// --8<-- [start:error-mapping]
// A framework call fails by throwing ZLinkFrameworkException. Left alone, it
// reaches the HTTP surface's default handler and every failure looks like a
// 500 -- the caller cannot tell "no node is available right now" from "this
// server has a bug". This wrapper turns the error kind into the status code
// that says what actually happened.
export interface HttpResult {
  readonly status: number;
  readonly body?: unknown;
}

// The same table the framework's own HTTP host uses.
function statusFor(kind: ZLinkFrameworkErrorKind): number {
  switch (kind) {
    case ZLinkFrameworkErrorKind.ProtocolError:
    case ZLinkFrameworkErrorKind.TypeMismatch:
    case ZLinkFrameworkErrorKind.InvalidOperation:
      return 400;
    case ZLinkFrameworkErrorKind.NotFound:
      return 404;
    case ZLinkFrameworkErrorKind.AlreadyExists:
      return 409;
    case ZLinkFrameworkErrorKind.Rejected:
      return 403;
    // No node can take the call now. The caller may retry.
    case ZLinkFrameworkErrorKind.NotConfigured:
    case ZLinkFrameworkErrorKind.Unavailable:
    case ZLinkFrameworkErrorKind.ShuttingDown:
      return 503;
    case ZLinkFrameworkErrorKind.DeadlineExceeded:
      return 504;
    default:
      return 500;
  }
}

function nameFor(kind: ZLinkFrameworkErrorKind): string {
  switch (kind) {
    case ZLinkFrameworkErrorKind.NotFound:
      return 'not_found';
    case ZLinkFrameworkErrorKind.AlreadyExists:
      return 'already_exists';
    case ZLinkFrameworkErrorKind.TypeMismatch:
      return 'type_mismatch';
    case ZLinkFrameworkErrorKind.NotConfigured:
      return 'not_configured';
    case ZLinkFrameworkErrorKind.Rejected:
      return 'rejected';
    case ZLinkFrameworkErrorKind.Unavailable:
      return 'unavailable';
    case ZLinkFrameworkErrorKind.DeadlineExceeded:
      return 'deadline_exceeded';
    case ZLinkFrameworkErrorKind.ShuttingDown:
      return 'shutting_down';
    case ZLinkFrameworkErrorKind.ProtocolError:
      return 'protocol_error';
    case ZLinkFrameworkErrorKind.InvalidOperation:
      return 'invalid_operation';
    case ZLinkFrameworkErrorKind.DataLost:
      return 'data_lost';
    default:
      return 'internal_failure';
  }
}

// Wrap the route dispatch once, ahead of the endpoints, so it covers every
// call below it. Nothing else in the process is a framework error, so an error
// of any other type is rethrown for the surface's own handler.
export async function withZLinkErrorResponse(
  next: () => Promise<HttpResult>
): Promise<HttpResult> {
  try {
    return await next();
  } catch (error: unknown) {
    if (!(error instanceof ZLinkFrameworkException)) throw error;
    return {
      status: statusFor(error.kind),
      body: { error: nameFor(error.kind), message: error.message }
    };
  }
}
// --8<-- [end:error-mapping]
