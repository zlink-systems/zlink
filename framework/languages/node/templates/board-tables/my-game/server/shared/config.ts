import { readFileSync } from 'node:fs';
export const NODE_CONFIG = Symbol('MyGame.NodeConfig');
export interface NodeConfig {
  id: string;
  redisEndpoint: string;
  keyPrefix: string;
  meshEndpoint: string | null;
  apiBindHost?: string;
  apiAdvertiseHost?: string;
  streamEndpoint: string | null;
  tableCapacity?: number;
}
export function readConfig(path: string): NodeConfig {
  const value = JSON.parse(readFileSync(path, 'utf8')) as NodeConfig;
  for (const key of ['id', 'redisEndpoint', 'keyPrefix'] as const) {
    if (typeof value[key] !== 'string' || !value[key]) throw new Error(`Invalid config ${key}`);
  }
  for (const key of ['meshEndpoint', 'streamEndpoint'] as const) {
    if (value[key] !== null && (typeof value[key] !== 'string' || !value[key]))
      throw new Error(`Invalid config ${key}`);
  }
  for (const key of ['apiBindHost', 'apiAdvertiseHost'] as const) {
    if (value[key] !== undefined && (typeof value[key] !== 'string' || !value[key]))
      throw new Error(`Invalid config ${key}`);
  }
  if (
    value.tableCapacity !== undefined &&
    (!Number.isInteger(value.tableCapacity) || value.tableCapacity < 0)
  )
    throw new Error('Table capacity must be a nonnegative integer');
  return value;
}
