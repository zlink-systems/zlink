export interface SessionOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly controlRouterEndpoint: string;
  readonly playControlEndpoints: readonly string[];
  readonly spotRouteEndpoint: string;
  readonly spotRouterEndpoint: string;
  readonly spotRouterPeers: readonly { rid: string; endpoint: string }[];
  readonly playSpotRouteEndpoints: readonly string[];
  readonly streamEndpoint: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly evidenceFile?: string;
  readonly logDir: string;
}

export const SESSION_OPTIONS = Symbol.for('AUTOMATIC_TURN_SESSION_OPTIONS');

export function validateSessionOptions(value: unknown): SessionOptions {
  const values = objectValue(value);
  return {
    rid: requiredString(values, 'rid'),
    httpUrl: requiredString(values, 'httpUrl'),
    controlRouterEndpoint: requiredString(values, 'controlRouterEndpoint'),
    playControlEndpoints: stringList(values, 'playControlEndpoints', true),
    spotRouteEndpoint: requiredString(values, 'spotRouteEndpoint'),
    spotRouterEndpoint: requiredString(values, 'spotRouterEndpoint'),
    spotRouterPeers: peerList(values, 'spotRouterPeers'),
    playSpotRouteEndpoints: stringList(values, 'playSpotRouteEndpoints', true),
    streamEndpoint: requiredString(values, 'streamEndpoint'),
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    evidenceFile: optionalString(values, 'evidenceFile'),
    logDir: requiredString(values, 'logDir')
  };
}
import { objectValue, optionalString, peerList, requiredString, stringList } from '../../../configuration';
