import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts/Errors/ZLinkFrameworkException';
import { ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason } from '../../contracts/Dispatch/ZLinkDispatchOptions';
import {
  internalFrameworkErrorKind,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';

export function dispatchReasonFromError(error: unknown): ZLinkDispatchErrorReason {
  if (!(error instanceof ZLinkFrameworkException)) return ZLinkDispatchErrorReason.HandlerException;
  const origin = internalFrameworkErrorKind(error);
  if (origin === ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed)
    return ZLinkDispatchErrorReason.PayloadDecodeFailed;
  if (
    origin === ZLinkFrameworkInternalErrorKind.SpotGenerationStale ||
    origin === ZLinkFrameworkInternalErrorKind.ActorGenerationStale ||
    origin === ZLinkFrameworkInternalErrorKind.SpotMoving ||
    origin === ZLinkFrameworkInternalErrorKind.ActorMoving ||
    origin === ZLinkFrameworkInternalErrorKind.ActorLocationStale
  )
    return ZLinkDispatchErrorReason.StaleTarget;
  switch (error.kind) {
    case ZLinkFrameworkErrorKind.NotFound:
      return ZLinkDispatchErrorReason.HandlerMissing;
    case ZLinkFrameworkErrorKind.ProtocolError:
      return ZLinkDispatchErrorReason.InvalidFrame;
    case ZLinkFrameworkErrorKind.ShuttingDown:
      return ZLinkDispatchErrorReason.Shutdown;
    default:
      return ZLinkDispatchErrorReason.HandlerException;
  }
}

export const ERROR_MESSAGE_MAX_LENGTH = 512;

const CREDENTIAL_PATTERNS: ReadonlyArray<readonly [RegExp, string]> = [
  [/Authorization\s*:\s*(?:(?:Bearer|Basic)\s+)?[^\s,;]+/gi, 'Authorization: <redacted>'],
  [/Bearer\s+[^\s,;]+/gi, 'Bearer <redacted>'],
  [/password\s*=\s*[^\s,;]+/gi, 'password=<redacted>'],
  [/token\s*=\s*[^\s,;]+/gi, 'token=<redacted>']
];

export function dispatchErrorDetails(error: unknown): {
  readonly errorType?: string;
  readonly errorMessage?: string;
  readonly errorCauseType?: string;
  readonly errorCauseMessage?: string;
} {
  if (error === undefined || error === null) return {};
  if (error instanceof Error) {
    const cause = deepestErrorCause(error);
    return {
      errorType: error.name,
      errorMessage: sanitizeErrorMessage(error.message),
      ...(cause === error
        ? {}
        : {
            errorCauseType: cause instanceof Error ? cause.name : typeof cause,
            errorCauseMessage: sanitizeErrorMessage(
              cause instanceof Error ? cause.message : String(cause)
            )
          })
    };
  }
  const errorType = typeof error === 'object' ? error.constructor.name : typeof error;
  const message =
    typeof error === 'string' ||
    typeof error === 'number' ||
    typeof error === 'boolean' ||
    typeof error === 'bigint'
      ? `${error}`
      : Object.prototype.toString.call(error);
  return { errorType, errorMessage: sanitizeErrorMessage(message) };
}

function deepestErrorCause(error: Error): unknown {
  let current: unknown = error;
  const seen = new Set<unknown>();
  while (current instanceof Error && current.cause !== undefined && !seen.has(current)) {
    seen.add(current);
    current = current.cause;
  }
  return current;
}

function sanitizeErrorMessage(message: string): string {
  if (message.length === 0) return '';
  const lineEnd = message.search(/[\r\n]/u);
  let sanitized = lineEnd < 0 ? message : message.slice(0, lineEnd);
  for (const [pattern, replacement] of CREDENTIAL_PATTERNS) {
    pattern.lastIndex = 0;
    sanitized = sanitized.replace(pattern, replacement);
  }
  return sanitized.slice(0, ERROR_MESSAGE_MAX_LENGTH);
}
