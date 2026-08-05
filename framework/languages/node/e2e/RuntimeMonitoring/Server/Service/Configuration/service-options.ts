import { objectValues, requiredString } from '../../../configuration';

export interface ServiceOptions {
  readonly rid: string; readonly httpUrl: string; readonly redisEndpoint: string; readonly redisKeyPrefix: string;
  readonly channelEndpoint: string; readonly spotRouterEndpoint: string; readonly spotPubEndpoint: string;
  readonly socketFilter: boolean; readonly throwMonitor: boolean; readonly evidenceFile?: string; readonly logDir: string;
}
export interface ServiceRoleOptions {
  readonly socketFilter?: boolean;
  readonly throwMonitor?: boolean;
  readonly profileServer?: boolean;
}
export function validateServiceOptions(value: unknown, role: ServiceRoleOptions = {}): ServiceOptions {
  const values = objectValues(value); const evidenceFile = values.evidenceFile;
  return {
    rid: requiredString(values, 'rid'), httpUrl: requiredString(values, 'httpUrl'), redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'), channelEndpoint: requiredString(values, 'channelEndpoint'),
    spotRouterEndpoint: requiredString(values, 'spotRouterEndpoint'), spotPubEndpoint: requiredString(values, 'spotPubEndpoint'),
    socketFilter: role.socketFilter === true, throwMonitor: role.throwMonitor === true,
    evidenceFile: typeof evidenceFile === 'string' && evidenceFile.length > 0 ? evidenceFile : undefined,
    logDir: requiredString(values, 'logDir')
  };
}
