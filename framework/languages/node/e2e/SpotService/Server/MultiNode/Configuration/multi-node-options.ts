export interface MultiNodeOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly routeEndpoint: string;
  readonly spotRouterEndpoint: string;
  readonly spotPubEndpoint?: string;
  readonly peerSpotRouterEndpoint?: string;
  readonly redisEndpoint?: string;
  readonly redisKeyPrefix?: string;
  readonly spotOnly: boolean;
  readonly evidenceFile?: string;
  readonly logDir: string;
}

export function validateMultiNodeOptions(value: unknown): MultiNodeOptions {
  const values = objectValues(value);
  return {
    rid: requiredString(values, 'rid'),
    httpUrl: requiredString(values, 'httpUrl'),
    routeEndpoint: requiredString(values, 'routeEndpoint'),
    spotRouterEndpoint: requiredString(values, 'spotRouterEndpoint'),
    spotPubEndpoint: optionalString(values, 'spotPubEndpoint'),
    peerSpotRouterEndpoint: optionalString(values, 'peerSpotRouterEndpoint'),
    redisEndpoint: optionalString(values, 'redisEndpoint'),
    redisKeyPrefix: optionalString(values, 'redisKeyPrefix'),
    spotOnly: optionalBoolean(values, 'spotOnly'),
    evidenceFile: optionalString(values, 'evidenceFile'),
    logDir: requiredString(values, 'logDir')
  };
}
import { objectValues, optionalBoolean, optionalString, requiredString } from '../../../configuration';
