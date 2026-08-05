import { objectValues, optionalString, requiredString } from '../../../configuration';

export interface ConsumerOptions {
  readonly httpUrl: string;
  readonly logDir: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly storeResponseGate: boolean;
  readonly traceLabel: string;
}

export function validateConsumerOptions(value: unknown): ConsumerOptions {
  const values = objectValues(value);
  return {
    httpUrl: optionalString(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optionalString(values, 'logDir') ?? 'logs',
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    storeResponseGate: optionalString(values, 'storeResponseGate') === 'enabled',
    traceLabel: optionalString(values, 'traceLabel') ?? 'consumer'
  };
}
