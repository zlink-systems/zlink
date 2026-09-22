const ERROR_MESSAGE_MAX_LENGTH = 512;

const CREDENTIAL_PATTERNS: ReadonlyArray<readonly [RegExp, string]> = [
  [/Authorization\s*:\s*(?:(?:Bearer|Basic)\s+)?[^\s,;]+/gi, 'Authorization: <redacted>'],
  [/Bearer\s+[^\s,;]+/gi, 'Bearer <redacted>'],
  [/password\s*=\s*[^\s,;]+/gi, 'password=<redacted>'],
  [/token\s*=\s*[^\s,;]+/gi, 'token=<redacted>']
];

export function dispatchErrorDetails(error: unknown): {
  readonly errorType?: string;
  readonly errorMessage?: string;
} {
  if (error === undefined || error === null) return {};
  if (error instanceof Error) {
    return {
      errorType: error.name,
      errorMessage: sanitizeErrorMessage(error.message)
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

function sanitizeErrorMessage(message: string | undefined): string | undefined {
  if (message === undefined || message.length === 0) return undefined;
  const lineEnd = message.search(/[\r\n]/u);
  let sanitized = lineEnd < 0 ? message : message.slice(0, lineEnd);
  for (const [pattern, replacement] of CREDENTIAL_PATTERNS) {
    pattern.lastIndex = 0;
    sanitized = sanitized.replace(pattern, replacement);
  }
  return sanitized.slice(0, ERROR_MESSAGE_MAX_LENGTH);
}
