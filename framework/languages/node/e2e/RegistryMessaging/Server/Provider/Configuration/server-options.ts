export interface ServerOptions {
  readonly role: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly evidenceFile?: string;
  readonly rid: string;
  readonly redisEndpoint?: string;
  readonly redisKeyPrefix?: string;
  readonly channelEndpoint?: string;
  readonly manualClientEndpoint?: string;
  readonly routeEndpoint?: string;
  readonly routePeers: readonly string[];
  readonly weight: number;
}

export function validateServerOptions(value: unknown, defaultRole = 'provider'): ServerOptions {
  const values = objectValues(value);
  const rid = optionalString(values, 'rid') ?? 'node';
  const routePeers = Array.isArray(values.routePeers)
    ? values.routePeers.filter((entry): entry is string => typeof entry === 'string' && entry.length > 0)
    : [];
  return {
    role: defaultRole,
    httpUrl: optionalString(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optionalString(values, 'logDir') ?? '/tmp/zlink-node-e2e-log',
    evidenceFile: optionalString(values, 'evidenceFile'),
    rid,
    redisEndpoint: optionalString(values, 'redisEndpoint'),
    redisKeyPrefix: optionalString(values, 'redisKeyPrefix'),
    channelEndpoint: optionalString(values, 'channelEndpoint'),
    manualClientEndpoint: optionalString(values, 'manualClientEndpoint'),
    routeEndpoint: optionalString(values, 'routeEndpoint'),
    routePeers,
    weight: parseWeight(values.weight)
  };
}

function parseWeight(value: unknown): number {
  if (value === undefined) {
    return 100;
  }
  const weight = Number(value);
  if (!Number.isInteger(weight) || weight < 0 || weight > 100) {
    throw new Error('--weight must be an integer between 0 and 100.');
  }
  return weight;
}

import { objectValues, optionalString } from '../../../configuration';
