export interface PlayOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly controlRouterEndpoint: string;
  readonly externalSpotEndpoint: string;
  readonly spotRouterEndpoint: string;
  readonly spotPubEndpoint: string;
  readonly clientSpotPubEndpoints: readonly string[];
  readonly playAExternalSpotEndpoint?: string;
  readonly externalClientEndpoint?: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly evidenceFile?: string;
  readonly logDir: string;
}

export function validatePlayOptions(value: unknown): PlayOptions {
  const values = objectValues(value);
  return {
    rid: requiredString(values, 'rid'),
    httpUrl: requiredString(values, 'httpUrl'),
    controlRouterEndpoint: requiredString(values, 'controlRouterEndpoint'),
    externalSpotEndpoint: requiredString(values, 'externalSpotEndpoint'),
    spotRouterEndpoint: requiredString(values, 'spotRouterEndpoint'),
    spotPubEndpoint: requiredString(values, 'spotPubEndpoint'),
    clientSpotPubEndpoints: stringList(values, 'clientSpotPubEndpoints'),
    playAExternalSpotEndpoint: optionalString(values, 'playAExternalSpotEndpoint'),
    externalClientEndpoint: optionalString(values, 'externalClientEndpoint'),
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    evidenceFile: optionalString(values, 'evidenceFile'),
    logDir: requiredString(values, 'logDir')
  };
}
import { objectValues, optionalString, requiredString, stringList } from '../../../configuration';
