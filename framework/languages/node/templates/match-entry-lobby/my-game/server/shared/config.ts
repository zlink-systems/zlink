import { readFileSync } from 'node:fs';
export const NODE_CONFIG = Symbol('MyGame.NodeConfig');
export interface NodeConfig {
  id: string;
  redisEndpoint: string;
  keyPrefix: string;
  meshEndpoint: string;
  streamEndpoint: string | null;
  bootstrapMatchmaking: boolean;
  roomCapacity: number;
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
  if (typeof value.bootstrapMatchmaking !== 'boolean')
    throw new Error('Invalid bootstrapMatchmaking');
  if (!Number.isSafeInteger(value.roomCapacity) || value.roomCapacity < 1)
    throw new Error('Invalid roomCapacity');
  return value;
}
