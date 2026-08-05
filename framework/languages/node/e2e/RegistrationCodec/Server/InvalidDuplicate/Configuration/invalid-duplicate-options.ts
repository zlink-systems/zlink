export interface InvalidDuplicateOptions {
  readonly rid: string;
  readonly logDir: string;
  readonly channelEndpoint: string;
  readonly invalidCase: 'duplicate' | 'missing-handler-group' | 'mixed-channel-kinds';
}

export function validateInvalidDuplicateOptions(value: unknown): InvalidDuplicateOptions {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw new Error("Configuration section 'e2e' must be an object.");
  const values = value as Record<string, unknown>;
  return {
    rid: optional(values, 'rid') ?? 'invalid-duplicate',
    logDir: optional(values, 'logDir') ?? 'logs',
    channelEndpoint: required(values, 'channelEndpoint'),
    invalidCase: invalidCase(values.invalidCase)
  };
}

function invalidCase(value: unknown): InvalidDuplicateOptions['invalidCase'] {
  if (value === 'duplicate' || value === 'missing-handler-group' || value === 'mixed-channel-kinds') {
    return value;
  }
  throw new Error("Configuration value 'e2e.invalidCase' is invalid.");
}

function optional(values: Record<string, unknown>, name: string): string | undefined {
  const value = values[name];
  return typeof value === 'string' && value.length > 0 ? value : undefined;
}
function required(values: Record<string, unknown>, name: string): string {
  const value = optional(values, name);
  if (value === undefined) throw new Error(`Configuration value 'e2e.${name}' must be a non-empty string.`);
  return value;
}
