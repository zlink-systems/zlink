export interface ClientOptions {
  readonly actorUrl: string;
  readonly callerUrl: string;
  readonly sessionUrl: string;
  readonly sessionStreamEndpoint: string;
  readonly scenario: string;
}

export function parseClientOptions(value: unknown): ClientOptions {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw new Error('Client configuration must be an object.');
  const values = value as Record<string, unknown>;
  const optional = (key: string, fallback: string): string => typeof values[key] === 'string' ? values[key] as string : fallback;
  return {
    actorUrl: optional('actorUrl', 'http://127.0.0.1:0'),
    callerUrl: optional('callerUrl', 'http://127.0.0.1:0'),
    sessionUrl: optional('sessionUrl', 'http://127.0.0.1:0'),
    sessionStreamEndpoint: optional('sessionStreamEndpoint', 'ws://127.0.0.1:0'),
    scenario: optional('scenario', 'all')
  };
}
