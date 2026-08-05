export interface CodecRequesterOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly targetEndpoint: string;
}

export function validateCodecRequesterOptions(value: unknown): CodecRequesterOptions {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw new Error("Configuration section 'e2e' must be an object.");
  const values = value as Record<string, unknown>;
  return {
    rid: optional(values, 'rid') ?? 'codec-requester',
    httpUrl: required(values, 'httpUrl'),
    logDir: optional(values, 'logDir') ?? 'logs',
    targetEndpoint: required(values, 'targetEndpoint')
  };
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
