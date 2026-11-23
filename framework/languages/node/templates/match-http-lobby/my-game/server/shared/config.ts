import { readFileSync } from 'node:fs';
export const NODE_CONFIG = Symbol('MyGame.NodeConfig');
export interface NodeConfig {
  id: string;
  title?: string;
  logDir?: string;
  redisEndpoint: string;
  keyPrefix: string;
  meshEndpoint: string;
  streamEndpoint: string | null;
  httpEndpoint?: string;
  sessionEndpoint?: string;
  roomCapacity?: number;
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
  if (value.title !== undefined && (typeof value.title !== 'string' || !value.title))
    throw new Error('Invalid title');
  if (value.logDir !== undefined && (typeof value.logDir !== 'string' || !value.logDir))
    throw new Error('Invalid logDir');
  return value;
}
