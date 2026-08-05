export interface ConsumerOptions {
  readonly httpUrl: string;
  readonly logDir: string;
  readonly traceLabel: string;
  readonly providerEndpoints: readonly string[];
  readonly redisEndpoint?: string;
  readonly redisKeyPrefix?: string;
}

export function validateConsumerOptions(value: unknown): ConsumerOptions {
  const values = objectValues(value);
  const providerEndpoints = Array.isArray(values.providerEndpoints)
    ? values.providerEndpoints.filter((entry): entry is string => typeof entry === 'string' && entry.length > 0)
    : [];
  const redisEndpoint = optionalString(values, 'redisEndpoint');
  const redisKeyPrefix = optionalString(values, 'redisKeyPrefix');
  if (providerEndpoints.length === 0 && (redisEndpoint === undefined || redisKeyPrefix === undefined)) {
    throw new Error('--provider-endpoint or --redis-endpoint/--redis-key-prefix is required.');
  }
  return {
    httpUrl: optionalString(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optionalString(values, 'logDir') ?? '/tmp/zlink-node-e2e-log',
    traceLabel: optionalString(values, 'traceLabel') ?? 'consumer',
    providerEndpoints,
    redisEndpoint,
    redisKeyPrefix
  };
}
import { objectValues, optionalString } from '../../../configuration';
