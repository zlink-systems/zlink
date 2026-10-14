import { readFileSync } from 'node:fs';
export const NODE_CONFIG = Symbol('MyGame.NodeConfig');
export interface NodeConfig {
  id: string;
  redisEndpoint: string;
  keyPrefix: string;
  meshEndpoint: string;
  streamEndpoint: string | null;
  capacity: number;
  regionLimit: number;
}
export function readConfig(path: string): NodeConfig {
  const value = JSON.parse(readFileSync(path, 'utf8')) as NodeConfig;
  for (const key of ['id', 'redisEndpoint', 'keyPrefix', 'meshEndpoint'] as const)
    if (typeof value[key] !== 'string' || !value[key].trim())
      throw new Error(`Invalid config ${key}`);
  if (
    value.streamEndpoint !== null &&
    (typeof value.streamEndpoint !== 'string' || !value.streamEndpoint)
  )
    throw new Error('Invalid streamEndpoint');
  for (const key of ['capacity', 'regionLimit'] as const)
    if (!Number.isSafeInteger(value[key]) || value[key] < 1)
      throw new Error(`Invalid config ${key}`);
  return value;
}
