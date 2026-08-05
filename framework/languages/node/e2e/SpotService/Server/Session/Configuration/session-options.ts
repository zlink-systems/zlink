export interface SessionOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly controlRouterEndpoint: string;
  readonly playControlEndpoints: ReadonlyMap<string, string>;
  readonly spotRouterEndpoint: string;
  readonly playSpotRouterEndpoints: ReadonlyMap<string, string>;
  readonly alternateObjectRouterEndpoint: string;
  readonly playAlternateObjectRouterEndpoints: ReadonlyMap<string, string>;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly streamEndpoint: string;
  readonly tlsStreamEndpoint?: string;
  readonly tlsCertPath?: string;
  readonly tlsKeyPath?: string;
  readonly evidenceFile?: string;
  readonly logDir: string;
}

export function validateSessionOptions(value: unknown): SessionOptions {
  const values = objectValues(value);
  const parsePeers = (name: string): ReadonlyMap<string, string> => {
    const peerMap = new Map<string, string>();
    for (const peer of stringList(values, name)) {
      const separator = peer.indexOf('=');
      if (separator <= 0 || separator === peer.length - 1) {
        throw new Error(`Configuration value 'e2e.${name}' entries must use '<rid>=<endpoint>'.`);
      }
      peerMap.set(peer.slice(0, separator), peer.slice(separator + 1));
    }
    if (peerMap.size === 0) {
      throw new Error(`Configuration value 'e2e.${name}' must contain at least one peer.`);
    }
    return peerMap;
  };
  const peerMap = parsePeers('playSpotRouterEndpoints');
  const alternateObjectPeerMap = parsePeers('playAlternateObjectRouterEndpoints');
  const controlPeerMap = parsePeers('playControlEndpoints');
  return {
    rid: requiredString(values, 'rid'),
    httpUrl: requiredString(values, 'httpUrl'),
    controlRouterEndpoint: requiredString(values, 'controlRouterEndpoint'),
    playControlEndpoints: controlPeerMap,
    spotRouterEndpoint: requiredString(values, 'spotRouterEndpoint'),
    playSpotRouterEndpoints: peerMap,
    alternateObjectRouterEndpoint: requiredString(values, 'alternateObjectRouterEndpoint'),
    playAlternateObjectRouterEndpoints: alternateObjectPeerMap,
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    streamEndpoint: requiredString(values, 'streamEndpoint'),
    tlsStreamEndpoint: optionalString(values, 'tlsStreamEndpoint'),
    tlsCertPath: optionalString(values, 'tlsCertPath'),
    tlsKeyPath: optionalString(values, 'tlsKeyPath'),
    evidenceFile: optionalString(values, 'evidenceFile'),
    logDir: requiredString(values, 'logDir')
  };
}
import { objectValues, optionalString, requiredString, stringList } from '../../../configuration';
