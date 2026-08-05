export interface ClientOptions {
  readonly playAUrl: string;
  readonly playBUrl: string;
  readonly gatewayUrl: string;
  readonly sessionAUrl: string;
  readonly sessionBUrl: string;
  readonly sessionAStreamEndpoint: string;
  readonly sessionATlsStreamEndpoint: string;
  readonly sessionBStreamEndpoint: string;
  readonly multiAUrl: string;
  readonly multiBUrl: string;
  readonly scenario: string;
}

export function parseClientOptions(value: unknown): ClientOptions {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw new Error('Client configuration must be an object.');
  const values = value as Record<string, unknown>;
  return {
    playAUrl: required(values, 'playAUrl'),
    playBUrl: required(values, 'playBUrl'),
    gatewayUrl: required(values, 'gatewayUrl'),
    sessionAUrl: required(values, 'sessionAUrl'),
    sessionBUrl: required(values, 'sessionBUrl'),
    sessionAStreamEndpoint: required(values, 'sessionAStreamEndpoint'),
    sessionATlsStreamEndpoint: optional(values, 'sessionATlsStreamEndpoint', ''),
    sessionBStreamEndpoint: required(values, 'sessionBStreamEndpoint'),
    multiAUrl: optional(values, 'multiAUrl', 'http://127.0.0.1:0'),
    multiBUrl: optional(values, 'multiBUrl', 'http://127.0.0.1:0'),
    scenario: optional(values, 'scenario', 'all')
  };
}

function required(values: Record<string, unknown>, key: string): string {
  const value = values[key];
  if (typeof value !== 'string' || value.length === 0) throw new Error(`Configuration value 'e2e.${key}' is required.`);
  return value;
}

function optional(values: Record<string, unknown>, key: string, fallback: string): string {
  const value = values[key];
  if (value === undefined) return fallback;
  if (typeof value !== 'string') throw new Error(`Configuration value 'e2e.${key}' must be a string.`);
  return value;
}
