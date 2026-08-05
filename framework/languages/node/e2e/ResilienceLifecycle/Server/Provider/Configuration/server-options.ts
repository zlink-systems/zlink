import { objectValues, optionalString } from '../../../configuration';

export interface ServerOptions {
  readonly role: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly evidenceFile?: string;
  readonly rid: string;
  readonly redisEndpoint?: string;
  readonly redisKeyPrefix?: string;
  readonly channelEndpoint?: string;
  readonly fanoutEndpoint?: string;
}

export function validateServerOptions(value: unknown, defaultRole = 'provider'): ServerOptions {
  const values = objectValues(value);
  return {
    role: defaultRole,
    httpUrl: optionalString(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optionalString(values, 'logDir') ?? '/tmp/zlink-node-e2e-log',
    evidenceFile: optionalString(values, 'evidenceFile'),
    rid: optionalString(values, 'rid') ?? 'node',
    redisEndpoint: optionalString(values, 'redisEndpoint'),
    redisKeyPrefix: optionalString(values, 'redisKeyPrefix'),
    channelEndpoint: optionalString(values, 'channelEndpoint'),
    fanoutEndpoint: optionalString(values, 'fanoutEndpoint')
  };
}
