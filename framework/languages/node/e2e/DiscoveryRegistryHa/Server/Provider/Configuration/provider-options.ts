import { objectValues, optionalString, requiredString } from '../../../configuration';

export interface ProviderOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly channelEndpoint: string;
  readonly fanoutEndpoint?: string;
  readonly multiRole: boolean;
  readonly capacityProfile: string;
  readonly evidenceFile?: string;
}

export function validateProviderOptions(value: unknown): ProviderOptions {
  const values = objectValues(value);
  return {
    rid: optionalString(values, 'rid') ?? 'api-a',
    httpUrl: optionalString(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optionalString(values, 'logDir') ?? 'logs',
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    channelEndpoint: requiredString(values, 'channelEndpoint'),
    fanoutEndpoint: optionalString(values, 'fanoutEndpoint'),
    multiRole: optionalString(values, 'multiRole') === 'enabled',
    capacityProfile: optionalString(values, 'capacityProfile') ?? 'default',
    evidenceFile: optionalString(values, 'evidenceFile')
  };
}
