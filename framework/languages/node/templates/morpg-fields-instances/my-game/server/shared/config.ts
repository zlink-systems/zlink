import { readFileSync } from 'node:fs';
export const NODE_CONFIG = Symbol('MyGame.NodeConfig');
export interface NodeConfig {
  id: string;
  redisEndpoint: string;
  keyPrefix: string;
  meshEndpoint: string;
  streamEndpoint: string | null;
  fieldIds: string[];
  fieldCapacity: number;
  dungeonCapacity: number;
}
export function readConfig(path: string): NodeConfig {
  const value = JSON.parse(readFileSync(path, 'utf8')) as NodeConfig;
  for (const key of ['id', 'redisEndpoint', 'keyPrefix', 'meshEndpoint'] as const)
    if (typeof value[key] !== 'string' || !value[key]) throw new Error(`Invalid config ${key}`);
  if (
    value.streamEndpoint !== null &&
    (typeof value.streamEndpoint !== 'string' || !value.streamEndpoint)
  )
    throw new Error('Invalid streamEndpoint');
  if (!Array.isArray(value.fieldIds) || value.fieldIds.some((id) => typeof id !== 'string' || !id))
    throw new Error('Invalid fieldIds');
  for (const key of ['fieldCapacity', 'dungeonCapacity'] as const)
    if (!Number.isSafeInteger(value[key]) || value[key] < 0) throw new Error(`Invalid ${key}`);
  return value;
}
