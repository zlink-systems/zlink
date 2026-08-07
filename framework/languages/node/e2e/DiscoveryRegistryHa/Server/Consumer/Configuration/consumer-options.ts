import { objectValues, optionalString, requiredString } from '../../../configuration';

export interface ConsumerOptions {
  readonly httpUrl: string;
  readonly logDir: string;
  readonly evidenceFile?: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly storeResponseGate: boolean;
  readonly traceLabel: string;
  readonly multiRole: boolean;
}

export function validateConsumerOptions(value: unknown): ConsumerOptions {
  const values = objectValues(value);
  return {
    httpUrl: optionalString(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optionalString(values, 'logDir') ?? 'logs',
    evidenceFile: optionalString(values, 'evidenceFile'),
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    storeResponseGate: optionalString(values, 'storeResponseGate') === 'enabled',
    traceLabel: optionalString(values, 'traceLabel') ?? 'consumer',
    multiRole: optionalString(values, 'multiRole') === 'enabled'
  };
}
