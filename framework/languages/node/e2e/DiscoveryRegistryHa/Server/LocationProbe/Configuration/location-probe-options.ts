import { objectValues, optionalString, requiredString } from '../../../configuration';

export interface LocationProbeOptions {
  readonly rid: string;
  readonly probeId: number;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly peers: readonly string[];
}

export function validateLocationProbeOptions(value: unknown): LocationProbeOptions {
  const values = objectValues(value);
  const probeId = values.probeId === undefined ? 1 : Number(values.probeId);
  if (!Number.isInteger(probeId)) throw new Error("Configuration value 'e2e.probeId' must be an integer.");
  return {
    rid: optionalString(values, 'rid') ?? 'reg-1',
    probeId,
    httpUrl: optionalString(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optionalString(values, 'logDir') ?? 'logs',
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    peers: Array.isArray(values.peers) ? values.peers.filter((entry): entry is string => typeof entry === 'string' && entry.length > 0) : []
  };
}
