export interface JsonOnlyOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly channelEndpoint: string;
  readonly evidenceFile?: string;
}

export function validateJsonOnlyOptions(value: unknown): JsonOnlyOptions {
  const values = asObject(value);
  return {
    rid: optional(values, 'rid') ?? 'json-only-peer',
    httpUrl: required(values, 'httpUrl'),
    logDir: optional(values, 'logDir') ?? 'logs',
    channelEndpoint: required(values, 'channelEndpoint'),
    evidenceFile: optional(values, 'evidenceFile')
  };
}

function asObject(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw new Error("Configuration section 'e2e' must be an object.");
  return value as Record<string, unknown>;
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
