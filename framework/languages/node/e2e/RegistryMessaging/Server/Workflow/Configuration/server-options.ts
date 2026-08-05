export interface ServerOptions {
  readonly role: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly evidenceFile?: string;
  readonly rid: string;
  readonly redisEndpoint?: string;
  readonly redisKeyPrefix?: string;
  readonly workflowEndpoint: string;
}

export function validateServerOptions(value: unknown, defaultRole = 'workflow'): ServerOptions {
  const values = objectValues(value);
  const rid = optionalString(values, 'rid') ?? 'workflow';
  const workflowEndpoint = optionalString(values, 'workflowEndpoint');
  if (workflowEndpoint === undefined || workflowEndpoint.length === 0) {
    throw new Error('--workflow-endpoint is required.');
  }
  return {
    role: defaultRole,
    httpUrl: optionalString(values, 'httpUrl') ?? 'http://127.0.0.1:0',
    logDir: optionalString(values, 'logDir') ?? '/tmp/zlink-node-e2e-log',
    evidenceFile: optionalString(values, 'evidenceFile'),
    rid,
    redisEndpoint: optionalString(values, 'redisEndpoint'),
    redisKeyPrefix: optionalString(values, 'redisKeyPrefix'),
    workflowEndpoint
  };
}
import { objectValues, optionalString } from '../../../configuration';
