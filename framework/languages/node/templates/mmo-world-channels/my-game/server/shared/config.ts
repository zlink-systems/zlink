import { readFileSync } from 'node:fs';
import { channelOf } from './constants';
import type { WorldInfo } from './messages';
export const NODE_CONFIG = Symbol('MyGame.NodeConfig');
export interface NodeConfig {
  id: string;
  redisEndpoint: string;
  keyPrefix: string;
  meshEndpoint: string;
  streamEndpoint: string | null;
  worldId: string;
  zoneIds: string[];
  zoneCapacity: number;
  httpEndpoint?: string;
  worlds?: WorldInfo[];
}
export function readConfig(path: string): NodeConfig {
  const value = JSON.parse(readFileSync(path, 'utf8')) as NodeConfig;
  for (const key of ['id', 'redisEndpoint', 'keyPrefix', 'meshEndpoint', 'worldId'] as const)
    if (typeof value[key] !== 'string' || !value[key]) throw new Error(`Invalid config ${key}`);
  if (
    value.streamEndpoint !== null &&
    (typeof value.streamEndpoint !== 'string' || !value.streamEndpoint)
  )
    throw new Error('Invalid streamEndpoint');
  if (!Array.isArray(value.zoneIds) || new Set(value.zoneIds).size !== value.zoneIds.length)
    throw new Error('Invalid Zone IDs');
  value.zoneIds.forEach(channelOf);
  if (!Number.isSafeInteger(value.zoneCapacity) || value.zoneCapacity < 1)
    throw new Error('Invalid Zone capacity');
  return value;
}
