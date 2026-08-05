import { objectValues, optionalString } from '../../../configuration';

export interface ObjectClientOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly routeEndpoint: string;
  readonly routePeers: readonly string[];
  readonly routePeerRids: readonly string[];
  readonly serverWeight?: number;
}

export function validateObjectClientOptions(value: unknown): ObjectClientOptions {
  const values = objectValues(value);
  const routePeers = Array.isArray(values.routePeers)
    ? values.routePeers.filter((entry): entry is string =>
      typeof entry === 'string' && entry.length > 0)
    : [];
  const routePeerRids = Array.isArray(values.routePeerRids)
    ? values.routePeerRids.filter((entry): entry is string =>
      typeof entry === 'string' && entry.length > 0)
    : [];
  if (routePeers.length !== routePeerRids.length) {
    throw new Error('routePeers and routePeerRids must contain the same number of entries.');
  }
  const serverWeight = values.serverWeight === undefined
    ? undefined
    : Number(values.serverWeight);
  if (serverWeight !== undefined
    && (!Number.isInteger(serverWeight) || serverWeight < 0 || serverWeight > 100)) {
    throw new Error('serverWeight must be an integer between 0 and 100.');
  }
  return {
    rid: requireString(values, 'rid'),
    httpUrl: requireString(values, 'httpUrl'),
    logDir: optionalString(values, 'logDir') ?? '/tmp/zlink-node-rm-a3',
    redisEndpoint: requireString(values, 'redisEndpoint'),
    redisKeyPrefix: requireString(values, 'redisKeyPrefix'),
    routeEndpoint: requireString(values, 'routeEndpoint'),
    routePeers,
    routePeerRids,
    serverWeight
  };
}

function requireString(values: Record<string, unknown>, key: string): string {
  const value = optionalString(values, key);
  if (value === undefined) {
    throw new Error(`${key} is required.`);
  }
  return value;
}
