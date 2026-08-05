export interface PlayOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly controlEndpoint: string;
  readonly spotRouteEndpoint: string;
  readonly peerSpotRouteEndpoints: readonly string[];
  readonly spotRouterEndpoint: string;
  readonly spotPubEndpoint: string;
  readonly spotRouterPeers: readonly { rid: string; endpoint: string }[];
  readonly delayEndpoint: string;
  readonly externalApiUrl: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly evidenceFile?: string;
  readonly logDir: string;
}

export const PLAY_OPTIONS = Symbol.for('AUTOMATIC_TURN_PLAY_OPTIONS');

export function validatePlayOptions(value: unknown): PlayOptions {
  const values = objectValue(value);
  return {
    rid: requiredString(values, 'rid'),
    httpUrl: requiredString(values, 'httpUrl'),
    controlEndpoint: requiredString(values, 'controlEndpoint'),
    spotRouteEndpoint: requiredString(values, 'spotRouteEndpoint'),
    peerSpotRouteEndpoints: stringList(values, 'peerSpotRouteEndpoints', false),
    spotRouterEndpoint: requiredString(values, 'spotRouterEndpoint'),
    spotPubEndpoint: requiredString(values, 'spotPubEndpoint'),
    spotRouterPeers: peerList(values, 'spotRouterPeers'),
    delayEndpoint: requiredString(values, 'delayEndpoint'),
    externalApiUrl: requiredString(values, 'externalApiUrl'),
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    evidenceFile: optionalString(values, 'evidenceFile'),
    logDir: requiredString(values, 'logDir')
  };
}
import { objectValue, optionalString, peerList, requiredString, stringList } from '../../../configuration';
